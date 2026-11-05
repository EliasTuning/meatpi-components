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
 * @file j2534_channel.c
 * @brief J2534 channel transports (see j2534_channel.h): lifecycle, the
 *        CAN channel on can_manager (raw frames), the filter table and
 *        the bus-bitrate rule at CONNECT. The ISO15765
 *        side (sessions, the SAE J2534-1 message layout, indications)
 *        lives in j2534_channel_iso.c; both share j2534_channel_private.h.
 */
#include "j2534_channel.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "can_core.h"
#include "can_manager.h"
#include "obd_gate.h"

#include "j2534_channel_private.h"
#include "j2534_server.h"   /* J2534_MAX_CHANNELS */
#include "j2534_server_private.h"

static const char *TAG = "j2534_server";

/* the channel records: PSRAM (no ISR and no flash path touches them) */
EXT_RAM_BSS_ATTR static j2534_channel_t s_ch[J2534_MAX_CHANNELS];
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;   /* internal: FreeRTOS object */
static bool s_refused_said;            /* the refusal line went out this connect */

/* The channels' bus queues live for the module's life (2026-11-06): the
 * RX pump blocks on the active channel's queue between frames
 * (j2534_channel_wait_queue), so no queue may be deleted under it; a
 * CONNECT resets its slot's queue instead. can_core fills them from its
 * RX task (never an ISR), so the storage sits in PSRAM like the records;
 * the queue objects stay internal. Before, a channel's queue was created
 * at CONNECT and deleted at teardown, and the pump polled every 2 ms. */
static StaticQueue_t s_can_q_obj[J2534_MAX_CHANNELS];   /* internal: FreeRTOS objects */
EXT_RAM_BSS_ATTR static uint8_t s_can_q_store[J2534_MAX_CHANNELS]
                                              [J2534_CH_CAN_QLEN * sizeof(can_core_frame_t)];
static QueueHandle_t s_can_q[J2534_MAX_CHANNELS];

/* The UDS memory-transfer services (0x34 RequestDownload, 0x35
 * RequestUpload, 0x36 TransferData, 0x37 RequestTransferExit) in a
 * tester's request are COUNTED (/api/j2534 reflash_seen), never refused.
 * Until 2026-10-27 a closed "allow_reflash" gate answered them with
 * ERR_NOT_SUPPORTED: on ISO15765 by the service byte, which blocked memory
 * READS (0x35/0x36 upload) and missed every other write service; on raw
 * CAN by reading every frame as an ISO-TP single/first frame, which refused
 * ordinary frames of tools with their own framing (a VW TP 2.0 flasher: any
 * payload byte 0x34..0x37 behind a 0x0N/0x1N sequence byte, "randomly",
 * field report 2026-10-27). A pass-through device forwards what the tool
 * sends; the owner's consent is `enabled` and `allow_lan`. */
void j2534_ch_note_reflash(uint8_t sid)
{
    if (sid >= 0x34 && sid <= 0x37)
    {
        j2534_srv_count_reflash_seen();
    }
}

/* A write the CAN node refused: it may not talk yet (listening for the
   bitrate on a silent bus with `auto`, a mismatch, listen-only). Counted
   on /api/j2534, said once per CONNECT (can_core says why, once per
   episode). 2026-10-17: a tester read the silent refusal as "no comm". */
void j2534_ch_say_refused(const char *what)
{
    j2534_srv_count_tx_refused();

    if (s_refused_said)
    {
        return;
    }

    s_refused_said = true;
    ESP_LOGW(TAG, "%s write refused: the CAN node may not talk yet (see "
             "the can_core line: no bitrate proven, a mismatch, or "
             "listen-only); set the bit rate, or CONNECT with the bus's",
             what);
}

uint32_t j2534_channel_bus_baud(uint32_t proto, uint32_t baud_bps)
{
    if (proto != J2534_PROT_CAN && proto != J2534_PROT_ISO15765)
    {
        return J2534_STATUS_NOERROR; /* not a CAN protocol: nothing to check */
    }

    /* the bus has its bitrate (the device's setting, what the node read
       off the wire, or what a tester stated and it runs at): a tester that
       asks for another one is told so, not served at the wrong rate */
    can_manager_status_t cst;
    uint32_t bus_kbps = 0;

    if (can_manager_status(&cst) == ESP_OK && cst.running)
    {
        bus_kbps = (cst.link.verified || cst.link.adopted)
                       ? cst.link.baud_kbps
                   : cst.baud_auto ? 0
                                   : cst.baud_kbps;
    }

    if (j2534_connect_baud_check(baud_bps, bus_kbps) != J2534_STATUS_NOERROR)
    {
        ESP_LOGW(TAG, "CONNECT at %lu bit/s refused: the CAN bus runs at "
                      "%lu kbit/s", (unsigned long)baud_bps,
                 (unsigned long)bus_kbps);
        return J2534_ERR_INVALID_BAUDRATE;
    }

    /* auto with nothing proven: the tester's baud is the best word there
       is on a silent bus (2026-10-17: a tester on a gatewayed OBD port,
       which speaks only when asked, read the refused writes as "connected,
       no comm to the CAN bus"). Not taken (the bus not silent, unreadable
       traffic seen, a rate this node does not know): the channel still
       opens and its writes are refused, with a W line per CONNECT. */
    if (bus_kbps == 0 && baud_bps >= 1000u)
    {
        esp_err_t ad = can_manager_adopt_baud(baud_bps / 1000u);

        if (ad == ESP_OK)
        {
            ESP_LOGI(TAG, "CONNECT at %lu bit/s: the bus was silent, the "
                          "node now runs at the tester's bitrate",
                     (unsigned long)baud_bps);
        }
        else if (ad == ESP_ERR_NOT_SUPPORTED)
        {
            ESP_LOGW(TAG, "CONNECT at %lu bit/s: no bitrate proven on the "
                          "bus and the tester's was not taken (the bus not "
                          "silent, unreadable traffic seen, or not a rate "
                          "this node knows): writes are refused until the "
                          "bus talks; set the bit rate",
                     (unsigned long)baud_bps);
        }
    }

    return J2534_STATUS_NOERROR;
}

static void put_id_be(uint8_t *p, uint32_t id)
{
    p[0] = (uint8_t)(id >> 24); p[1] = (uint8_t)(id >> 16);
    p[2] = (uint8_t)(id >> 8);  p[3] = (uint8_t)id;
}

static uint32_t get_id_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* ---- lifecycle -------------------------------------------------------- */

void j2534_channel_init(void)
{
    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }

    /* no subscription yet: a zeroed record would name subscriber 0, which
       belongs to someone else, and the teardown before a CONNECT would
       unsubscribe it (latent before 2026-10-19) */
    for (int i = 0; i < J2534_MAX_CHANNELS; i++)
    {
        s_ch[i].can_sub = -1;

        if (s_can_q[i] == NULL)
        {
            s_can_q[i] = xQueueCreateStatic(J2534_CH_CAN_QLEN,
                                            sizeof(can_core_frame_t),
                                            s_can_q_store[i],
                                            &s_can_q_obj[i]);
        }
    }
}

QueueHandle_t j2534_channel_wait_queue(void)
{
    /* every CAN and ISO15765 channel subscribes to every bus frame, so the
       first one's queue announces them all; read without the lock: the
       handles are static and never freed, and a channel closing under the
       pump costs it one 2 ms timeout at most */
    for (int i = 0; i < J2534_MAX_CHANNELS; i++)
    {
        if (s_ch[i].in_use && s_ch[i].can_q != NULL)
        {
            return s_ch[i].can_q;
        }
    }

    return NULL;
}

static void teardown(j2534_channel_t *c)
{
    if (c->can_sub >= 0)
    {
        can_manager_unsubscribe_queue(c->can_sub);
        c->can_sub = -1;
    }
    if (c->can_q != NULL)
    {
        xQueueReset(c->can_q);   /* static: lives on for the next CONNECT */
        c->can_q = NULL;
    }
    j2534_iso_unbind_all(c);
    j2534_legacy_drop((int)(c - s_ch)); /* J1850: the slot's buffered replies */
    obd_gate_release(c); /* idempotent: only if this channel holds it */
    memset(c, 0, sizeof(*c));
    c->can_sub = -1;
}

void j2534_channel_reset_all(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < J2534_MAX_CHANNELS; i++)
    {
        if (s_ch[i].in_use || s_ch[i].can_q != NULL || s_ch[i].can_sub >= 0)
        {
            teardown(&s_ch[i]);
        }
    }
    xSemaphoreGive(s_lock);
}

/* every frame on the bus into the channel's queue: the traffic itself on
   a CAN channel; first-frame peeks and the single frames no session owns
   on an ISO15765 one */
static bool subscribe_bus(j2534_channel_t *c, const char *name)
{
    c->can_q = s_can_q[c - s_ch];

    if (c->can_q != NULL)
    {
        xQueueReset(c->can_q);   /* frames of the slot's previous life */
    }

    if (c->can_q == NULL ||
        can_manager_subscribe_queue(c->can_q, 0, 0, c->ext, true,
                                    &c->can_sub) != ESP_OK)
    {
        ESP_LOGW(TAG, "no CAN subscription for the channel (no queue "
                      "or no free subscriber slot)");
        return false;
    }

    (void)can_manager_subscriber_name(c->can_sub, name);
    return true;
}

uint32_t j2534_channel_connect(int slot, uint32_t protocol, uint32_t flags,
                               uint32_t tx_id, uint32_t rx_id, bool ext)
{
    if (slot < 0 || slot >= J2534_MAX_CHANNELS)
    {
        return J2534_ERR_INVALID_CHANNEL_ID;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    j2534_channel_t *c = &s_ch[slot];
    teardown(c);
    s_refused_said = false;
    c->in_use = true;
    c->protocol = protocol;
    c->flags = flags;
    c->ext = ext;
    c->next_filter_id = 1;

    uint32_t st = J2534_STATUS_NOERROR;

    if (protocol == J2534_PROT_CAN)
    {
        if (!subscribe_bus(c, "j2534"))
        {
            st = J2534_ERR_FAILED;
        }
    }
    else if (protocol == J2534_PROT_ISO15765)
    {
        if (!subscribe_bus(c, "j2534_iso"))
        {
            st = J2534_ERR_FAILED;
        }
        else if (tx_id != 0 && rx_id != 0)
        {
            /* the ids on CONNECT (a WiCAN extension); standard tools arm a
               FLOW_CONTROL filter instead. Padding follows each message. */
            st = j2534_iso_bind(c, tx_id, rx_id, ext, false, NULL);
        }
    }
    else if (protocol == J2534_PROT_J1850VPW)
    {
        /* J1850 VPW through the OBD chip (GM Class 2); PWM / K-line share
           this dispatch when built. No CAN subscription: the chip owns the
           bus and WRITE_MSGS collects the replies. */
        st = j2534_legacy_connect(c);
    }
    else
    {
        st = J2534_ERR_NOT_SUPPORTED;
    }

    if (st != J2534_STATUS_NOERROR)
    {
        teardown(c);
    }

    xSemaphoreGive(s_lock);
    return st;
}

void j2534_channel_disconnect(int slot)
{
    if (slot < 0 || slot >= J2534_MAX_CHANNELS)
    {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    teardown(&s_ch[slot]);
    xSemaphoreGive(s_lock);
}

bool j2534_channel_active(int slot)
{
    return slot >= 0 && slot < J2534_MAX_CHANNELS && s_ch[slot].in_use;
}

/* ---- write ------------------------------------------------------------ */

static uint32_t write_common(int slot, const j2534_msg_t *msg, bool indicate)
{
    if (slot < 0 || slot >= J2534_MAX_CHANNELS || !s_ch[slot].in_use)
    {
        return J2534_ERR_INVALID_CHANNEL_ID;
    }

    j2534_channel_t *c = &s_ch[slot];
    uint32_t st = J2534_STATUS_NOERROR;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (c->protocol == J2534_PROT_CAN)
    {
        /* raw CAN: whatever the tool sends goes out; no content gate
           (2026-10-27, see j2534_ch_note_reflash) */
        if (msg->data_size < 4 || msg->data_size > 12)
        {
            st = J2534_ERR_INVALID_MSG;
        }
        else
        {
            uint32_t id = get_id_be(msg->data);
            bool ext = (msg->tx_flags & J2534_TX_CAN_29BIT_ID) != 0;
            uint8_t dlc = (uint8_t)(msg->data_size - 4);
            esp_err_t err = can_manager_send(id, ext, false, msg->data + 4,
                                             dlc);

            if (err != ESP_OK)
            {
                st = J2534_ERR_FAILED;

                if (err == ESP_ERR_INVALID_STATE)
                {
                    j2534_ch_say_refused("CAN");
                }
            }
        }
    }
    else if (c->protocol == J2534_PROT_ISO15765)
    {
        st = j2534_iso_write(c, msg, indicate);
    }
    else if (c->protocol == J2534_PROT_J1850VPW)
    {
        st = j2534_legacy_write(c, slot, msg, indicate);
    }
    else
    {
        st = J2534_ERR_MSG_PROTOCOL_ID;
    }

    xSemaphoreGive(s_lock);
    return st;
}

uint32_t j2534_channel_write(int slot, const j2534_msg_t *msg)
{
    return write_common(slot, msg, true);
}

uint32_t j2534_channel_write_periodic(int slot, const j2534_msg_t *msg)
{
    return write_common(slot, msg, false);
}

/* ---- filters ---------------------------------------------------------- */

static void filter_remove_at(j2534_channel_t *c, int i)
{
    c->filters[i] = c->filters[--c->n_filters];
}

uint32_t j2534_channel_add_filter(int slot, uint32_t type,
                                  const j2534_msg_t *mask,
                                  const j2534_msg_t *pattern,
                                  const j2534_msg_t *flow_control,
                                  uint32_t *filter_id)
{
    if (slot < 0 || slot >= J2534_MAX_CHANNELS || !s_ch[slot].in_use)
    {
        return J2534_ERR_INVALID_CHANNEL_ID;
    }

    if (filter_id == NULL)
    {
        return J2534_ERR_NULL_PARAMETER;
    }

    j2534_channel_t *c = &s_ch[slot];
    uint32_t st = J2534_STATUS_NOERROR;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (type == J2534_FLOW_CONTROL_FILTER)
    {
        /* ISO15765: pattern = the ECU's response id, flow-control message =
           the request id; its TxFlags name 29-bit ids and the padding of
           the frames this session sends (SAE J2534-1) */
        if (c->protocol != J2534_PROT_ISO15765 ||
            pattern == NULL || flow_control == NULL ||
            pattern->data_size < 4 || flow_control->data_size < 4)
        {
            st = J2534_ERR_INVALID_MSG;
        }
        else if (c->n_filters >= J2534_CH_MAX_FILTERS)
        {
            st = J2534_ERR_EXCEEDED_LIMIT;
        }
        else
        {
            uint32_t fl = pattern->tx_flags | flow_control->tx_flags;
            bool ext = (fl & J2534_TX_CAN_29BIT_ID) != 0;
            bool pad = (flow_control->tx_flags & J2534_TX_ISO15765_FRAME_PAD) != 0;
            uint32_t idmask = ext ? 0x1FFFFFFFu : 0x7FFu;
            uint32_t rx = get_id_be(pattern->data) & idmask;
            uint32_t tx = get_id_be(flow_control->data) & idmask;
            int sess = -1;

            st = j2534_iso_bind(c, tx, rx, ext, pad, &sess);

            if (st == J2534_STATUS_NOERROR)
            {
                /* the tool re-arming the same ECU: its old record goes */
                for (int i = 0; i < c->n_filters; i++)
                {
                    if (c->filters[i].type == J2534_FLOW_CONTROL_FILTER &&
                        c->filters[i].session == sess)
                    {
                        filter_remove_at(c, i);
                        break;
                    }
                }

                j2534_ch_filter_t *f = &c->filters[c->n_filters++];

                memset(f, 0, sizeof(*f));
                f->id = c->next_filter_id++;
                f->type = type;
                f->session = sess;
                *filter_id = f->id;
            }
        }
    }
    else if (type == J2534_PASS_FILTER || type == J2534_BLOCK_FILTER)
    {
        if (mask == NULL || pattern == NULL)
        {
            st = J2534_ERR_NULL_PARAMETER;
        }
        else if (c->n_filters >= J2534_CH_MAX_FILTERS)
        {
            st = J2534_ERR_EXCEEDED_LIMIT;
        }
        else
        {
            j2534_ch_filter_t *f = &c->filters[c->n_filters];
            uint8_t n = (uint8_t)(mask->data_size < 8 ? mask->data_size : 8);

            memset(f, 0, sizeof(*f));
            f->id = c->next_filter_id++;
            f->type = type;
            f->session = -1;
            f->len = n;
            memcpy(f->mask, mask->data, n);
            memcpy(f->pattern, pattern->data,
                   pattern->data_size < n ? pattern->data_size : n);
            c->n_filters++;
            *filter_id = f->id;
        }
    }
    else
    {
        st = J2534_ERR_INVALID_MSG;
    }

    xSemaphoreGive(s_lock);
    return st;
}

uint32_t j2534_channel_stop_filter(int slot, uint32_t filter_id)
{
    if (slot < 0 || slot >= J2534_MAX_CHANNELS || !s_ch[slot].in_use)
    {
        return J2534_ERR_INVALID_CHANNEL_ID;
    }

    j2534_channel_t *c = &s_ch[slot];
    uint32_t st = J2534_ERR_INVALID_FILTER_ID;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < c->n_filters; i++)
    {
        if (c->filters[i].id == filter_id)
        {
            if (c->filters[i].type == J2534_FLOW_CONTROL_FILTER)
            {
                j2534_iso_unbind(c, c->filters[i].session);
            }

            filter_remove_at(c, i);
            st = J2534_STATUS_NOERROR;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return st;
}

void j2534_channel_clear_filters(int slot)
{
    if (slot < 0 || slot >= J2534_MAX_CHANNELS)
    {
        return;
    }

    j2534_channel_t *c = &s_ch[slot];

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < c->n_filters; i++)
    {
        if (c->filters[i].type == J2534_FLOW_CONTROL_FILTER)
        {
            j2534_iso_unbind(c, c->filters[i].session);
        }
    }
    c->n_filters = 0;   /* the ids given at CONNECT keep their session */
    xSemaphoreGive(s_lock);
}

bool j2534_ch_has_pass_filter(const j2534_channel_t *c)
{
    for (int i = 0; i < c->n_filters; i++)
    {
        if (c->filters[i].type == J2534_PASS_FILTER)
        {
            return true;
        }
    }

    return false;
}

/* apply PASS/BLOCK filters to a [id][data] view */
bool j2534_ch_filters_pass(const j2534_channel_t *c, const uint8_t *view,
                           size_t len)
{
    bool any_pass = false, has_pass = false;

    for (int i = 0; i < c->n_filters; i++)
    {
        const j2534_ch_filter_t *f = &c->filters[i];

        if (f->type == J2534_FLOW_CONTROL_FILTER)
        {
            continue; /* a session, not a match rule */
        }

        bool m = j2534_filter_match(f->mask, f->len, f->pattern, f->len,
                                    view, len);

        if (f->type == J2534_BLOCK_FILTER && m)
        {
            return false;
        }
        if (f->type == J2534_PASS_FILTER)
        {
            has_pass = true;
            any_pass = any_pass || m;
        }
    }

    return has_pass ? any_pass : true; /* no PASS filter = pass all */
}

/* ---- rx poll ---------------------------------------------------------- */

bool j2534_channel_poll_rx(int slot, j2534_msg_t *out)
{
    if (slot < 0 || slot >= J2534_MAX_CHANNELS)
    {
        return false;
    }

    j2534_channel_t *c = &s_ch[slot];
    if (!c->in_use)
    {
        return false;
    }

    bool got = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (c->protocol == J2534_PROT_CAN && c->can_q != NULL)
    {
        can_core_frame_t fr;
        if (xQueueReceive(c->can_q, &fr, 0) == pdTRUE)
        {
            uint8_t view[12];
            put_id_be(view, fr.id);
            memcpy(view + 4, fr.data, fr.dlc);
            if (j2534_ch_filters_pass(c, view, 4u + fr.dlc))
            {
                memset(out, 0, sizeof(*out));
                out->protocol_id = J2534_PROT_CAN;
                /* the frame's own RX-interrupt time, not the poll time */
                out->timestamp = fr.ts_us ? fr.ts_us
                                          : (uint32_t)esp_timer_get_time();
                out->rx_status = fr.ext ? J2534_RX_CAN_29BIT_ID : 0;
                out->data_size = 4u + fr.dlc;
                memcpy(out->data, view, out->data_size);
                got = true;
            }
        }
    }
    else if (c->protocol == J2534_PROT_ISO15765)
    {
        got = j2534_iso_poll(c, out);
    }
    else if (c->protocol == J2534_PROT_J1850VPW)
    {
        got = j2534_legacy_poll(c, slot, out);
    }

    xSemaphoreGive(s_lock);
    return got;
}
