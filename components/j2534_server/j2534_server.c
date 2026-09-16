/*
 * This file is part of the MeatPi components project.
 *
 * Copyright (C) 2022-2026 MeatPi Electronics.
 * Written by Ali Slim <ali@meatpi.com>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file j2534_server.c
 * @brief J2534 PassThru server — lifecycle, the TCP transport, the
 *        session/channel state machine, and the data ops wired to
 *        j2534_channel (CAN via can_manager, ISO15765 via the
 *        registered ISO-TP provider).
 *        WRITE_MSGS / filters / periodics / ioctl + an RX pump that
 *        pushes RX_MSG frames. Settings live in j2534_server_settings.c.
 *        See TASK_j2534_server.md.
 */
#include "j2534_server.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "obd_gate.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "esp_netif.h"

#include "log_manager.h"

#include "j2534_channel.h"
#include "j2534_proto.h"
#include "j2534_server_private.h"

static const char *TAG = "j2534_server";

#define J2534_LISTEN_BACKLOG 1
#define J2534_RX_CAP         (J2534_MAX_DATA + 256)
#define J2534_TASK_STACK     4096   /* PSRAM: no flash from this task */
#define J2534_TASK_PRIO      5

/* ---- channel state ---------------------------------------------------- */

static struct {
    bool device_open;
    bool ch_used[J2534_MAX_CHANNELS];
} s_session;

/* periodic messages (esp_timer -> j2534_channel_write) */
#define J2534_MAX_PERIODIC 4
typedef struct {
    bool                in_use;
    uint32_t            id;
    int                 slot;      /* channel slot */
    esp_timer_handle_t  timer;
    j2534_msg_t         msg;
} periodic_t;
/* PSRAM: each slot embeds a 4 KB j2534_msg_t — must not sit in internal
 * BSS (starves WiFi; §2 memory budget). */
EXT_RAM_BSS_ATTR static periodic_t s_periodic[J2534_MAX_PERIODIC];
static uint32_t s_next_periodic_id = 1;

/* big scratch buffers — PSRAM (single tester, one task each owns them).
 * j2534_msg_t is ~4 KB; NEVER put one on a task stack (the listener/RX
 * tasks have 4 KB stacks — three on the stack overflowed and reset the
 * device, caught 2026-07-07 via the DLL's FLOW_CONTROL filter). */
EXT_RAM_BSS_ATTR static uint8_t s_req_payload[J2534_RX_CAP];
EXT_RAM_BSS_ATTR static uint8_t s_rx_wire[J2534_MAX_DATA + 32];
EXT_RAM_BSS_ATTR static j2534_msg_t s_rx_msg;   /* RX pump task only */
/* request-handler scratch (listener task only, one request at a time) */
EXT_RAM_BSS_ATTR static j2534_msg_t s_h_mask, s_h_pattern, s_h_fc, s_h_wmsg;

/* ---- module state ----------------------------------------------------- */

static bool s_started;

static TaskHandle_t s_task;
static StaticTask_t s_task_tcb;
EXT_RAM_BSS_ATTR static StackType_t s_task_stack[J2534_TASK_STACK];
static TaskHandle_t s_rx_task;
static StaticTask_t s_rx_tcb;
EXT_RAM_BSS_ATTR static StackType_t s_rx_stack[J2534_TASK_STACK];
static volatile bool s_run;

/* the active tester socket (-1 = none) + a lock so the RX pump and the
 * request handler don't interleave on the wire */
static volatile int s_client_sock = -1;
static SemaphoreHandle_t s_send_lock;
static StaticSemaphore_t s_send_lock_buf;
/* held by whichever transport currently owns the single tester session */
static SemaphoreHandle_t s_session_lock;
static StaticSemaphore_t s_session_lock_buf;

static j2534_server_status_t s_status;

/* the "exclusive" option: while a tester is attached, hold obd_gate's
 * diagnostics hold so autopid (PID polling + DTC scans) stays off the bus
 * and cannot interleave with the tool's conversations; runtime-
 * switchable, boot default = the setting */
static const int s_diag_token;
static volatile bool s_exclusive;

static void diag_hold(bool on)
{
    obd_gate_diag_hold(&s_diag_token, on);

    if (on)
    {
        /* let the poller get off the bus (it loops within 500 ms) before
           the tester's first frame */
        (void)obd_gate_diag_wait_ack(700);
    }
}

void j2534_server_set_exclusive(bool on)
{
    s_exclusive = on;
    diag_hold(on && s_status.client_connected);
}

bool j2534_server_exclusive(void)
{
    return s_exclusive;
}

/* ---- helpers ---------------------------------------------------------- */

/* True if @p addr is the current IP of the netif with @p ifkey. */
static bool addr_is_ifkey(uint32_t addr, const char *ifkey)
{
    esp_netif_t *n = esp_netif_get_handle_from_ifkey(ifkey);
    esp_netif_ip_info_t ip;

    return n != NULL &&
           esp_netif_get_ip_info(n, &ip) == ESP_OK &&
           ip.ip.addr == addr;
}

/* Interface-exposure gate: is a connection that landed on local address
 * @p local_addr allowed? With allow_lan off we accept only connections
 * that arrived on WiCAN's own SoftAP or the USB-device (NCM) netif — the
 * two interfaces a user is physically at. STA / USB-Ethernet uplinks (a
 * shared LAN) are refused so the unauthenticated port isn't reachable
 * across a network. Loopback is always allowed (local diagnostics). */
static bool local_iface_allowed(uint32_t local_addr)
{
    if (j2534_settings_config()->allow_lan)
    {
        return true;
    }

    if (local_addr == htonl(INADDR_LOOPBACK))
    {
        return true;
    }

    /* WIFI_AP_DEF / "USBND" are the ifkeys of the SoftAP and the
     * usb_net_device NCM netif (see usb_net_device.c base_cfg.if_key). */
    return addr_is_ifkey(local_addr, "WIFI_AP_DEF") ||
           addr_is_ifkey(local_addr, "USBND");
}

/* Transport indirection: the same framed wire protocol runs over TCP
 * (a socket fd) or over a CDC-ACM serial link. The dispatch code threads
 * an `int fd`; a value of J2534_FD_SERIAL routes I/O through the serial
 * vtable (registered by the CDC-device transport, device_class=cdc)
 * instead of lwIP recv/send. Only these two functions branch — every
 * handle_frame/send_ack call site is transport-agnostic. */
#define J2534_FD_SERIAL (-2)

static const j2534_serial_transport_t *s_serial;   /* NULL until registered */

void j2534_server_set_serial_transport(const j2534_serial_transport_t *t)
{
    s_serial = t;
}

static bool read_exact(int fd, uint8_t *buf, size_t n)
{
    size_t got = 0;

    while (got < n && s_run)
    {
        int r;

        if (fd == J2534_FD_SERIAL)
        {
            r = (s_serial != NULL)
                    ? s_serial->read(buf + got, n - got, 200)
                    : -1;
            if (r == 0)
            {
                continue; /* serial read timed out; keep waiting for a frame */
            }
            if (r < 0)
            {
                return false;
            }
        }
        else
        {
            r = recv(fd, buf + got, n - got, 0);
            if (r <= 0)
            {
                return false; /* 0 = peer closed the socket */
            }
        }

        got += (size_t)r;
    }

    return got == n;
}

static bool send_frame(int fd, uint8_t type, uint16_t seq,
                       uint16_t channel, const uint8_t *payload,
                       uint32_t len)
{
    uint8_t hdr[J2534_HDR_SIZE];
    bool ok;

    j2534_hdr_encode(hdr, type, seq, channel, len);

    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    if (fd == J2534_FD_SERIAL)
    {
        ok = s_serial != NULL &&
             s_serial->write(hdr, sizeof(hdr)) == (int)sizeof(hdr) &&
             (len == 0 || s_serial->write(payload, len) == (int)len);
    }
    else
    {
        ok = (send(fd, hdr, sizeof(hdr), 0) == (int)sizeof(hdr)) &&
             (len == 0 || send(fd, payload, len, 0) == (int)len);
    }
    if (ok)
    {
        s_status.frames_tx++;
    }
    xSemaphoreGive(s_send_lock);
    return ok;
}

/* ACK = status u32 + optional result u32 (result_len 0 or 4). */
static bool send_ack(int sock, uint16_t seq, uint16_t channel,
                     uint32_t status, const uint32_t *result)
{
    uint8_t p[8];

    p[0] = (uint8_t)status;
    p[1] = (uint8_t)(status >> 8);
    p[2] = (uint8_t)(status >> 16);
    p[3] = (uint8_t)(status >> 24);

    uint32_t len = 4;

    if (result != NULL)
    {
        p[4] = (uint8_t)*result;
        p[5] = (uint8_t)(*result >> 8);
        p[6] = (uint8_t)(*result >> 16);
        p[7] = (uint8_t)(*result >> 24);
        len = 8;
    }

    return send_frame(sock, J2534_MT_ACK, seq, channel, p, len);
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- periodic messages (esp_timer -> channel write) ------------------- */

static void periodic_cb(void *arg)
{
    periodic_t *pm = arg;
    if (pm->in_use && j2534_channel_active(pm->slot))
    {
        (void)j2534_channel_write(pm->slot, &pm->msg);
    }
}

static void periodic_clear_all(void)
{
    for (int i = 0; i < J2534_MAX_PERIODIC; i++)
    {
        if (s_periodic[i].in_use)
        {
            esp_timer_stop(s_periodic[i].timer);
            esp_timer_delete(s_periodic[i].timer);
            s_periodic[i].in_use = false;
        }
    }
}

/* ---- request dispatch ------------------------------------------------- */

static void handle_frame(int sock, const j2534_hdr_t *h,
                         const uint8_t *payload)
{
    switch (h->type)
    {
        case J2534_MT_HELLO:
        {
            /* reply with our wire version so the DLL can gate features */
            uint32_t ver = J2534_WIRE_VERSION;
            send_ack(sock, h->seq, 0, J2534_STATUS_NOERROR, &ver);
            break;
        }

        case J2534_MT_OPEN:
        {
            uint32_t dev_id = 1;
            s_session.device_open = true;
            s_status.device_open = true;
            send_ack(sock, h->seq, 0, J2534_STATUS_NOERROR, &dev_id);
            break;
        }

        case J2534_MT_CLOSE:
            periodic_clear_all();
            j2534_channel_reset_all();
            memset(&s_session, 0, sizeof(s_session));
            s_status.device_open = false;
            s_status.channel_count = 0;
            send_ack(sock, h->seq, 0, J2534_STATUS_NOERROR, NULL);
            break;

        case J2534_MT_CONNECT:
        {
            /* payload: proto u32, flags u32, baud u32, [tx_id u32, rx_id
             * u32] (tx/rx optional — ISO15765 may instead bind via a
             * FLOW_CONTROL filter) */
            if (!s_session.device_open || h->length < 12)
            {
                send_ack(sock, h->seq, 0,
                         J2534_ERR_DEVICE_NOT_CONNECTED, NULL);
                break;
            }

            uint32_t proto = rd_u32(payload);
            uint32_t flags = rd_u32(payload + 4);
            uint32_t tx_id = (h->length >= 20) ? rd_u32(payload + 12) : 0;
            uint32_t rx_id = (h->length >= 20) ? rd_u32(payload + 16) : 0;
            bool ext = (flags & J2534_TX_CAN_29BIT_ID) != 0;

            int slot = -1;
            for (int i = 0; i < J2534_MAX_CHANNELS; i++)
            {
                if (!s_session.ch_used[i]) { slot = i; break; }
            }
            if (slot < 0)
            {
                send_ack(sock, h->seq, 0, J2534_ERR_FAILED, NULL);
                break;
            }

            uint32_t st = j2534_channel_connect(slot, proto, flags,
                                                tx_id, rx_id, ext);
            if (st != J2534_STATUS_NOERROR)
            {
                send_ack(sock, h->seq, 0, st, NULL);
                break;
            }

            s_session.ch_used[slot] = true;
            s_status.channel_count++;
            uint32_t ch_id = (uint32_t)slot + 1;
            ESP_LOGI(TAG, "CONNECT proto=%lu -> channel %lu",
                     (unsigned long)proto, (unsigned long)ch_id);
            send_ack(sock, h->seq, (uint16_t)ch_id,
                     J2534_STATUS_NOERROR, &ch_id);
            break;
        }

        case J2534_MT_DISCONNECT:
        {
            int slot = (int)h->channel - 1;
            if (slot >= 0 && slot < J2534_MAX_CHANNELS &&
                s_session.ch_used[slot])
            {
                j2534_channel_disconnect(slot);
                s_session.ch_used[slot] = false;
                if (s_status.channel_count > 0) s_status.channel_count--;
                send_ack(sock, h->seq, h->channel,
                         J2534_STATUS_NOERROR, NULL);
            }
            else
            {
                send_ack(sock, h->seq, h->channel,
                         J2534_ERR_INVALID_CHANNEL_ID, NULL);
            }
            break;
        }

        case J2534_MT_WRITE_MSGS:
        {
            int slot = (int)h->channel - 1;
            if (h->length < 4 + 24)
            {
                send_ack(sock, h->seq, h->channel, J2534_ERR_INVALID_MSG,
                         NULL);
                break;
            }
            uint32_t count = rd_u32(payload);
            const uint8_t *p = payload + 4;
            size_t left = h->length - 4;
            uint32_t st = J2534_STATUS_NOERROR;
            for (uint32_t i = 0; i < count && st == J2534_STATUS_NOERROR; i++)
            {
                if (!j2534_msg_decode(p, left, &s_h_wmsg))
                {
                    st = J2534_ERR_INVALID_MSG;
                    break;
                }
                size_t consumed = 24 + s_h_wmsg.data_size;
                p += consumed; left -= consumed;
                st = j2534_channel_write(slot, &s_h_wmsg);
            }
            send_ack(sock, h->seq, h->channel, st, NULL);
            break;
        }

        case J2534_MT_START_FILTER:
        {
            int slot = (int)h->channel - 1;
            /* payload: type u32, then mask, pattern, [flow_control] MSGs */
            if (h->length < 4 + 24)
            {
                send_ack(sock, h->seq, h->channel, J2534_ERR_INVALID_MSG,
                         NULL);
                break;
            }
            uint32_t ftype = rd_u32(payload);
            const uint8_t *p = payload + 4;
            size_t left = h->length - 4;
            bool ok = j2534_msg_decode(p, left, &s_h_mask);
            if (ok) { size_t c = 24 + s_h_mask.data_size; p += c; left -= c;
                      ok = j2534_msg_decode(p, left, &s_h_pattern); }
            bool has_fc = false;
            if (ok) { size_t c = 24 + s_h_pattern.data_size; p += c; left -= c;
                      has_fc = j2534_msg_decode(p, left, &s_h_fc); }
            if (!ok)
            {
                send_ack(sock, h->seq, h->channel, J2534_ERR_INVALID_MSG,
                         NULL);
                break;
            }
            uint32_t fid = 0;
            uint32_t st = j2534_channel_add_filter(
                slot, ftype, &s_h_mask, &s_h_pattern,
                has_fc ? &s_h_fc : NULL, &fid);
            send_ack(sock, h->seq, h->channel, st,
                     st == J2534_STATUS_NOERROR ? &fid : NULL);
            break;
        }

        case J2534_MT_STOP_FILTER:
        {
            int slot = (int)h->channel - 1;
            uint32_t st = (h->length >= 4)
                ? j2534_channel_stop_filter(slot, rd_u32(payload))
                : J2534_ERR_INVALID_FILTER_ID;
            send_ack(sock, h->seq, h->channel, st, NULL);
            break;
        }

        case J2534_MT_START_PERIODIC:
        {
            int slot = (int)h->channel - 1;
            /* payload: interval_ms u32, then one PASSTHRU_MSG */
            if (h->length < 4 + 24 || !j2534_channel_active(slot) ||
                !j2534_msg_decode(payload + 4, h->length - 4, &s_h_wmsg))
            {
                send_ack(sock, h->seq, h->channel, J2534_ERR_INVALID_MSG,
                         NULL);
                break;
            }
            int pi = -1;
            for (int i = 0; i < J2534_MAX_PERIODIC; i++)
                if (!s_periodic[i].in_use) { pi = i; break; }
            if (pi < 0)
            {
                send_ack(sock, h->seq, h->channel, J2534_ERR_FAILED,
                         NULL);
                break;
            }
            uint32_t interval = rd_u32(payload);
            if (interval < 5) interval = 5;
            periodic_t *pm = &s_periodic[pi];
            pm->slot = slot; pm->msg = s_h_wmsg; pm->id = s_next_periodic_id++;
            esp_timer_create_args_t ta = {
                .callback = periodic_cb, .arg = pm,
                .name = "j2534_per" };
            if (esp_timer_create(&ta, &pm->timer) != ESP_OK ||
                esp_timer_start_periodic(pm->timer,
                                         (uint64_t)interval * 1000) != ESP_OK)
            {
                send_ack(sock, h->seq, h->channel, J2534_ERR_FAILED, NULL);
                break;
            }
            pm->in_use = true;
            send_ack(sock, h->seq, h->channel, J2534_STATUS_NOERROR, &pm->id);
            break;
        }

        case J2534_MT_STOP_PERIODIC:
        {
            uint32_t pid = (h->length >= 4) ? rd_u32(payload) : 0;
            uint32_t st = J2534_ERR_INVALID_MSG;
            for (int i = 0; i < J2534_MAX_PERIODIC; i++)
            {
                if (s_periodic[i].in_use && s_periodic[i].id == pid)
                {
                    esp_timer_stop(s_periodic[i].timer);
                    esp_timer_delete(s_periodic[i].timer);
                    s_periodic[i].in_use = false;
                    st = J2534_STATUS_NOERROR;
                    break;
                }
            }
            send_ack(sock, h->seq, h->channel, st, NULL);
            break;
        }

        case J2534_MT_IOCTL:
        {
            int slot = (int)h->channel - 1;
            uint32_t ioctl_id = (h->length >= 4) ? rd_u32(payload) : 0;
            /* v1: config is best-effort (bus params come from can_manager
             * settings); CLEAR_* operate on our channel state */
            switch (ioctl_id)
            {
                case J2534_IOCTL_CLEAR_MSG_FILTERS:
                    j2534_channel_clear_filters(slot);
                    break;
                case J2534_IOCTL_CLEAR_PERIODIC_MSGS:
                    periodic_clear_all();
                    break;
                default: break; /* GET/SET_CONFIG, CLEAR_*_BUFFER: ack OK */
            }
            send_ack(sock, h->seq, h->channel, J2534_STATUS_NOERROR, NULL);
            break;
        }

        default:
            send_ack(sock, h->seq, h->channel, J2534_ERR_FAILED, NULL);
            break;
    }
}

/* ---- one client session ----------------------------------------------- */

static void serve_client(int sock)
{
    uint8_t *payload = s_req_payload; /* PSRAM; single client, one task */

    /* single-tester across all transports: if TCP and serial race, the
     * second one in is refused rather than corrupting the shared session */
    if (xSemaphoreTake(s_session_lock, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "tester already attached on another transport; "
                 "refusing this one");
        return;
    }

    j2534_channel_reset_all();
    periodic_clear_all();
    memset(&s_session, 0, sizeof(s_session));
    s_status.client_connected = true;
    s_status.device_open = false;
    s_status.channel_count = 0;
    s_client_sock = sock;                 /* the RX pump may now push */
    ESP_LOGI(TAG, "tester connected");

    if (s_exclusive)
    {
        diag_hold(true);
    }

    while (s_run)
    {
        uint8_t hdr[J2534_HDR_SIZE];

        if (!read_exact(sock, hdr, sizeof(hdr)))
        {
            break;
        }

        j2534_hdr_t h;

        if (!j2534_hdr_decode(hdr, sizeof(hdr), &h) ||
            h.length > J2534_RX_CAP)
        {
            ESP_LOGW(TAG, "bad frame header; dropping tester");
            break;
        }

        if (h.length > 0 && !read_exact(sock, payload, h.length))
        {
            break;
        }

        s_status.frames_rx++;
        handle_frame(sock, &h, payload);
    }

    s_client_sock = -1;                   /* stop the RX pump pushing */
    periodic_clear_all();
    j2534_channel_reset_all();
    s_status.client_connected = false;
    s_status.device_open = false;
    s_status.channel_count = 0;
    diag_hold(false);
    ESP_LOGI(TAG, "tester disconnected");
    xSemaphoreGive(s_session_lock);
}

/* Serial-transport entry point (device_class=cdc). The CDC-device task
 * calls this once the host opens the port; it blocks for the session. */
void j2534_server_serve_serial(void)
{
    if (!s_run)
    {
        return; /* server disabled — nothing to serve */
    }
    serve_client(J2534_FD_SERIAL);
}

/* ---- RX pump: drain every active channel, push RX_MSG to the tester --- */

static void rx_pump_task(void *arg)
{
    (void)arg;

    while (s_run)
    {
        int sock = s_client_sock;
        if (sock == -1) /* -1 = idle; -2 (J2534_FD_SERIAL) is an active session */
        {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        bool any = false;
        for (int slot = 0; slot < J2534_MAX_CHANNELS; slot++)
        {
            if (!s_session.ch_used[slot])
            {
                continue;
            }

            if (j2534_channel_poll_rx(slot, &s_rx_msg))
            {
                size_t n = j2534_msg_encode(&s_rx_msg, s_rx_wire,
                                            sizeof(s_rx_wire));
                if (n > 0 && s_client_sock == sock)
                {
                    send_frame(sock, J2534_MT_RX_MSG, 0,
                               (uint16_t)(slot + 1), s_rx_wire,
                               (uint32_t)n);
                    any = true;
                }
            }
        }

        if (!any)
        {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }

    s_rx_task = NULL;
    vTaskDelete(NULL);
}

/* ---- listener task ---------------------------------------------------- */

static void listener_task(void *arg)
{
    (void)arg;

    const uint16_t port = j2534_settings_config()->port;
    int listen_fd = -1;

    while (s_run)
    {
        if (listen_fd < 0)
        {
            listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

            if (listen_fd < 0)
            {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }

            int opt = 1;
            setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt,
                       sizeof(opt));

            struct sockaddr_in addr = { 0 };
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
            addr.sin_port = htons(port);

            if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
                listen(listen_fd, J2534_LISTEN_BACKLOG) != 0)
            {
                ESP_LOGE(TAG, "bind/listen on :%u failed", port);
                close(listen_fd);
                listen_fd = -1;
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }

            s_status.listening = true;
            ESP_LOGI(TAG, "listening on tcp/%u", port);
        }

        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int cli = accept(listen_fd, (struct sockaddr *)&peer, &plen);

        if (cli < 0)
        {
            if (!s_run)
            {
                break;
            }
            continue;
        }

        /* interface-exposure gate (allow_lan): which of WiCAN's own IPs
         * did this connection land on? Refuse anything but AP / USB. */
        struct sockaddr_in local;
        socklen_t llen = sizeof(local);
        if (getsockname(cli, (struct sockaddr *)&local, &llen) != 0 ||
            !local_iface_allowed(local.sin_addr.s_addr))
        {
            ESP_LOGW(TAG, "refused connection on a non-AP/USB interface "
                     "(enable j2534_server.allow_lan to permit LAN access)");
            close(cli);
            continue;
        }

        int one = 1;
        setsockopt(cli, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        serve_client(cli);
        close(cli);
    }

    if (listen_fd >= 0)
    {
        close(listen_fd);
    }

    s_status.listening = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

/* ---- lifecycle -------------------------------------------------------- */

esp_err_t j2534_server_init(void)
{
    static const log_descriptor_t LOG_DESC = { "j2534_server", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);
    if (s_send_lock == NULL)
    {
        s_send_lock = xSemaphoreCreateMutexStatic(&s_send_lock_buf);
    }
    if (s_session_lock == NULL)
    {
        s_session_lock = xSemaphoreCreateMutexStatic(&s_session_lock_buf);
    }
    j2534_channel_init();
    return j2534_settings_register();
}

esp_err_t j2534_server_start(void)
{
    const j2534_config_t *cfg = j2534_settings_config();

    if (!j2534_settings_is_configured())
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_started)
    {
        return ESP_OK;
    }

    s_started = true;
    s_status.enabled = cfg->enabled;
    s_status.port = cfg->port;

    if (!cfg->enabled)
    {
        ESP_LOGI(TAG, "disabled by settings");
        return ESP_OK;
    }

    s_exclusive = j2534_settings_config()->exclusive;
    s_run = true;
    s_task = xTaskCreateStatic(listener_task, "j2534", J2534_TASK_STACK,
                               NULL, J2534_TASK_PRIO, s_task_stack,
                               &s_task_tcb);
    s_rx_task = xTaskCreateStatic(rx_pump_task, "j2534_rx",
                                  J2534_TASK_STACK, NULL, J2534_TASK_PRIO,
                                  s_rx_stack, &s_rx_tcb);

    ESP_LOGI(TAG, "started (tcp/%u; CAN + ISO15765 channels)", cfg->port);
    return (s_task != NULL && s_rx_task != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t j2534_server_stop(void)
{
    diag_hold(false); /* a stopping server holds nothing */
    s_run = false;
    s_started = false;
    return ESP_OK;
}

esp_err_t j2534_server_status(j2534_server_status_t *out)
{
    const j2534_config_t *cfg = j2534_settings_config();

    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    s_status.enabled = cfg->enabled;
    s_status.port = cfg->port;
    s_status.allow_reflash = cfg->allow_reflash;
    s_status.allow_lan = cfg->allow_lan;
    *out = s_status;
    out->exclusive = s_exclusive;
    out->autopid_paused = obd_gate_diag_held() && obd_gate_diag_acked();
    return ESP_OK;
}
