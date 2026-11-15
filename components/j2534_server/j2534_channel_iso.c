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
 * @file j2534_channel_iso.c
 * @brief The ISO15765 side of a J2534 channel: one ISO-TP session per
 *        FLOW_CONTROL filter on the registered provider (can_isotp.h),
 *        the SAE J2534-1 message layout [4-byte id][payload] both ways,
 *        single frames without a session (functional addressing), and the
 *        TxDone and START_OF_MESSAGE indications. Every function runs with
 *        the channel lock held by j2534_channel.c.
 *
 * 2026-10-19: a udsoncan tester's `10 03` reached the bus as the 6-byte
 * PDU `00 00 07 E0 10 03`, unpadded, because the device took the bare
 * payload and read the pad flag from CONNECT. Standard tools put the id
 * first and the pad flag on every message; the device does the same now.
 */
#include "j2534_channel_private.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "can_core.h"
#include "can_isotp.h"
#include "can_manager.h"
#include "obd_gate.h"

static const char *TAG = "j2534_server";

/* the reassembled PDU scratch: PSRAM, one RX pump drains every channel */
EXT_RAM_BSS_ATTR static uint8_t s_pdu[J2534_MAX_DATA];

static uint32_t now_us(void)
{
    return (uint32_t)esp_timer_get_time();
}

static void put_id_be(uint8_t *p, uint32_t id)
{
    p[0] = (uint8_t)(id >> 24);
    p[1] = (uint8_t)(id >> 16);
    p[2] = (uint8_t)(id >> 8);
    p[3] = (uint8_t)id;
}

bool j2534_iso_provider_ready(void)
{
    return can_isotp() != NULL && can_manager_core_handle() != NULL;
}

/* ---- the session table ------------------------------------------------ */

static int find_free(const j2534_channel_t *c)
{
    for (int i = 0; i < J2534_CH_MAX_SESSIONS; i++)
    {
        if (!c->sess[i].in_use)
        {
            return i;
        }
    }

    return -1;
}

static int find_by_rx(const j2534_channel_t *c, uint32_t rx_id, bool ext)
{
    for (int i = 0; i < J2534_CH_MAX_SESSIONS; i++)
    {
        if (c->sess[i].in_use && c->sess[i].rx_id == rx_id &&
            c->sess[i].ext == ext)
        {
            return i;
        }
    }

    return -1;
}

static int find_by_tx(const j2534_channel_t *c, uint32_t tx_id, bool ext)
{
    for (int i = 0; i < J2534_CH_MAX_SESSIONS; i++)
    {
        if (c->sess[i].in_use && c->sess[i].tx_id == tx_id &&
            c->sess[i].ext == ext)
        {
            return i;
        }
    }

    return -1;
}

/* open the provider session for a record whose ids and padding are set */
static uint32_t session_open(j2534_channel_t *c, j2534_iso_session_t *s)
{
    uint32_t st_us = c->iso_st_us;
    uint8_t stmin_ms = 0;

    if (st_us >= 1000u && (st_us % 1000u) == 0 && st_us / 1000u <= 127u)
    {
        stmin_ms = (uint8_t)(st_us / 1000u);
    }

    can_isotp_cfg_t cfg = {
        .tx_id        = s->tx_id,
        .rx_id        = s->rx_id,
        .ext_id       = s->ext,
        .block_size   = c->iso_bs,
        .stmin_ms     = stmin_ms,
        .stmin_us     = st_us,
        .padding_byte = 0xAA,
        .use_padding  = s->pad,
    };
    esp_err_t err = can_isotp()->open(&cfg, &s->isotp);

    if (err == ESP_OK)
    {
        s->in_use = true;
        ESP_LOGI(TAG, "ISO15765 session tx %03lX rx %03lX%s%s",
                 (unsigned long)s->tx_id, (unsigned long)s->rx_id,
                 s->ext ? " 29-bit" : "", s->pad ? " padded" : "");
        return J2534_STATUS_NOERROR;
    }

    s->isotp = NULL;
    s->in_use = false;

    if (err == ESP_ERR_NO_MEM)
    {
        ESP_LOGW(TAG, "ISO15765: the ISO-TP provider has no session left "
                      "for rx %03lX", (unsigned long)s->rx_id);
        return J2534_ERR_EXCEEDED_LIMIT;
    }

    /* expected refusals: the UDS terminal holds the id for 5 s after its
       last request, or the CAN node is down (the ACK carries the verdict) */
    ESP_LOGW(TAG, "ISO15765 session tx %03lX rx %03lX refused: %s",
             (unsigned long)s->tx_id, (unsigned long)s->rx_id,
             esp_err_to_name(err));
    return J2534_ERR_FAILED;
}

static void session_close(j2534_iso_session_t *s)
{
    if (s->in_use && s->isotp != NULL)
    {
        can_isotp()->close(s->isotp);
    }

    memset(s, 0, sizeof(*s));
}

uint32_t j2534_iso_bind(j2534_channel_t *c, uint32_t tx_id, uint32_t rx_id,
                        bool ext, bool pad, int *slot)
{
    if (!j2534_iso_provider_ready())
    {
        ESP_LOGW(TAG, "ISO15765 not available (no ISO-TP provider, or the "
                      "CAN node is down)");
        return J2534_ERR_FAILED;
    }

    /* the same ECU again: the tool re-arms its filter, rebind in place */
    int s = find_by_rx(c, rx_id, ext);

    if (s >= 0)
    {
        session_close(&c->sess[s]);
    }
    else
    {
        s = find_free(c);

        if (s < 0)
        {
            ESP_LOGW(TAG, "ISO15765: the channel's %d sessions are in use",
                     J2534_CH_MAX_SESSIONS);
            return J2534_ERR_EXCEEDED_LIMIT;
        }
    }

    c->sess[s].tx_id = tx_id;
    c->sess[s].rx_id = rx_id;
    c->sess[s].ext = ext;
    c->sess[s].pad = pad;

    uint32_t st = session_open(c, &c->sess[s]);

    if (st == J2534_STATUS_NOERROR && slot != NULL)
    {
        *slot = s;
    }

    return st;
}

void j2534_iso_unbind(j2534_channel_t *c, int slot)
{
    if (slot >= 0 && slot < J2534_CH_MAX_SESSIONS)
    {
        session_close(&c->sess[slot]);
    }
}

void j2534_iso_unbind_all(j2534_channel_t *c)
{
    for (int i = 0; i < J2534_CH_MAX_SESSIONS; i++)
    {
        session_close(&c->sess[i]);
    }

    c->ind_head = 0;
    c->ind_tail = 0;
}

/* ---- indications ------------------------------------------------------ */

static void indicate(j2534_channel_t *c, uint32_t rx_status, uint32_t id,
                     bool ext)
{
    uint8_t next = (uint8_t)((c->ind_head + 1) % J2534_CH_MAX_IND);

    if (next == c->ind_tail)
    {
        /* the pump is behind: the oldest indication goes */
        c->ind_tail = (uint8_t)((c->ind_tail + 1) % J2534_CH_MAX_IND);
    }

    c->ind[c->ind_head].rx_status = rx_status;
    c->ind[c->ind_head].id = id;
    c->ind[c->ind_head].ext = ext;
    c->ind[c->ind_head].ts_us = now_us();
    c->ind_head = next;
}

/* ---- write ------------------------------------------------------------ */

/* padding follows each message: a session opened the other way is reopened */
static bool pad_rebind(j2534_channel_t *c, j2534_iso_session_t *s, bool pad)
{
    if (s->pad == pad)
    {
        return true;
    }

    uint32_t tx = s->tx_id;
    uint32_t rx = s->rx_id;
    bool ext = s->ext;

    session_close(s);
    s->tx_id = tx;
    s->rx_id = rx;
    s->ext = ext;
    s->pad = pad;
    return session_open(c, s) == J2534_STATUS_NOERROR;
}

/* the send's own wait: flow control after the first frame, then the
   consecutive frames at the ECU's STmin; capped under the DLL's 6 s ACK
   wait (a 4 KB block at STmin 5 ms needs about 3 s) */
static uint32_t send_timeout_ms(size_t len)
{
    uint32_t frames = (len <= 7) ? 1u : 1u + (uint32_t)((len - 6u + 6u) / 7u);
    uint32_t t = 1500u + frames * 10u;

    return (t > 5500u) ? 5500u : t;
}

uint32_t j2534_iso_write(j2534_channel_t *c, const j2534_msg_t *msg,
                         bool indicate_done)
{
    uint32_t id;
    const uint8_t *pl;
    size_t n;

    if (!j2534_iso_split(msg, &id, &pl, &n))
    {
        if ((msg->tx_flags & J2534_TX_ISO15765_ADDR_TYPE) != 0)
        {
            ESP_LOGW(TAG, "ISO15765 extended addressing is not supported");
        }

        return J2534_ERR_INVALID_MSG;
    }

    if (n == 0)
    {
        return J2534_ERR_INVALID_MSG;
    }

    bool ext = (msg->tx_flags & J2534_TX_CAN_29BIT_ID) != 0;

    id &= ext ? 0x1FFFFFFFu : 0x7FFu;

    bool pad = (msg->tx_flags & J2534_TX_ISO15765_FRAME_PAD) != 0;
    int s = find_by_tx(c, id, ext);

    if (s < 0 && n > 7)
    {
        ESP_LOGD(TAG, "ISO15765 %u B to %03lX: no flow-control filter",
                 (unsigned)n, (unsigned long)id);
        return J2534_ERR_NO_FLOW_CONTROL;
    }

    if (!can_manager_tx_ready())
    {
        j2534_ch_say_refused("ISO15765");
        return J2534_ERR_FAILED;
    }

    /* an ESP-side requester on the shared bus: hold the conversation gate
       from the request until the reply is delivered (or the hold expires)
       so the MIC chip does not interleave; fail-open (obd_gate.h) */
    obd_gate_acquire(c, OBD_GATE_WAIT_MS);

    esp_err_t err;

    if (s >= 0)
    {
        if (!pad_rebind(c, &c->sess[s], pad))
        {
            obd_gate_release(c);
            return J2534_ERR_FAILED;
        }

        err = can_isotp()->send(c->sess[s].isotp, pl, n, send_timeout_ms(n));
    }
    else
    {
        /* functional addressing: one frame, no session */
        uint8_t fr[8];
        uint8_t dlc = j2534_iso_single_frame(pl, n, pad, fr);

        err = can_manager_send(id, ext, false, fr, dlc);
    }

    if (err != ESP_OK)
    {
        obd_gate_release(c);

        if (err == ESP_ERR_INVALID_STATE)
        {
            j2534_ch_say_refused("ISO15765");
        }

        ESP_LOGD(TAG, "ISO15765 send to %03lX: %s", (unsigned long)id,
                 esp_err_to_name(err));
        return (err == ESP_ERR_TIMEOUT) ? J2534_ERR_TIMEOUT : J2534_ERR_FAILED;
    }

    j2534_ch_note_reflash(pl[0]); /* forwarded: counted on the status, never refused */

    if (indicate_done)
    {
        indicate(c, J2534_RX_TX_MSG_TYPE | J2534_RX_TX_INDICATION, id, ext);
    }

    return J2534_STATUS_NOERROR;
}

/* ---- poll ------------------------------------------------------------- */

bool j2534_iso_poll(j2534_channel_t *c, j2534_msg_t *out)
{
    /* 1. a pending indication */
    if (c->ind_tail != c->ind_head)
    {
        const j2534_ch_ind_t *i = &c->ind[c->ind_tail];

        j2534_iso_indication(out, i->id, i->ext, i->rx_status, i->ts_us);
        c->ind_tail = (uint8_t)((c->ind_tail + 1) % J2534_CH_MAX_IND);
        return true;
    }

    /* 2. the bus frames: a first frame on a session's id announces a
          message; a single frame on an id no session owns is a reply to
          a functional request, delivered through the PASS filters */
    can_core_frame_t fr;
    bool pass_armed = j2534_ch_has_pass_filter(c);

    while (c->can_q != NULL && xQueueReceive(c->can_q, &fr, 0) == pdTRUE)
    {
        if (fr.rtr || fr.dlc > 8)
        {
            continue;
        }

        uint32_t ts = fr.ts_us ? fr.ts_us : now_us();

        if (find_by_rx(c, fr.id, fr.ext) >= 0)
        {
            if (j2534_iso_is_first_frame(fr.data, fr.dlc))
            {
                j2534_iso_indication(out, fr.id, fr.ext,
                                     J2534_RX_START_OF_MESSAGE, ts);
                return true;
            }

            continue; /* the session reassembles the rest */
        }

        if (!pass_armed)
        {
            continue; /* ISO15765 delivers nothing it was not asked for */
        }

        uint8_t view[12];
        size_t n;

        put_id_be(view, fr.id);
        memcpy(view + 4, fr.data, fr.dlc);

        if (j2534_ch_filters_pass(c, view, 4u + fr.dlc) &&
            j2534_iso_parse_single_frame(fr.data, fr.dlc, &n) &&
            j2534_iso_join(out, fr.id, fr.ext, fr.data + 1, n, ts))
        {
            obd_gate_release(c);
            return true;
        }
    }

    /* 3. the sessions' reassembled PDUs */
    for (int i = 0; i < J2534_CH_MAX_SESSIONS; i++)
    {
        j2534_iso_session_t *s = &c->sess[i];
        size_t n = 0;

        if (!s->in_use)
        {
            continue;
        }

        if (can_isotp()->recv(s->isotp, s_pdu, sizeof(s_pdu), &n, 0) == ESP_OK &&
            n > 0 && j2534_iso_join(out, s->rx_id, s->ext, s_pdu, n, now_us()))
        {
            obd_gate_release(c); /* the reply is in: conversation over */
            return true;
        }
    }

    return false;
}
