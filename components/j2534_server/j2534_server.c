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
 * @brief J2534 PassThru server core: lifecycle, the session/channel state
 *        machine (request dispatch), periodic messages and the RX pump
 *        that pushes RX_MSG frames. Transport-agnostic: every frame comes
 *        in and goes out through j2534_server_transport.c (TCP, CDC-ACM
 *        serial, BLE). Channels: j2534_channel (CAN via can_manager,
 *        ISO15765 via the registered ISO-TP provider). Settings live in
 *        j2534_server_settings.c. Wire protocol: J2534_WIRE_PROTOCOL.md.
 */
#include "j2534_server.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "can_core.h"      /* can_core_frame_t: the pump's queue peek */
#include "can_manager.h"
#include "log_manager.h"
#include "obd_gate.h"

#include "j2534_channel.h"
#include "j2534_proto.h"
#include "j2534_server_private.h"

static const char *TAG = "j2534_server";

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
/* PSRAM: each slot embeds a 4 KB j2534_msg_t - must not sit in internal
 * BSS (starves WiFi; §2 memory budget). */
EXT_RAM_BSS_ATTR static periodic_t s_periodic[J2534_MAX_PERIODIC];
static uint32_t s_next_periodic_id = 1;

/* big scratch buffers - PSRAM (single tester, one task each owns them).
 * j2534_msg_t is ~4 KB; NEVER put one on a task stack (the listener/RX
 * tasks have 4 KB stacks - three on the stack overflowed and reset the
 * device, caught 2026-07-07 via the DLL's FLOW_CONTROL filter). */
EXT_RAM_BSS_ATTR static uint8_t s_req_payload[J2534_RX_CAP];
EXT_RAM_BSS_ATTR static uint8_t s_rx_wire[J2534_MAX_DATA + 32];
EXT_RAM_BSS_ATTR static j2534_msg_t s_rx_msg;   /* RX pump task only */
/* request-handler scratch (session task only, one request at a time) */
EXT_RAM_BSS_ATTR static j2534_msg_t s_h_mask, s_h_pattern, s_h_fc, s_h_wmsg;

/* ---- module state ----------------------------------------------------- */

static bool s_started;
static volatile bool s_run;

static TaskHandle_t s_rx_task;
static StaticTask_t s_rx_tcb;
EXT_RAM_BSS_ATTR static StackType_t s_rx_stack[J2534_TASK_STACK];

static j2534_server_status_t s_status = { .transport = "none" };

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

/* ---- seam to the transport layer -------------------------------------- */

bool j2534_srv_running(void)
{
    return s_run;
}

volatile const bool *j2534_srv_run_flag(void)
{
    return &s_run;
}

uint8_t *j2534_srv_req_payload(size_t *cap)
{
    *cap = sizeof(s_req_payload);
    return s_req_payload;
}

void j2534_srv_count_rx(void)
{
    s_status.frames_rx++;
}

void j2534_srv_count_tx(void)
{
    s_status.frames_tx++;
}

void j2534_srv_count_tx_refused(void)
{
    s_status.tx_refused++;
}

void j2534_srv_count_reflash_seen(void)
{
    s_status.reflash_seen++;
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

    /* a periodic message raises no TxDone indication (SAE J2534-1) */
    if (pm->in_use && j2534_channel_active(pm->slot))
    {
        (void)j2534_channel_write_periodic(pm->slot, &pm->msg);
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

#define ACK(st, res) j2534_srv_send_ack(t, h->seq, h->channel, (st), (res))

static void handle_connect(const j2534_transport_t *t, const j2534_hdr_t *h,
                           const uint8_t *payload)
{
    /* payload: proto u32, flags u32, baud u32, [tx_id u32, rx_id u32]
     * (tx/rx optional - ISO15765 may instead bind via a FLOW_CONTROL
     * filter) */
    if (!s_session.device_open || h->length < 12)
    {
        j2534_srv_send_ack(t, h->seq, 0, J2534_ERR_DEVICE_NOT_CONNECTED, NULL);
        return;
    }

    uint32_t proto = rd_u32(payload);
    uint32_t flags = rd_u32(payload + 4);
    uint32_t baud = rd_u32(payload + 8);
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
        j2534_srv_send_ack(t, h->seq, 0, J2534_ERR_FAILED, NULL);
        return;
    }

    /* the bus has its bitrate, or takes the tester's on a silent bus
       (j2534_channel.c): a tester asking for another one is told so */
    uint32_t bst = j2534_channel_bus_baud(proto, baud);

    if (bst != J2534_STATUS_NOERROR)
    {
        j2534_srv_send_ack(t, h->seq, 0, bst, NULL);
        return;
    }

    uint32_t st = j2534_channel_connect(slot, proto, flags, tx_id, rx_id, ext);

    if (st != J2534_STATUS_NOERROR)
    {
        j2534_srv_send_ack(t, h->seq, 0, st, NULL);
        return;
    }

    s_session.ch_used[slot] = true;
    s_status.channel_count++;

    uint32_t ch_id = (uint32_t)slot + 1;

    ESP_LOGI(TAG, "CONNECT proto=%lu -> channel %lu", (unsigned long)proto,
             (unsigned long)ch_id);
    j2534_srv_send_ack(t, h->seq, (uint16_t)ch_id, J2534_STATUS_NOERROR,
                       &ch_id);
}

static void handle_write_msgs(const j2534_transport_t *t,
                              const j2534_hdr_t *h, const uint8_t *payload)
{
    int slot = (int)h->channel - 1;

    if (h->length < 4 + 24)
    {
        ACK(J2534_ERR_INVALID_MSG, NULL);
        return;
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

        p += consumed;
        left -= consumed;
        st = j2534_channel_write(slot, &s_h_wmsg);
    }

    ACK(st, NULL);
}

static void handle_start_filter(const j2534_transport_t *t,
                                const j2534_hdr_t *h, const uint8_t *payload)
{
    int slot = (int)h->channel - 1;

    /* payload: type u32, then mask, pattern, [flow_control] MSGs */
    if (h->length < 4 + 24)
    {
        ACK(J2534_ERR_INVALID_MSG, NULL);
        return;
    }

    uint32_t ftype = rd_u32(payload);
    const uint8_t *p = payload + 4;
    size_t left = h->length - 4;
    bool ok = j2534_msg_decode(p, left, &s_h_mask);
    bool has_fc = false;

    if (ok)
    {
        size_t c = 24 + s_h_mask.data_size;

        p += c; left -= c;
        ok = j2534_msg_decode(p, left, &s_h_pattern);
    }

    if (ok)
    {
        size_t c = 24 + s_h_pattern.data_size;

        p += c; left -= c;
        has_fc = j2534_msg_decode(p, left, &s_h_fc);
    }

    if (!ok)
    {
        ACK(J2534_ERR_INVALID_MSG, NULL);
        return;
    }

    uint32_t fid = 0;
    uint32_t st = j2534_channel_add_filter(slot, ftype, &s_h_mask, &s_h_pattern,
                                           has_fc ? &s_h_fc : NULL, &fid);

    ACK(st, st == J2534_STATUS_NOERROR ? &fid : NULL);
}

static void handle_start_periodic(const j2534_transport_t *t,
                                  const j2534_hdr_t *h, const uint8_t *payload)
{
    int slot = (int)h->channel - 1;

    /* payload: interval_ms u32, then one PASSTHRU_MSG */
    if (h->length < 4 + 24 || !j2534_channel_active(slot) ||
        !j2534_msg_decode(payload + 4, h->length - 4, &s_h_wmsg))
    {
        ACK(J2534_ERR_INVALID_MSG, NULL);
        return;
    }

    int pi = -1;

    for (int i = 0; i < J2534_MAX_PERIODIC; i++)
    {
        if (!s_periodic[i].in_use) { pi = i; break; }
    }

    if (pi < 0)
    {
        ACK(J2534_ERR_FAILED, NULL);
        return;
    }

    uint32_t interval = rd_u32(payload);

    if (interval < 5) interval = 5;

    periodic_t *pm = &s_periodic[pi];

    pm->slot = slot;
    pm->msg = s_h_wmsg;
    pm->id = s_next_periodic_id++;

    esp_timer_create_args_t ta = { .callback = periodic_cb, .arg = pm,
                                   .name = "j2534_per" };

    if (esp_timer_create(&ta, &pm->timer) != ESP_OK ||
        esp_timer_start_periodic(pm->timer, (uint64_t)interval * 1000) != ESP_OK)
    {
        ACK(J2534_ERR_FAILED, NULL);
        return;
    }

    pm->in_use = true;
    ACK(J2534_STATUS_NOERROR, &pm->id);
}

static void handle_stop_periodic(const j2534_transport_t *t,
                                 const j2534_hdr_t *h, const uint8_t *payload)
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

    ACK(st, NULL);
}

static void handle_ioctl(const j2534_transport_t *t, const j2534_hdr_t *h,
                         const uint8_t *payload)
{
    int slot = (int)h->channel - 1;
    uint32_t ioctl_id = (h->length >= 4) ? rd_u32(payload) : 0;

    /* v1: config is best-effort (bus params come from can_manager
     * settings); CLEAR_* operate on our channel state */
    switch (ioctl_id)
    {
        case J2534_IOCTL_CLEAR_MSG_FILTERS:   j2534_channel_clear_filters(slot); break;
        case J2534_IOCTL_CLEAR_PERIODIC_MSGS: periodic_clear_all(); break;
        default: break; /* GET/SET_CONFIG, CLEAR_*_BUFFER: ack OK */
    }

    ACK(J2534_STATUS_NOERROR, NULL);
}

void j2534_srv_handle_frame(const j2534_transport_t *t, const j2534_hdr_t *h,
                            const uint8_t *payload)
{
    switch (h->type)
    {
        case J2534_MT_HELLO:
        {
            /* reply with our wire version so the DLL can gate features */
            uint32_t ver = J2534_WIRE_VERSION;
            j2534_srv_send_ack(t, h->seq, 0, J2534_STATUS_NOERROR, &ver);
            break;
        }

        case J2534_MT_OPEN:
        {
            uint32_t dev_id = 1;
            s_session.device_open = true;
            s_status.device_open = true;
            j2534_srv_send_ack(t, h->seq, 0, J2534_STATUS_NOERROR, &dev_id);
            break;
        }

        case J2534_MT_CLOSE:
            periodic_clear_all();
            j2534_channel_reset_all();
            memset(&s_session, 0, sizeof(s_session));
            s_status.device_open = false;
            s_status.channel_count = 0;
            j2534_srv_send_ack(t, h->seq, 0, J2534_STATUS_NOERROR, NULL);
            break;

        case J2534_MT_CONNECT:
            handle_connect(t, h, payload);
            break;

        case J2534_MT_DISCONNECT:
        {
            int slot = (int)h->channel - 1;

            if (slot >= 0 && slot < J2534_MAX_CHANNELS && s_session.ch_used[slot])
            {
                j2534_channel_disconnect(slot);
                s_session.ch_used[slot] = false;
                if (s_status.channel_count > 0) s_status.channel_count--;
                ACK(J2534_STATUS_NOERROR, NULL);
            }
            else
            {
                ACK(J2534_ERR_INVALID_CHANNEL_ID, NULL);
            }
            break;
        }

        case J2534_MT_WRITE_MSGS:     handle_write_msgs(t, h, payload); break;
        case J2534_MT_START_FILTER:   handle_start_filter(t, h, payload); break;
        case J2534_MT_STOP_FILTER:
        {
            int slot = (int)h->channel - 1;
            uint32_t st = (h->length >= 4)
                ? j2534_channel_stop_filter(slot, rd_u32(payload))
                : J2534_ERR_INVALID_FILTER_ID;
            ACK(st, NULL);
            break;
        }
        case J2534_MT_START_PERIODIC: handle_start_periodic(t, h, payload); break;
        case J2534_MT_STOP_PERIODIC:  handle_stop_periodic(t, h, payload); break;
        case J2534_MT_IOCTL:          handle_ioctl(t, h, payload); break;

        default:
            ACK(J2534_ERR_FAILED, NULL);
            break;
    }
}

/* ---- session begin / end (called by the transport layer) --------------- */

void j2534_srv_session_begin(const char *transport_name)
{
    j2534_channel_reset_all();
    periodic_clear_all();
    memset(&s_session, 0, sizeof(s_session));
    s_status.client_connected = true;
    s_status.device_open = false;
    s_status.channel_count = 0;
    strlcpy(s_status.transport, transport_name, sizeof(s_status.transport));
    ESP_LOGI(TAG, "tester connected (%s)", transport_name);

    if (s_exclusive)
    {
        diag_hold(true);
    }
}

void j2534_srv_session_end(void)
{
    periodic_clear_all();
    j2534_channel_reset_all();
    s_status.client_connected = false;
    s_status.device_open = false;
    s_status.channel_count = 0;
    diag_hold(false);
    ESP_LOGI(TAG, "tester disconnected (%s)", s_status.transport);
    strlcpy(s_status.transport, "none", sizeof(s_status.transport));
}

/* ---- RX pump: drain every active channel, push RX_MSG to the tester --- */

/* RX_MSG frames waiting to go out in ONE transport write (PSRAM; the
 * pump task only). Up to a maximal frame's worth: every transport takes
 * that size today (CDC's DMA buffer, BLE's channel write), and the wire
 * contract lets frames be coalesced (J2534_WIRE_PROTOCOL.md section 1). */
#define J2534_RX_BATCH_MAX  32   /* frames per write at most */
EXT_RAM_BSS_ATTR static uint8_t s_rx_batch[J2534_MAX_FRAME];

/* One pass over the active channels into the batch. Returns the number
 * of frames appended (0 = nothing was pending anywhere). */
static unsigned pump_pass(const j2534_transport_t *t, size_t *used)
{
    unsigned got = 0;

    for (int slot = 0; slot < J2534_MAX_CHANNELS; slot++)
    {
        if (!s_session.ch_used[slot])
        {
            continue;
        }

        if (!j2534_channel_poll_rx(slot, &s_rx_msg))
        {
            continue;
        }

        size_t n = j2534_msg_encode(&s_rx_msg, s_rx_wire, sizeof(s_rx_wire));

        if (n == 0 || j2534_srv_active() != t)
        {
            continue;
        }

        if (*used + J2534_HDR_SIZE + n > sizeof(s_rx_batch))
        {
            /* no room beside what waits: send that first, alone */
            (void)j2534_srv_send_raw(t, s_rx_batch, *used);
            *used = 0;
        }

        j2534_hdr_encode(s_rx_batch + *used, J2534_MT_RX_MSG, 0,
                         (uint16_t)(slot + 1), (uint32_t)n);
        memcpy(s_rx_batch + *used + J2534_HDR_SIZE, s_rx_wire, n);
        *used += J2534_HDR_SIZE + n;
        got++;
    }

    return got;
}

/* The pump pushes what the channels received as it arrives. 2026-11-06
 * (the VW TP 2.0 speed report: a tool that acknowledges every block of a
 * transfer pays the device's round trip at every block end): the pump
 * blocks on the active channel's bus queue instead of sleeping 2 ms
 * between polls (a frame woke the tester up to 2 ms late), and what has
 * piled up goes out in one transport write instead of one per frame. */
static void rx_pump_task(void *arg)
{
    (void)arg;

    while (s_run)
    {
        const j2534_transport_t *t = j2534_srv_active();

        if (t == NULL)
        {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        size_t used = 0;
        unsigned frames = 0;
        unsigned got;

        /* drain: a burst that arrives while the pass runs joins the batch */
        while ((got = pump_pass(t, &used)) > 0)
        {
            frames += got;

            if (frames >= J2534_RX_BATCH_MAX)
            {
                break;
            }
        }

        if (used > 0)
        {
            (void)j2534_srv_send_raw(t, s_rx_batch, used);
            continue; /* straight back for what came in meanwhile */
        }

        /* nothing pending: wait for the next bus frame on the active
           channel's queue (peek: the poll above consumes), or 2 ms for
           what no queue announces (an ISO-TP PDU the provider completes,
           a J1850 reply, an indication) */
        QueueHandle_t q = j2534_channel_wait_queue();
        can_core_frame_t peek;

        if (q == NULL || xQueuePeek(q, &peek, pdMS_TO_TICKS(2)) != pdTRUE)
        {
            if (q == NULL)
            {
                vTaskDelay(pdMS_TO_TICKS(2));
            }
        }
    }

    s_rx_task = NULL;
    vTaskDelete(NULL);
}

/* ---- lifecycle -------------------------------------------------------- */

esp_err_t j2534_server_init(void)
{
    static const log_descriptor_t LOG_DESC = { "j2534_server", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);
    j2534_srv_transport_init();
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
    s_exclusive = cfg->exclusive; /* the runtime switch's boot default, shown
                                     in the status even while disabled */
    s_status.enabled = cfg->enabled;
    s_status.port = cfg->port;

    if (!cfg->enabled)
    {
        ESP_LOGI(TAG, "disabled by settings");
        return ESP_OK;
    }

    s_run = true;

    esp_err_t err = j2534_srv_listener_start(cfg->port);

    s_rx_task = xTaskCreateStatic(rx_pump_task, "j2534_rx", J2534_TASK_STACK,
                                  NULL, J2534_TASK_PRIO, s_rx_stack, &s_rx_tcb);

    ESP_LOGI(TAG, "started (tcp/%u + serial + ble transports; CAN + ISO15765)",
             cfg->port);
    return (err == ESP_OK && s_rx_task != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t j2534_server_stop(void)
{
    diag_hold(false); /* a stopping server holds nothing */
    s_run = false;
    s_started = false;
    return ESP_OK;
}

bool j2534_server_is_enabled(void)
{
    return j2534_settings_is_configured() && j2534_settings_config()->enabled;
}

bool j2534_server_is_running(void)
{
    return s_run;
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
    s_status.allow_lan = cfg->allow_lan;
    s_status.listening = j2534_srv_listening();
    *out = s_status;
    out->exclusive = s_exclusive;
    out->autopid_paused = obd_gate_diag_held() && obd_gate_diag_acked();
    return ESP_OK;
}
