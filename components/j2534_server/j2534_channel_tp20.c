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
 * @file j2534_channel_tp20.c
 * @brief SAE J2534-2 TP2.0 on a CAN channel (protocol 0x800E).
 *
 * One channel holds four connections. REQUEST_CONNECTION returns as soon
 * as the setup telegram is on the bus; the RX pump finishes the 0xD0 / 0xA1
 * exchange and queues CONNECTION_ESTABLISHED or CONNECTION_LOST. Application
 * messages use the ISO15765 shape (4-byte id, then the KWP bytes). The
 * 2-byte TP length prefix stays on the device. Block ACKs, connection tests,
 * and 0xA8 teardown stay on the device too.
 *
 * Callers hold the channel lock. A write waits here for block ACKs, and
 * reads the bus queue itself while it waits, so the pump does not have to
 * run underneath it.
 */
#include "j2534_channel_private.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "can_core.h"
#include "can_manager.h"

static const char *TAG = "j2534_tp20";

#define TP_CONN_N   4
#define TP_PAYLOAD  4092
#define TP_IND_N    8
#define TP_RAW_N    8
#define TP_CFG_MAX  24

enum {
    TP_FREE = 0,
    TP_WAIT_D0,
    TP_WAIT_A1,
    TP_UP,
};

typedef struct {
    uint32_t rx_status;
    uint32_t ts;
    uint8_t  data_size;
    uint8_t  data[8];
} tp_ind_t;

typedef struct {
    uint8_t  len;
    bool     ext;
    uint32_t ts;
    uint8_t  data[12];
} tp_raw_t;

typedef struct {
    int      phase;
    bool     in_write;
    bool     have_filter;
    bool     have_rx;
    bool     asm_on;
    bool     ready_on;
    bool     ct_waiting;
    bool     ack_fresh;
    uint8_t  dest;
    uint8_t  tx_seq;
    uint8_t  rx_last;
    uint8_t  ecu_bs;
    uint8_t  ecu_t1;
    uint8_t  ecu_t3;
    uint8_t  ack_op;
    uint8_t  ack_seq;
    uint32_t ct_miss;
    uint32_t tries;
    uint32_t setup_id;
    uint32_t resp_id;
    uint32_t rx_id;
    uint32_t tx_id;
    uint32_t filter_id;
    uint32_t gap_us;
    uint32_t asm_need;
    uint32_t asm_got;
    uint32_t ready_len;
    uint32_t ready_id;
    int64_t  last_tx_us;
    int64_t  last_activity_us;
    int64_t  deadline_us;
    uint8_t  setup[7];
    uint8_t *asm_buf;
    uint8_t *ready;
} tp_conn_t;

struct j2534_tp20_state {
    uint32_t data_rate;
    uint32_t loopback;
    uint32_t sample_point;
    uint32_t sjw;
    uint32_t pins;
    uint32_t t_br_int;
    uint32_t t_e;
    uint32_t mntc;
    uint32_t t_cta;
    uint32_t mnct;
    uint32_t mntb;
    uint32_t mnt;
    uint32_t t_wait;
    uint32_t t1_ms;
    uint32_t t3_ms;
    uint32_t identifier;
    uint32_t rx_passive;
    uint32_t passive_fid;
    bool     have_passive_fid;
    uint8_t *block;
    tp_conn_t conn[TP_CONN_N];
    tp_ind_t ind[TP_IND_N];
    uint8_t  ind_h;
    uint8_t  ind_t;
    tp_raw_t raw[TP_RAW_N];
    uint8_t  raw_n;
};

/* ---- small codecs ----------------------------------------------------- */

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t id_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void put_id_be(uint8_t *p, uint32_t id)
{
    p[0] = (uint8_t)(id >> 24);
    p[1] = (uint8_t)(id >> 16);
    p[2] = (uint8_t)(id >> 8);
    p[3] = (uint8_t)id;
}

static uint32_t unpack_id(uint8_t lo, uint8_t hi)
{
    return (uint32_t)lo | ((uint32_t)(hi & 0x0Fu) << 8);
}

static void wait_until(int64_t due)
{
    int64_t now = esp_timer_get_time();

    if (now >= due)
    {
        return;
    }

    int64_t left = due - now;

    if (left > 2000)
    {
        vTaskDelay(pdMS_TO_TICKS((uint32_t)((left - 500) / 1000)));
    }

    while (esp_timer_get_time() < due)
    {
    }
}

static bool can_tx(uint32_t id, bool ext, const uint8_t *data, uint8_t n)
{
    esp_err_t err;

    if (n > 8)
    {
        return false;
    }

    err = can_manager_send(id, ext, false, data, n);

    if (err == ESP_ERR_INVALID_STATE)
    {
        j2534_ch_say_refused("TP2.0");
    }

    return err == ESP_OK;
}

static uint8_t wire_byte(const uint8_t *kwp, size_t kwp_len, size_t index)
{
    if (index == 0)
    {
        return (uint8_t)((kwp_len >> 8) & 0x7Fu);
    }

    if (index == 1)
    {
        return (uint8_t)kwp_len;
    }

    return kwp[index - 2];
}

/* ---- filters (one implicit pass per connection, plus passive) -------- */

static uint32_t arm_pass(j2534_channel_t *c, uint32_t can_id, uint32_t *fid)
{
    j2534_ch_filter_t *f;

    if (c->n_filters >= J2534_CH_MAX_FILTERS)
    {
        return J2534_ERR_EXCEEDED_LIMIT;
    }

    f = &c->filters[c->n_filters];
    memset(f, 0, sizeof(*f));
    f->id = c->next_filter_id++;

    if (c->next_filter_id == 0)
    {
        c->next_filter_id = 1;
    }

    f->type = J2534_PASS_FILTER;
    f->session = -1;
    f->tp_owned = true;
    f->len = 4;
    memset(f->mask, 0xFF, 4);
    put_id_be(f->pattern, can_id);
    c->n_filters++;
    *fid = f->id;
    return J2534_STATUS_NOERROR;
}

static void retarget(j2534_channel_t *c, uint32_t fid, uint32_t can_id)
{
    for (int i = 0; i < c->n_filters; i++)
    {
        if (c->filters[i].id == fid && c->filters[i].tp_owned)
        {
            put_id_be(c->filters[i].pattern, can_id);
            return;
        }
    }
}

static void drop_filter(j2534_channel_t *c, uint32_t fid)
{
    for (int i = 0; i < c->n_filters; i++)
    {
        if (c->filters[i].id == fid && c->filters[i].tp_owned)
        {
            c->filters[i] = c->filters[--c->n_filters];
            return;
        }
    }
}

static bool user_wants(const j2534_channel_t *c, const uint8_t *view, size_t len)
{
    bool has = false;
    bool any = false;

    for (int i = 0; i < c->n_filters; i++)
    {
        const j2534_ch_filter_t *f = &c->filters[i];
        bool match;

        if (f->tp_owned || f->type == J2534_FLOW_CONTROL_FILTER)
        {
            continue;
        }

        match = j2534_filter_match(f->mask, f->len, f->pattern, f->len,
                                   view, len);

        if (f->type == J2534_BLOCK_FILTER && match)
        {
            return false;
        }

        if (f->type == J2534_PASS_FILTER)
        {
            has = true;
            any = any || match;
        }
    }

    return has && any;
}

/* ---- queues ----------------------------------------------------------- */

static void push_ind(j2534_tp20_state_t *s, const tp_ind_t *n)
{
    uint8_t next = (uint8_t)((s->ind_h + 1) % TP_IND_N);

    if (next == s->ind_t)
    {
        s->ind_t = (uint8_t)((s->ind_t + 1) % TP_IND_N);
    }

    s->ind[s->ind_h] = *n;
    s->ind_h = next;
}

static bool pop_ind(j2534_tp20_state_t *s, j2534_msg_t *out)
{
    tp_ind_t *n;

    if (s->ind_t == s->ind_h)
    {
        return false;
    }

    n = &s->ind[s->ind_t];
    s->ind_t = (uint8_t)((s->ind_t + 1) % TP_IND_N);
    memset(out, 0, sizeof(*out));
    out->protocol_id = J2534_PROT_TP2_0;
    out->rx_status = n->rx_status;
    out->timestamp = n->ts;
    out->data_size = n->data_size;
    memcpy(out->data, n->data, n->data_size);
    return true;
}

static void queue_raw(j2534_tp20_state_t *s, const can_core_frame_t *fr)
{
    tp_raw_t *r;
    uint8_t dlc = fr->dlc > 8 ? 8 : fr->dlc;

    if (s->raw_n >= TP_RAW_N)
    {
        memmove(&s->raw[0], &s->raw[1], (TP_RAW_N - 1) * sizeof(s->raw[0]));
        s->raw_n = TP_RAW_N - 1;
    }

    r = &s->raw[s->raw_n++];
    memset(r, 0, sizeof(*r));
    r->ext = fr->ext;
    r->ts = fr->ts_us ? fr->ts_us : (uint32_t)esp_timer_get_time();
    put_id_be(r->data, fr->id);
    memcpy(r->data + 4, fr->data, dlc);
    r->len = (uint8_t)(4u + dlc);
}

static void indicate_up(j2534_tp20_state_t *s, uint32_t rx, uint32_t tx)
{
    tp_ind_t n;

    memset(&n, 0, sizeof(n));
    n.rx_status = J2534_RX_CONNECTION_ESTABLISHED;
    n.ts = (uint32_t)esp_timer_get_time();
    n.data_size = 8;
    put_id_be(n.data, rx);
    put_id_be(n.data + 4, tx);
    push_ind(s, &n);
}

static void indicate_lost(j2534_tp20_state_t *s, uint32_t rx, uint8_t reason)
{
    tp_ind_t n;

    memset(&n, 0, sizeof(n));
    n.rx_status = J2534_RX_CONNECTION_LOST;
    n.ts = (uint32_t)esp_timer_get_time();
    n.data_size = 5;
    put_id_be(n.data, rx);
    n.data[4] = reason;
    push_ind(s, &n);
}

/* ---- connections ------------------------------------------------------ */

static tp_conn_t *find_free(j2534_tp20_state_t *s)
{
    for (int i = 0; i < TP_CONN_N; i++)
    {
        if (s->conn[i].phase == TP_FREE)
        {
            return &s->conn[i];
        }
    }

    return NULL;
}

static bool id_busy(const j2534_tp20_state_t *s, uint32_t id)
{
    if (id == 0)
    {
        return false;
    }

    for (int i = 0; i < TP_CONN_N; i++)
    {
        const tp_conn_t *k = &s->conn[i];

        if (k->phase != TP_FREE && (k->rx_id == id || k->tx_id == id))
        {
            return true;
        }
    }

    return false;
}

static bool dest_busy(const j2534_tp20_state_t *s, uint8_t dest)
{
    for (int i = 0; i < TP_CONN_N; i++)
    {
        if (s->conn[i].phase != TP_FREE && s->conn[i].dest == dest)
        {
            return true;
        }
    }

    return false;
}

static tp_conn_t *find_tx(j2534_tp20_state_t *s, uint32_t id)
{
    for (int i = 0; i < TP_CONN_N; i++)
    {
        if (s->conn[i].phase == TP_UP && s->conn[i].tx_id == id)
        {
            return &s->conn[i];
        }
    }

    return NULL;
}

static void blank_conn(tp_conn_t *k)
{
    uint8_t *asm_buf = k->asm_buf;
    uint8_t *ready = k->ready;

    memset(k, 0, sizeof(*k));
    k->asm_buf = asm_buf;
    k->ready = ready;
}

static void fail_link(j2534_channel_t *c, tp_conn_t *k, uint8_t reason,
                      bool disconnect)
{
    uint32_t rx;
    uint32_t tx;
    int phase;
    uint32_t fid;
    bool have_filter;

    if (k->phase == TP_FREE)
    {
        return;
    }

    rx = k->rx_id;
    tx = k->tx_id;
    phase = k->phase;
    fid = k->filter_id;
    have_filter = k->have_filter;

    if (disconnect && tx != 0 && phase != TP_WAIT_D0)
    {
        uint8_t bye = 0xA8;

        (void)can_tx(tx, false, &bye, 1);
    }

    if (have_filter)
    {
        drop_filter(c, fid);
    }

    indicate_lost(c->tp20, rx, reason);
    ESP_LOGI(TAG, "connection lost rx=0x%03lx reason=0x%02x",
             (unsigned long)rx, reason);
    blank_conn(k);
}

static bool send_paced(tp_conn_t *k, const uint8_t *data, uint8_t n)
{
    if (k->gap_us != 0 && k->last_tx_us != 0)
    {
        wait_until(k->last_tx_us + (int64_t)k->gap_us);
    }

    if (!can_tx(k->tx_id, false, data, n))
    {
        return false;
    }

    k->last_tx_us = esp_timer_get_time();
    k->last_activity_us = k->last_tx_us;
    return true;
}

static void set_gap_wire(tp_conn_t *k, uint8_t wire)
{
    k->gap_us = j2534_tp20_round_gap_us(j2534_tp20_decode_us(wire));
    k->ecu_t3 = wire;
}

static void set_gap_ms(tp_conn_t *k, uint32_t ms)
{
    uint32_t us = (ms > (UINT32_MAX / 1000u)) ? UINT32_MAX : ms * 1000u;

    k->gap_us = j2534_tp20_round_gap_us(us);
}

static bool send_a0(j2534_channel_t *c, tp_conn_t *k)
{
    uint8_t a0[6];

    a0[0] = 0xA0;
    a0[1] = 15;
    a0[2] = j2534_tp20_encode_ms(c->tp20->t1_ms);
    a0[3] = 0xFF;
    a0[4] = j2534_tp20_encode_ms(c->tp20->t3_ms);
    a0[5] = 0xFF;
    return send_paced(k, a0, sizeof(a0));
}

static bool send_a1(j2534_channel_t *c, tp_conn_t *k)
{
    uint8_t a1[6];

    a1[0] = 0xA1;
    a1[1] = k->ecu_bs ? k->ecu_bs : 15;
    a1[2] = j2534_tp20_encode_ms(c->tp20->t1_ms);
    a1[3] = 0xFF;
    a1[4] = j2534_tp20_encode_ms(c->tp20->t3_ms);
    a1[5] = 0xFF;
    return send_paced(k, a1, sizeof(a1));
}

static void note_up(j2534_channel_t *c, tp_conn_t *k)
{
    k->phase = TP_UP;
    k->ct_waiting = false;
    k->ct_miss = 0;
    k->last_activity_us = esp_timer_get_time();

    if (k->ecu_bs == 0)
    {
        k->ecu_bs = 15;
    }

    if (k->have_filter)
    {
        retarget(c, k->filter_id, k->rx_id);
    }

    indicate_up(c->tp20, k->rx_id, k->tx_id);
    ESP_LOGI(TAG, "connection up rx=0x%03lx tx=0x%03lx bs=%u",
             (unsigned long)k->rx_id, (unsigned long)k->tx_id, k->ecu_bs);
}

static void send_ack(tp_conn_t *k, uint8_t seq)
{
    uint8_t ack = (uint8_t)(0xB0u | ((seq + 1u) & 0x0Fu));

    (void)send_paced(k, &ack, 1);
}

static void accept_data(tp_conn_t *k, uint8_t op, uint8_t seq,
                        const uint8_t *pay, uint8_t pay_len)
{
    bool replay = k->have_rx && seq == k->rx_last;
    bool next = !k->have_rx || seq == (uint8_t)((k->rx_last + 1u) & 0x0Fu);
    bool last = (op == 0x1u || op == 0x3u);
    bool need = (op == 0x0u || op == 0x1u);
    const uint8_t *p = pay;
    uint8_t n = pay_len;

    if (!replay && !next)
    {
        return;
    }

    if (!replay)
    {
        if (!k->asm_on)
        {
            uint32_t len = 0;
            bool bad = n < 2;

            if (!bad)
            {
                len = ((uint32_t)p[0] << 8 | p[1]) & 0x7FFFu;
                bad = len > TP_PAYLOAD || (last && len > (uint32_t)(n - 2));
            }

            if (bad)
            {
                k->rx_last = seq;
                k->have_rx = true;

                if (need)
                {
                    send_ack(k, seq);
                }

                return;
            }

            k->asm_need = len;
            k->asm_got = 0;
            k->asm_on = true;
            p += 2;
            n = (uint8_t)(n - 2);
        }

        if ((uint32_t)k->asm_got + n > TP_PAYLOAD)
        {
            k->asm_on = false;
            return;
        }

        memcpy(k->asm_buf + k->asm_got, p, n);
        k->asm_got += n;
        k->rx_last = seq;
        k->have_rx = true;

        if (k->asm_got >= k->asm_need)
        {
            if (!k->ready_on)
            {
                memcpy(k->ready, k->asm_buf, k->asm_need);
                k->ready_len = k->asm_need;
                k->ready_id = k->rx_id;
                k->ready_on = true;
            }
            else
            {
                ESP_LOGW(TAG, "TP2.0 message dropped: the tester has not read");
            }

            k->asm_on = false;
            k->asm_got = 0;
        }
        else if (last)
        {
            k->asm_on = false;
            k->asm_got = 0;
        }
    }

    if (need)
    {
        send_ack(k, seq);
    }

    k->last_activity_us = esp_timer_get_time();
}

static void on_a1(tp_conn_t *k, const uint8_t *d, uint8_t dlc)
{
    if (dlc < 6)
    {
        return;
    }

    k->ecu_bs = (d[1] >= 1 && d[1] <= 15) ? d[1] : 15;
    k->ecu_t1 = d[2];
    set_gap_wire(k, d[4]);
}

static void on_conn_frame(j2534_channel_t *c, tp_conn_t *k,
                          const can_core_frame_t *fr)
{
    const uint8_t *d = fr->data;
    uint8_t dlc = fr->dlc > 8 ? 8 : fr->dlc;
    uint8_t op;
    uint8_t seq;

    if (dlc < 1)
    {
        return;
    }

    if (k->phase == TP_WAIT_D0)
    {
        if (dlc >= 2 && d[1] >= 0xD6 && d[1] <= 0xD8)
        {
            fail_link(c, k, d[1], false);
            return;
        }

        if (dlc >= 7 && d[1] == 0xD0)
        {
            uint32_t rx;
            uint32_t tx;

            if ((d[3] & 0x10) != 0 || (d[5] & 0x10) != 0)
            {
                return;
            }

            rx = unpack_id(d[2], d[3]);
            tx = unpack_id(d[4], d[5]);

            if (rx == 0 || tx == 0)
            {
                return;
            }

            if (id_busy(c->tp20, rx) || id_busy(c->tp20, tx))
            {
                k->rx_id = rx;
                k->tx_id = tx;
                k->phase = TP_WAIT_A1;
                fail_link(c, k, 0xD8, true);
                return;
            }

            k->rx_id = rx;
            k->tx_id = tx;
            k->phase = TP_WAIT_A1;
            k->ecu_bs = 15;
            k->tries = 1;
            (void)send_a0(c, k);
            k->deadline_us = esp_timer_get_time() +
                             (int64_t)c->tp20->t_e * 1000;
        }

        return;
    }

    if (d[0] == 0xA8)
    {
        fail_link(c, k, 0, false);
        return;
    }

    if (d[0] == 0xA3)
    {
        /* An 0xA3 while we are waiting is the answer to ours. Answering
         * that as well makes the two sides trade tests forever. */
        bool reply_to_us = k->ct_waiting;
        uint8_t ans = 0xA3;

        k->ct_waiting = false;
        k->ct_miss = 0;
        k->last_activity_us = esp_timer_get_time();

        if (!reply_to_us)
        {
            (void)send_paced(k, &ans, 1);
        }

        return;
    }

    if (d[0] == 0xA0 && dlc >= 6 && k->phase == TP_UP)
    {
        if (d[1] >= 1 && d[1] <= 15)
        {
            k->ecu_bs = d[1];
        }

        set_gap_wire(k, d[4]);
        (void)send_a1(c, k);
        return;
    }

    if (d[0] == 0xA1)
    {
        on_a1(k, d, dlc);

        if (k->phase == TP_WAIT_A1)
        {
            note_up(c, k);
        }

        return;
    }

    if (k->phase != TP_UP)
    {
        return;
    }

    op = (uint8_t)(d[0] >> 4);
    seq = (uint8_t)(d[0] & 0x0Fu);

    if (op == 0x9u || op == 0xBu)
    {
        k->ack_op = op;
        k->ack_seq = seq;
        k->ack_fresh = true;
        k->last_activity_us = esp_timer_get_time();
        return;
    }

    if (op <= 0x3u)
    {
        accept_data(k, op, seq, d + 1, (uint8_t)(dlc - 1));
    }
}

static bool our_setup_echo(const j2534_tp20_state_t *s,
                           const can_core_frame_t *fr)
{
    if (fr->dlc < 7)
    {
        return false;
    }

    for (int i = 0; i < TP_CONN_N; i++)
    {
        const tp_conn_t *k = &s->conn[i];

        if (k->phase == TP_WAIT_D0 && fr->id == k->setup_id &&
            memcmp(fr->data, k->setup, 7) == 0)
        {
            return true;
        }
    }

    return false;
}

static void reject_passive(uint32_t resp, const uint8_t *req)
{
    uint8_t n[7];

    memcpy(n, req, 7);
    n[1] = 0xD8;
    (void)can_tx(resp, false, n, 7);
}

static void on_passive(j2534_channel_t *c, const can_core_frame_t *fr)
{
    j2534_tp20_state_t *s = c->tp20;
    const uint8_t *d = fr->data;
    uint8_t dest;
    uint32_t resp;
    uint32_t our_rx;
    uint32_t our_tx;
    uint32_t their_rx;
    tp_conn_t *k;
    uint32_t fid = 0;
    uint8_t reply[7];

    if (fr->dlc < 7 || d[1] != 0xC0 || our_setup_echo(s, fr))
    {
        return;
    }

    dest = d[0];
    resp = 0x200u + dest;
    our_rx = s->rx_passive;
    k = find_free(s);

    if (k == NULL || our_rx < 0x300u || our_rx > 0x7FFu)
    {
        reject_passive(resp, d);
        return;
    }

    their_rx = unpack_id(d[2], d[3]);

    if (their_rx >= 0x300u && their_rx <= 0x7FFu && their_rx != our_rx &&
        !id_busy(s, their_rx))
    {
        our_tx = their_rx;
    }
    else
    {
        our_tx = (our_rx == 0x300u) ? 0x301u : 0x300u;
    }

    if (our_tx < 0x300u || our_tx > 0x7FFu || our_tx == our_rx ||
        id_busy(s, our_tx) || id_busy(s, our_rx) || dest_busy(s, dest) ||
        arm_pass(c, our_rx, &fid) != J2534_STATUS_NOERROR)
    {
        reject_passive(resp, d);
        return;
    }

    blank_conn(k);
    k->phase = TP_UP;
    k->dest = dest;
    k->resp_id = resp;
    k->rx_id = our_rx;
    k->tx_id = our_tx;
    k->ecu_bs = 15;
    k->have_filter = true;
    k->filter_id = fid;
    set_gap_ms(k, s->t3_ms);
    reply[0] = dest;
    reply[1] = 0xD0;
    reply[2] = (uint8_t)our_tx;
    reply[3] = (uint8_t)((our_tx >> 8) & 0x0Fu);
    reply[4] = (uint8_t)our_rx;
    reply[5] = (uint8_t)((our_rx >> 8) & 0x0Fu);
    reply[6] = d[6];

    if (!can_tx(resp, false, reply, 7))
    {
        drop_filter(c, fid);
        blank_conn(k);
        return;
    }

    k->last_tx_us = esp_timer_get_time();
    note_up(c, k);
}

static tp_conn_t *match_frame(j2534_tp20_state_t *s, uint32_t id)
{
    for (int i = 0; i < TP_CONN_N; i++)
    {
        tp_conn_t *k = &s->conn[i];

        if (k->phase == TP_WAIT_D0 && id == k->resp_id)
        {
            return k;
        }
    }

    for (int i = 0; i < TP_CONN_N; i++)
    {
        tp_conn_t *k = &s->conn[i];

        if (k->phase != TP_FREE && k->phase != TP_WAIT_D0 && id == k->rx_id)
        {
            return k;
        }
    }

    return NULL;
}

static void on_frame(j2534_channel_t *c, const can_core_frame_t *fr)
{
    j2534_tp20_state_t *s = c->tp20;
    tp_conn_t *k;
    uint8_t view[12];
    uint8_t dlc;

    if (fr->rtr)
    {
        return;
    }

    dlc = fr->dlc > 8 ? 8 : fr->dlc;

    if (!fr->ext && s->identifier != 0 && fr->id == s->identifier)
    {
        on_passive(c, fr);

        if (fr->dlc >= 7 && fr->data[1] == 0xC0)
        {
            return;
        }
    }

    if (!fr->ext)
    {
        k = match_frame(s, fr->id);

        if (k != NULL)
        {
            on_conn_frame(c, k, fr);
            return;
        }
    }

    put_id_be(view, fr->id);
    memcpy(view + 4, fr->data, dlc);

    if (user_wants(c, view, 4u + dlc))
    {
        queue_raw(s, fr);
    }
}

static void tp_advance(j2534_channel_t *c)
{
    j2534_tp20_state_t *s = c->tp20;
    int64_t now = esp_timer_get_time();

    for (int i = 0; i < TP_CONN_N; i++)
    {
        tp_conn_t *k = &s->conn[i];

        if (k->phase == TP_WAIT_D0 || k->phase == TP_WAIT_A1)
        {
            bool ok;

            if (now < k->deadline_us)
            {
                continue;
            }

            if (s->mntc == 0 || k->tries >= s->mntc)
            {
                fail_link(c, k, 1, k->phase != TP_WAIT_D0);
                continue;
            }

            ok = (k->phase == TP_WAIT_D0)
                     ? can_tx(k->setup_id, false, k->setup, 7)
                     : send_a0(c, k);

            if (!ok)
            {
                fail_link(c, k, 1, k->phase != TP_WAIT_D0);
                continue;
            }

            k->tries++;
            k->deadline_us = esp_timer_get_time() + (int64_t)s->t_e * 1000;
            continue;
        }

        if (k->phase != TP_UP || k->in_write || s->t_cta == 0)
        {
            continue;
        }

        if (k->ct_waiting)
        {
            if (now >= k->deadline_us)
            {
                k->ct_waiting = false;
                k->ct_miss++;

                if (s->mnct == 0 || k->ct_miss >= s->mnct)
                {
                    fail_link(c, k, 1, true);
                }
            }

            continue;
        }

        if (now - k->last_activity_us >= (int64_t)s->t_cta * 1000)
        {
            uint8_t test = 0xA3;

            if (!send_paced(k, &test, 1))
            {
                continue;
            }

            k->ct_waiting = true;
            k->deadline_us = esp_timer_get_time() + (int64_t)s->t_e * 1000;
        }
    }
}

/* ---- write ------------------------------------------------------------ */

static uint32_t ack_window_us(const j2534_channel_t *c, const tp_conn_t *k)
{
    uint32_t us = j2534_tp20_decode_us(k->ecu_t1);

    if (us == 0)
    {
        us = c->tp20->t1_ms * 1000u;
    }

    if (us < 1000u)
    {
        us = 1000u;
    }

    return us;
}

static uint32_t wait_block_ack(j2534_channel_t *c, tp_conn_t *k,
                               uint8_t sent_seq, const uint8_t *frame,
                               uint8_t frame_len)
{
    uint8_t expected = (uint8_t)((sent_seq + 1u) & 0x0Fu);
    uint32_t extras = 0;

    for (;;)
    {
        int64_t limit = esp_timer_get_time() + (int64_t)ack_window_us(c, k);
        uint32_t notready = 0;
        bool got = false;

        while (esp_timer_get_time() < limit)
        {
            can_core_frame_t fr;
            int64_t left;
            TickType_t ticks;

            if (k->phase == TP_FREE)
            {
                return J2534_ERR_TIMEOUT;
            }

            tp_advance(c);
            left = limit - esp_timer_get_time();

            if (left <= 0)
            {
                break;
            }

            ticks = pdMS_TO_TICKS((uint32_t)((left + 999) / 1000));

            if (ticks == 0)
            {
                ticks = 1;
            }

            if (c->can_q == NULL ||
                xQueueReceive(c->can_q, &fr, ticks) != pdTRUE)
            {
                continue;
            }

            k->ack_fresh = false;
            on_frame(c, &fr);

            if (k->phase == TP_FREE)
            {
                return J2534_ERR_TIMEOUT;
            }

            if (!k->ack_fresh)
            {
                continue;
            }

            if (k->ack_op == 0xBu && k->ack_seq == expected)
            {
                got = true;
                break;
            }

            if (k->ack_op == 0x9u &&
                (k->ack_seq == expected || k->ack_seq == sent_seq))
            {
                notready++;

                if (notready > c->tp20->mntb)
                {
                    fail_link(c, k, 1, true);
                    return J2534_ERR_TIMEOUT;
                }

                if (c->tp20->t_wait > 0)
                {
                    wait_until(esp_timer_get_time() +
                               (int64_t)c->tp20->t_wait * 1000);
                }

                limit = esp_timer_get_time() + (int64_t)ack_window_us(c, k);
            }
        }

        if (got)
        {
            return J2534_STATUS_NOERROR;
        }

        if (k->phase == TP_FREE)
        {
            return J2534_ERR_TIMEOUT;
        }

        if (extras >= c->tp20->mnt)
        {
            fail_link(c, k, 1, true);
            return J2534_ERR_TIMEOUT;
        }

        extras++;
        k->ack_fresh = false;

        if (!send_paced(k, frame, frame_len))
        {
            return J2534_ERR_FAILED;
        }
    }
}

static uint32_t send_broadcast(j2534_channel_t *c, const j2534_msg_t *msg)
{
    uint8_t dlc;
    uint8_t frame[8];
    uint32_t id;
    bool ext;
    int64_t when;

    if (msg->data_size < 6 || msg->data_size > 12)
    {
        return J2534_ERR_INVALID_MSG;
    }

    if (msg->data[4] < 0xF0)
    {
        return J2534_ERR_INVALID_MSG;
    }

    dlc = (uint8_t)(msg->data_size - 4);
    memcpy(frame, msg->data + 4, dlc);
    ext = (msg->tx_flags & J2534_TX_CAN_29BIT_ID) != 0;
    id = id_be(msg->data);

    if (!ext)
    {
        id &= 0x7FFu;
    }

    when = esp_timer_get_time();

    for (int i = 0; i < 5; i++)
    {
        wait_until(when);
        frame[dlc - 2] = (i & 1) ? 0xAA : 0x55;
        frame[dlc - 1] = (i & 1) ? 0x55 : 0xAA;

        if (!can_tx(id, ext, frame, dlc))
        {
            return J2534_ERR_FAILED;
        }

        when = esp_timer_get_time() + (int64_t)c->tp20->t_br_int * 1000;
    }

    return J2534_STATUS_NOERROR;
}

static uint32_t segment(j2534_channel_t *c, tp_conn_t *k, const j2534_msg_t *msg)
{
    const uint8_t *kwp = msg->data + 4;
    size_t kwp_len = msg->data_size - 4;
    size_t total = kwp_len + 2;
    size_t offset = 0;
    unsigned block = 0;
    uint8_t seq = k->tx_seq;
    uint8_t bs = (k->ecu_bs >= 1 && k->ecu_bs <= 15) ? k->ecu_bs : 15;

    if (kwp_len > 0)
    {
        j2534_ch_note_reflash(kwp[0]);
    }

    k->in_write = true;
    k->ack_fresh = false;

    while (offset < total)
    {
        uint8_t n;
        uint8_t frame[8];
        bool last;
        bool need;
        uint8_t op;
        uint32_t st;

        if (k->phase != TP_UP)
        {
            k->in_write = false;
            return J2534_ERR_TIMEOUT;
        }

        n = (uint8_t)((total - offset) > 7 ? 7 : (total - offset));
        last = (offset + n) >= total;
        block++;
        need = block >= bs || last;
        op = need ? (last ? 0x1u : 0x0u) : (last ? 0x3u : 0x2u);
        frame[0] = (uint8_t)((op << 4) | (seq & 0x0Fu));

        for (uint8_t i = 0; i < n; i++)
        {
            frame[1 + i] = wire_byte(kwp, kwp_len, offset + i);
        }

        k->ack_fresh = false;

        if (!send_paced(k, frame, (uint8_t)(1u + n)))
        {
            k->in_write = false;
            return J2534_ERR_FAILED;
        }

        if (need)
        {
            st = wait_block_ack(c, k, seq, frame, (uint8_t)(1u + n));

            if (st != J2534_STATUS_NOERROR)
            {
                k->in_write = false;
                return st;
            }

            block = 0;
        }

        offset += n;
        seq = (uint8_t)((seq + 1u) & 0x0Fu);
        k->tx_seq = seq;
    }

    k->in_write = false;
    k->last_activity_us = esp_timer_get_time();
    return J2534_STATUS_NOERROR;
}

uint32_t j2534_tp20_write(struct j2534_channel *c, const j2534_msg_t *msg,
                          bool indicate)
{
    uint32_t id;
    uint32_t plen;
    bool ext;
    tp_conn_t *k;

    (void)indicate;

    if (c->tp20 == NULL || msg == NULL)
    {
        return J2534_ERR_INVALID_MSG;
    }

    if (msg->data_size < 4 || msg->data_size > 4096)
    {
        return J2534_ERR_INVALID_MSG;
    }

    if ((msg->tx_flags & J2534_TX_TP2_0_BROADCAST) != 0)
    {
        return send_broadcast(c, msg);
    }

    ext = (msg->tx_flags & J2534_TX_CAN_29BIT_ID) != 0;
    id = id_be(msg->data);

    if (!ext)
    {
        id &= 0x7FFu;
    }

    k = find_tx(c->tp20, id);

    if (k != NULL)
    {
        return segment(c, k, msg);
    }

    plen = msg->data_size - 4;

    if (plen > 8)
    {
        return J2534_ERR_NO_CONNECTION_ESTABLISHED;
    }

    if (plen > 0)
    {
        j2534_ch_note_reflash(msg->data[4]);
    }

    return can_tx(id, ext, msg->data + 4, (uint8_t)plen)
               ? J2534_STATUS_NOERROR
               : J2534_ERR_FAILED;
}

/* ---- ioctl ------------------------------------------------------------ */

static bool take_u32(j2534_tp20_state_t *s, uint32_t param, uint32_t *value)
{
    switch (param)
    {
        case J2534_CFG_DATA_RATE:           *value = s->data_rate; break;
        case J2534_CFG_LOOPBACK:            *value = s->loopback; break;
        case J2534_CFG_BIT_SAMPLE_POINT:    *value = s->sample_point; break;
        case J2534_CFG_SYNC_JUMP_WIDTH:     *value = s->sjw; break;
        case J2534_CFG_J1962_PINS:          *value = s->pins; break;
        case J2534_CFG_TP2_0_T_BR_INT:      *value = s->t_br_int; break;
        case J2534_CFG_TP2_0_T_E:           *value = s->t_e; break;
        case J2534_CFG_TP2_0_MNTC:          *value = s->mntc; break;
        case J2534_CFG_TP2_0_T_CTA:         *value = s->t_cta; break;
        case J2534_CFG_TP2_0_MNCT:          *value = s->mnct; break;
        case J2534_CFG_TP2_0_MNTB:          *value = s->mntb; break;
        case J2534_CFG_TP2_0_MNT:           *value = s->mnt; break;
        case J2534_CFG_TP2_0_T_WAIT:        *value = s->t_wait; break;
        case J2534_CFG_TP2_0_T1:            *value = s->t1_ms; break;
        case J2534_CFG_TP2_0_T3:            *value = s->t3_ms; break;
        case J2534_CFG_TP2_0_IDENTIFIER:    *value = s->identifier; break;
        case J2534_CFG_TP2_0_RXIDPASSIVE:   *value = s->rx_passive; break;
        default:                            return false;
    }

    return true;
}

static uint32_t apply_identifier(j2534_channel_t *c, uint32_t value)
{
    j2534_tp20_state_t *s = c->tp20;
    uint32_t fid = 0;
    uint32_t st;

    if (value != 0 && (value < 0x200u || value > 0x2EFu))
    {
        return J2534_ERR_INVALID_IOCTL_VALUE;
    }

    if (s->have_passive_fid)
    {
        drop_filter(c, s->passive_fid);
        s->have_passive_fid = false;
    }

    s->identifier = value;

    if (value == 0)
    {
        return J2534_STATUS_NOERROR;
    }

    st = arm_pass(c, value, &fid);

    if (st != J2534_STATUS_NOERROR)
    {
        s->identifier = 0;
        return st;
    }

    s->passive_fid = fid;
    s->have_passive_fid = true;
    return J2534_STATUS_NOERROR;
}

static uint32_t store_u32(j2534_channel_t *c, uint32_t param, uint32_t value)
{
    j2534_tp20_state_t *s = c->tp20;

    switch (param)
    {
        case J2534_CFG_DATA_RATE:         s->data_rate = value; break;
        case J2534_CFG_LOOPBACK:
            if (value > 1)
            {
                return J2534_ERR_INVALID_IOCTL_VALUE;
            }

            s->loopback = value;
            break;
        case J2534_CFG_BIT_SAMPLE_POINT:  s->sample_point = value; break;
        case J2534_CFG_SYNC_JUMP_WIDTH:   s->sjw = value; break;
        case J2534_CFG_J1962_PINS:
            if (value != 0x060Eu)
            {
                return J2534_ERR_NOT_SUPPORTED;
            }

            s->pins = value;
            break;
        case J2534_CFG_TP2_0_T_BR_INT:    s->t_br_int = value; break;
        case J2534_CFG_TP2_0_T_E:         s->t_e = value; break;
        case J2534_CFG_TP2_0_MNTC:        s->mntc = value; break;
        case J2534_CFG_TP2_0_T_CTA:       s->t_cta = value; break;
        case J2534_CFG_TP2_0_MNCT:        s->mnct = value; break;
        case J2534_CFG_TP2_0_MNTB:        s->mntb = value; break;
        case J2534_CFG_TP2_0_MNT:         s->mnt = value; break;
        case J2534_CFG_TP2_0_T_WAIT:      s->t_wait = value; break;
        case J2534_CFG_TP2_0_T1:          s->t1_ms = value; break;
        case J2534_CFG_TP2_0_T3:          s->t3_ms = value; break;
        case J2534_CFG_TP2_0_IDENTIFIER:  return apply_identifier(c, value);
        case J2534_CFG_TP2_0_RXIDPASSIVE:
            if (value != 0 && (value < 0x300u || value > 0x7FFu))
            {
                return J2534_ERR_INVALID_IOCTL_VALUE;
            }

            s->rx_passive = value;
            break;
        default:
            return J2534_ERR_NOT_SUPPORTED;
    }

    return J2534_STATUS_NOERROR;
}

static uint32_t ioctl_config(j2534_channel_t *c, bool get,
                             const uint8_t *body, uint32_t body_len,
                             uint8_t *out, uint32_t out_cap, uint32_t *out_len)
{
    uint32_t count;

    if (body == NULL || body_len < 4)
    {
        return J2534_ERR_INVALID_IOCTL_VALUE;
    }

    count = rd_u32(body);

    if (count > TP_CFG_MAX || body_len != 4u + count * 8u)
    {
        return J2534_ERR_INVALID_IOCTL_VALUE;
    }

    if (get)
    {
        if (out == NULL || out_len == NULL || out_cap < 4u + count * 8u)
        {
            return J2534_ERR_INVALID_MSG;
        }

        wr_u32(out, count);

        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t param = rd_u32(body + 4 + i * 8);
            uint32_t value = 0;

            if (!take_u32(c->tp20, param, &value))
            {
                return J2534_ERR_NOT_SUPPORTED;
            }

            wr_u32(out + 4 + i * 8, param);
            wr_u32(out + 8 + i * 8, value);
        }

        *out_len = 4u + count * 8u;
        return J2534_STATUS_NOERROR;
    }

    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t param = rd_u32(body + 4 + i * 8);
        uint32_t value = rd_u32(body + 8 + i * 8);
        uint32_t st = store_u32(c, param, value);

        if (st != J2534_STATUS_NOERROR)
        {
            return st;
        }
    }

    return J2534_STATUS_NOERROR;
}

static uint32_t ioctl_request(j2534_channel_t *c, const uint8_t *body,
                              uint32_t body_len)
{
    const uint8_t *b;
    uint32_t setup_id;
    uint32_t rx;
    uint32_t tx;
    uint32_t fid = 0;
    uint32_t st;
    uint8_t dest;
    tp_conn_t *k;

    if (body == NULL || body_len != 15 || rd_u32(body) != 11)
    {
        return J2534_ERR_INVALID_IOCTL_VALUE;
    }

    b = body + 4;

    if (b[5] != 0xC0)
    {
        return J2534_ERR_INVALID_IOCTL_VALUE;
    }

    setup_id = id_be(b) & 0x7FFu;

    if (setup_id == 0)
    {
        return J2534_ERR_INVALID_IOCTL_VALUE;
    }

    dest = b[4];
    rx = unpack_id(b[6], b[7]);
    tx = unpack_id(b[8], b[9]);

    if (dest_busy(c->tp20, dest) ||
        (((b[7] & 0x10) == 0) && rx != 0 && id_busy(c->tp20, rx)) ||
        (((b[9] & 0x10) == 0) && tx != 0 && id_busy(c->tp20, tx)))
    {
        return J2534_ERR_NOT_UNIQUE;
    }

    k = find_free(c->tp20);

    if (k == NULL)
    {
        return J2534_ERR_EXCEEDED_LIMIT;
    }

    /* Reserve the implicit pass filter before the bus is touched. The id
     * is unknown until 0xD0; 0xFFFFFFFF matches no 11-bit frame. */
    st = arm_pass(c, 0xFFFFFFFFu, &fid);

    if (st != J2534_STATUS_NOERROR)
    {
        return st;
    }

    blank_conn(k);
    k->phase = TP_WAIT_D0;
    k->dest = dest;
    k->setup_id = setup_id;
    k->resp_id = 0x200u + dest;
    k->ecu_bs = 15;
    k->have_filter = true;
    k->filter_id = fid;
    memcpy(k->setup, b + 4, 7);

    if (!can_tx(setup_id, false, k->setup, 7))
    {
        drop_filter(c, fid);
        blank_conn(k);
        return J2534_ERR_FAILED;
    }

    k->tries = 1;
    k->last_activity_us = esp_timer_get_time();
    k->deadline_us = k->last_activity_us + (int64_t)c->tp20->t_e * 1000;
    return J2534_STATUS_NOERROR;
}

static uint32_t ioctl_teardown(j2534_channel_t *c, const uint8_t *body,
                               uint32_t body_len)
{
    uint32_t rx;

    if (body == NULL || body_len != 8 || rd_u32(body) != 4)
    {
        return J2534_ERR_INVALID_IOCTL_VALUE;
    }

    rx = id_be(body + 4);

    for (int i = 0; i < TP_CONN_N; i++)
    {
        tp_conn_t *k = &c->tp20->conn[i];

        if (k->phase != TP_FREE && k->phase != TP_WAIT_D0 && k->rx_id == rx)
        {
            fail_link(c, k, 0, true);
            return J2534_STATUS_NOERROR;
        }
    }

    return J2534_ERR_INVALID_IOCTL_VALUE;
}

uint32_t j2534_tp20_ioctl(struct j2534_channel *c, uint32_t ioctl_id,
                          const uint8_t *body, uint32_t body_len,
                          uint8_t *out, uint32_t out_cap, uint32_t *out_len)
{
    if (c->tp20 == NULL)
    {
        return J2534_ERR_FAILED;
    }

    if (out_len != NULL)
    {
        *out_len = 0;
    }

    switch (ioctl_id)
    {
        case J2534_IOCTL_GET_CONFIG:
            return ioctl_config(c, true, body, body_len, out, out_cap, out_len);
        case J2534_IOCTL_SET_CONFIG:
            return ioctl_config(c, false, body, body_len, out, out_cap, out_len);
        case J2534_IOCTL_REQUEST_CONNECTION:
            return ioctl_request(c, body, body_len);
        case J2534_IOCTL_TEARDOWN_CONNECTION:
            return ioctl_teardown(c, body, body_len);
        default:
            return J2534_ERR_NOT_SUPPORTED;
    }
}

/* ---- lifecycle and poll ----------------------------------------------- */

static void apply_defaults(j2534_tp20_state_t *s)
{
    s->data_rate = 500000;
    s->sample_point = 80;
    s->sjw = 15;
    s->pins = 0x060E;
    s->t_br_int = 20;
    s->t_e = 100;
    s->mntc = 10;
    s->t_cta = 1000;
    s->mnct = 5;
    s->mntb = 5;
    s->mnt = 2;
    s->t_wait = 100;
    s->t1_ms = 100;
    s->t3_ms = 0;
}

uint32_t j2534_tp20_attach(struct j2534_channel *c)
{
    j2534_tp20_state_t *s;
    uint8_t *block;

    s = calloc(1, sizeof(*s));
    block = malloc((size_t)TP_CONN_N * 2u * TP_PAYLOAD);

    if (s == NULL || block == NULL)
    {
        free(block);
        free(s);
        return J2534_ERR_FAILED;
    }

    s->block = block;

    for (int i = 0; i < TP_CONN_N; i++)
    {
        s->conn[i].asm_buf = block + ((size_t)i * 2u) * TP_PAYLOAD;
        s->conn[i].ready = block + ((size_t)i * 2u + 1u) * TP_PAYLOAD;
    }

    apply_defaults(s);
    c->tp20 = s;
    return J2534_STATUS_NOERROR;
}

void j2534_tp20_detach(struct j2534_channel *c)
{
    j2534_tp20_state_t *s = c->tp20;

    if (s == NULL)
    {
        return;
    }

    for (int i = 0; i < TP_CONN_N; i++)
    {
        tp_conn_t *k = &s->conn[i];

        if (k->phase != TP_FREE && k->phase != TP_WAIT_D0 && k->tx_id != 0)
        {
            uint8_t bye = 0xA8;

            (void)can_tx(k->tx_id, false, &bye, 1);
        }

        if (k->have_filter)
        {
            drop_filter(c, k->filter_id);
        }
    }

    if (s->have_passive_fid)
    {
        drop_filter(c, s->passive_fid);
    }

    free(s->block);
    free(s);
    c->tp20 = NULL;
}

static bool deliver(j2534_channel_t *c, j2534_msg_t *out)
{
    j2534_tp20_state_t *s = c->tp20;

    if (pop_ind(s, out))
    {
        return true;
    }

    for (int i = 0; i < TP_CONN_N; i++)
    {
        tp_conn_t *k = &s->conn[i];

        if (!k->ready_on)
        {
            continue;
        }

        memset(out, 0, sizeof(*out));
        out->protocol_id = J2534_PROT_TP2_0;
        out->timestamp = (uint32_t)esp_timer_get_time();
        out->data_size = 4u + k->ready_len;

        if (out->data_size > J2534_MAX_DATA)
        {
            k->ready_on = false;
            continue;
        }

        put_id_be(out->data, k->ready_id);
        memcpy(out->data + 4, k->ready, k->ready_len);
        k->ready_on = false;
        return true;
    }

    if (s->raw_n > 0)
    {
        tp_raw_t raw = s->raw[0];

        memmove(&s->raw[0], &s->raw[1], (s->raw_n - 1) * sizeof(s->raw[0]));
        s->raw_n--;
        memset(out, 0, sizeof(*out));
        out->protocol_id = J2534_PROT_TP2_0;
        out->timestamp = raw.ts;
        out->rx_status = raw.ext ? J2534_RX_CAN_29BIT_ID : 0;
        out->data_size = raw.len;
        memcpy(out->data, raw.data, raw.len);
        return true;
    }

    return false;
}

bool j2534_tp20_poll(struct j2534_channel *c, j2534_msg_t *out)
{
    if (c->tp20 == NULL || out == NULL)
    {
        return false;
    }

    tp_advance(c);

    if (c->can_q != NULL)
    {
        can_core_frame_t fr;

        for (int n = 0; n < 32 && xQueueReceive(c->can_q, &fr, 0) == pdTRUE; n++)
        {
            on_frame(c, &fr);
        }
    }

    tp_advance(c);
    return deliver(c, out);
}
