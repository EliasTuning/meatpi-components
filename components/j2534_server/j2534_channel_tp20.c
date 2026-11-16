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
 * messages use the ISO15765 shape (4-byte id, then the message's bytes) and
 * go to the bus as written: "Messages passed to PassThruWriteMsgs by the
 * application will include all message address and data bytes" (SAE
 * J2534-2 19.3.1). A message ends at the end-of-message telegram (SAE J2819
 * has no length field), so the two length bytes a VW ECU wants in front of
 * a message are the application's, on the way out and on the way in. Block
 * ACKs, connection tests, and 0xA8 teardown stay on the device.
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
#define TP_BROADCAST_N 5u   /* telegrams of one broadcast (SAE J2819 5.1.2) */

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
    bool     asm_over;       /* the message under way is longer than TP_PAYLOAD */
    bool     ready_on;
    bool     ct_waiting;
    bool     ack_fresh;
    bool     break_seen;     /* a break came while a write was under way */
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
    uint32_t asm_got;
    uint32_t ready_len;
    uint32_t ready_id;
    int64_t  last_tx_us;
    int64_t  last_ct_us;     /* the connection came up, or the last test left */
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

/* An id in a channel setup or its reply: the low byte, then bits 10 to 8 in
 * bits 2 to 0 of the next (SAE J2819 5.1.3; bit 3 is reserved, bit 4 says
 * "not given"). */
static uint32_t unpack_id(uint8_t lo, uint8_t hi)
{
    return (uint32_t)lo | ((uint32_t)(hi & 0x07u) << 8);
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

/* True when a connection other than @p except receives or sends on @p id. */
static bool id_busy(const j2534_tp20_state_t *s, uint32_t id,
                    const tp_conn_t *except)
{
    if (id == 0)
    {
        return false;
    }

    for (int i = 0; i < TP_CONN_N; i++)
    {
        const tp_conn_t *k = &s->conn[i];

        if (k == except)
        {
            continue;
        }

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
    k->last_ct_us = esp_timer_get_time();

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

    if (!replay && !next)
    {
        /* Out of sequence: a telegram was lost on the way. It is not taken;
         * the acknowledge the other side asks for names the one that is
         * missing, and the block comes again from there (SAE J2819 5.2,
         * table 8). */
        if (need)
        {
            send_ack(k, k->rx_last);
        }

        return;
    }

    if (!replay)
    {
        /* A message is the data bytes of its telegrams, up to the telegram
         * that says "end of message" (SAE J2819 5.2). The device does not
         * look inside: where an ECU puts two length bytes in front, they
         * reach the application with the rest (SAE J2534-2 19.3.1). One
         * that is longer than a J2534 message is acknowledged all the way
         * and not handed over. */
        if (k->asm_got + pay_len > TP_PAYLOAD)
        {
            k->asm_over = true;
        }
        else if (pay_len > 0)
        {
            memcpy(k->asm_buf + k->asm_got, pay, pay_len);
            k->asm_got += pay_len;
        }

        k->rx_last = seq;
        k->have_rx = true;

        if (last)
        {
            if (k->asm_over)
            {
                ESP_LOGW(TAG, "TP2.0 message dropped: longer than %u bytes",
                         (unsigned)TP_PAYLOAD);
            }
            else if (k->ready_on)
            {
                ESP_LOGW(TAG, "TP2.0 message dropped: the tester has not read");
            }
            else
            {
                memcpy(k->ready, k->asm_buf, k->asm_got);
                k->ready_len = k->asm_got;
                k->ready_id = k->rx_id;
                k->ready_on = true;
            }

            k->asm_got = 0;
            k->asm_over = false;
        }
    }

    if (need)
    {
        send_ack(k, seq);
    }
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

            if (id_busy(c->tp20, rx, k) || id_busy(c->tp20, tx, k))
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
        /* a disconnect is answered with a disconnect (SAE J2819 6.2.11) */
        fail_link(c, k, 0, true);
        return;
    }

    if (d[0] == 0xA3)
    {
        /* A connection test is answered with a connection acknowledge,
         * never with another test (SAE J2819 5.2, 6.2.9). It also shows
         * the other side alive, so a test of ours counts as answered. */
        k->ct_waiting = false;
        k->ct_miss = 0;

        if (k->phase == TP_UP)
        {
            (void)send_a1(c, k);
        }

        return;
    }

    if (d[0] == 0xA4)
    {
        /* A break (SAE J2819 5.2, 6.2.7): the other side stops the message
         * this side is sending. The writer ends it; with no write under way
         * there is nothing to stop. */
        if (k->in_write)
        {
            k->break_seen = true;
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
        else
        {
            /* the answer to our connection test (SAE J2819 6.2.9) */
            k->ct_waiting = false;
            k->ct_miss = 0;
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
        !id_busy(s, their_rx, NULL))
    {
        our_tx = their_rx;
    }
    else
    {
        our_tx = (our_rx == 0x300u) ? 0x301u : 0x300u;
    }

    if (our_tx < 0x300u || our_tx > 0x7FFu || our_tx == our_rx ||
        id_busy(s, our_tx, NULL) || id_busy(s, our_rx, NULL) ||
        dest_busy(s, dest) ||
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

        /* The connection test (SAE J2819 5.2, table 7): one every T_CTa,
         * whatever else the connection carries, answered with a connection
         * acknowledge. A test still unanswered when the next one is due is
         * a miss; after MNCT repeats the connection is closed (6.2.10). A
         * write of ours holds the test back until its last telegram. */
        if (k->phase != TP_UP || k->in_write || s->t_cta == 0 ||
            now - k->last_ct_us < (int64_t)s->t_cta * 1000)
        {
            continue;
        }

        if (k->ct_waiting)
        {
            k->ct_miss++;

            if (k->ct_miss > s->mnct)
            {
                fail_link(c, k, 1, true);
                continue;
            }
        }

        {
            uint8_t test = 0xA3;

            if (!send_paced(k, &test, 1))
            {
                continue;
            }

            k->ct_waiting = true;
            k->last_ct_us = esp_timer_get_time();
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

/* What the wait behind an acknowledge request found. */
enum {
    TP_ACK_OK = 0,   /* the block is taken: go on behind it */
    TP_ACK_AGAIN,    /* the other side names a telegram: from there again */
    TP_ACK_BREAK,    /* a break came: end the message */
    TP_ACK_FAILED,   /* the connection is gone or a send failed */
};

/* Frames that came while a write is under way: the answer to a connection
 * test, a break, the other connections' telegrams. Nothing waits here. */
static void drain_bus(j2534_channel_t *c)
{
    can_core_frame_t fr;

    for (int n = 0; n < 8 && c->can_q != NULL &&
                    xQueueReceive(c->can_q, &fr, 0) == pdTRUE; n++)
    {
        on_frame(c, &fr);
    }
}

/* Wait for the acknowledge of the telegram @p sent_seq (SAE J2819 5.2,
 * table 8, 6.2.3 to 6.2.8):
 *   - an acknowledge with sent_seq + 1: the block is taken;
 *   - an acknowledge with another sequence number names the telegram to
 *     send again from (@p again_seq);
 *   - "receiver not ready" is either of the two after T_WAIT (6.2.4, 6.2.6);
 *   - none within T1: the telegram goes out again, MNT times (6.2.5), then
 *     the connection is closed;
 *   - a break ends the message (6.2.7, 6.2.8).
 * On TP_ACK_FAILED @p status holds the J2534 status. */
static int wait_block_ack(j2534_channel_t *c, tp_conn_t *k, uint8_t sent_seq,
                          const uint8_t *frame, uint8_t frame_len,
                          uint8_t *again_seq, uint32_t *status)
{
    uint8_t expected = (uint8_t)((sent_seq + 1u) & 0x0Fu);
    uint32_t extras = 0;

    *status = J2534_ERR_TIMEOUT;

    for (;;)
    {
        int64_t limit = esp_timer_get_time() + (int64_t)ack_window_us(c, k);

        while (esp_timer_get_time() < limit)
        {
            can_core_frame_t fr;
            int64_t left;
            TickType_t ticks;

            if (k->phase == TP_FREE)
            {
                return TP_ACK_FAILED;
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
                return TP_ACK_FAILED;
            }

            if (k->break_seen)
            {
                return TP_ACK_BREAK;
            }

            if (!k->ack_fresh)
            {
                continue;
            }

            if (k->ack_op == 0x9u && c->tp20->t_wait > 0)
            {
                wait_until(esp_timer_get_time() +
                           (int64_t)c->tp20->t_wait * 1000);
            }

            if (k->ack_seq == expected)
            {
                return TP_ACK_OK;
            }

            *again_seq = k->ack_seq;
            return TP_ACK_AGAIN;
        }

        if (k->phase == TP_FREE)
        {
            return TP_ACK_FAILED;
        }

        if (extras >= c->tp20->mnt)
        {
            fail_link(c, k, 1, true);
            return TP_ACK_FAILED;
        }

        extras++;
        k->ack_fresh = false;

        if (!send_paced(k, frame, frame_len))
        {
            *status = J2534_ERR_FAILED;
            return TP_ACK_FAILED;
        }
    }
}

/* A break stops the message (SAE J2819 5.2, 6.2.7): one data telegram with
 * the end-of-message bit and an acknowledge request and nothing behind it.
 * The connection stays; the write has failed. */
static uint32_t end_after_break(j2534_channel_t *c, tp_conn_t *k, uint8_t seq)
{
    uint8_t eom = (uint8_t)(0x10u | (seq & 0x0Fu));
    uint8_t again_seq = 0;
    uint32_t st = J2534_ERR_FAILED;

    k->break_seen = false;
    ESP_LOGW(TAG, "the write to 0x%03lx was ended by a break",
             (unsigned long)k->tx_id);

    if (send_paced(k, &eom, 1))
    {
        (void)wait_block_ack(c, k, seq, &eom, 1, &again_seq, &st);
    }

    if (k->phase == TP_UP)
    {
        k->tx_seq = (uint8_t)((seq + 1u) & 0x0Fu);
        k->break_seen = false;
    }

    k->in_write = false;
    return J2534_ERR_FAILED;
}

/* Telegrams @p first to @p first + @p count - 1 of one broadcast request,
 * T_BR_INT apart. The two key bytes behind the request are equal and change
 * with every telegram: 0x5555, 0xAAAA, 0x5555 .. (SAE J2819 5.1.2). */
static uint32_t send_broadcast(j2534_channel_t *c, const j2534_msg_t *msg,
                               uint32_t first, uint32_t count)
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

    for (uint32_t i = 0; i < count; i++)
    {
        uint8_t key = j2534_tp20_broadcast_key(first + i);

        wait_until(when);
        frame[dlc - 2] = key;
        frame[dlc - 1] = key;

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
    /* The message goes to the bus as written, nothing put in front of it
     * (SAE J2534-2 19.3.1); the id alone is one empty end-of-message
     * telegram. */
    const uint8_t *data = msg->data + 4;
    size_t total = msg->data_size - 4;
    size_t offset = 0;
    size_t block_off = 0;           /* where the block under way began */
    uint8_t block_seq = k->tx_seq;  /* the sequence number of its first telegram */
    uint32_t trouble = 0;           /* times this block was asked for again */
    uint8_t bs = (k->ecu_bs >= 1 && k->ecu_bs <= 15) ? k->ecu_bs : 15;

    /* Counted on the status, never refused. A VW ECU's message begins with
     * its two length bytes, so the service id is the third byte. */
    if (total > 2)
    {
        j2534_ch_note_reflash(data[2]);
    }

    k->in_write = true;
    k->ack_fresh = false;
    k->break_seen = false;

    /* Every telegram carries seven bytes but the message's last, so telegram
     * number j of a block starts at block_off + 7 j and has the sequence
     * number block_seq + j. That is what a "send again from" needs. The
     * last telegram always asks for an acknowledge, and the loop ends when
     * it has come. */
    for (;;)
    {
        unsigned in_block = (unsigned)((offset - block_off) / 7u);
        uint8_t seq = (uint8_t)((block_seq + in_block) & 0x0Fu);
        uint8_t n = (uint8_t)((total - offset) > 7 ? 7 : (total - offset));
        bool last = (offset + n) >= total;
        bool need = (in_block + 1u >= bs) || last;
        uint8_t op = need ? (last ? 0x1u : 0x0u) : (last ? 0x3u : 0x2u);
        uint8_t frame[8];
        uint8_t again_seq = 0;
        uint32_t st = J2534_ERR_TIMEOUT;
        int found;

        drain_bus(c);

        if (k->phase != TP_UP)
        {
            k->in_write = false;
            return J2534_ERR_TIMEOUT;
        }

        if (k->break_seen)
        {
            return end_after_break(c, k, seq);
        }

        frame[0] = (uint8_t)((op << 4) | seq);

        if (n > 0)
        {
            memcpy(frame + 1, data + offset, n);
        }

        k->ack_fresh = false;

        if (!send_paced(k, frame, (uint8_t)(1u + n)))
        {
            k->in_write = false;
            return J2534_ERR_FAILED;
        }

        if (!need)
        {
            offset += n;
            continue;
        }

        found = wait_block_ack(c, k, seq, frame, (uint8_t)(1u + n),
                               &again_seq, &st);

        if (found == TP_ACK_OK)
        {
            offset += n;
            block_off = offset;
            block_seq = (uint8_t)((seq + 1u) & 0x0Fu);
            k->tx_seq = block_seq;
            trouble = 0;

            if (last)
            {
                break;
            }
        }
        else if (found == TP_ACK_AGAIN)
        {
            /* The telegram named has to be one of this block, and a block
             * is asked for again MNTB times at most (SAE J2819 table 8). */
            int from = j2534_tp20_resend_from(seq, in_block, again_seq);

            trouble++;

            if (from < 0 || trouble > c->tp20->mntb)
            {
                fail_link(c, k, 1, true);
                k->in_write = false;
                return J2534_ERR_TIMEOUT;
            }

            offset = block_off + (size_t)from * 7u;
        }
        else if (found == TP_ACK_BREAK)
        {
            return end_after_break(c, k, (uint8_t)((seq + 1u) & 0x0Fu));
        }
        else
        {
            k->in_write = false;
            return st;
        }
    }

    k->in_write = false;
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
        return send_broadcast(c, msg, 0, TP_BROADCAST_N);
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

uint32_t j2534_tp20_write_periodic(struct j2534_channel *c,
                                   const j2534_msg_t *msg, uint32_t tick)
{
    if (c->tp20 == NULL || msg == NULL)
    {
        return J2534_ERR_INVALID_MSG;
    }

    if ((msg->tx_flags & J2534_TX_TP2_0_BROADCAST) == 0)
    {
        return j2534_tp20_write(c, msg, false);
    }

    /* The re-triggered broadcast (SAE J2534-2 19.3.2.3, SAE J2819 5.1.2):
     * the five at once, then ONE telegram per period, the key going on
     * where the five left it. */
    if (tick == 0)
    {
        return send_broadcast(c, msg, 0, TP_BROADCAST_N);
    }

    return send_broadcast(c, msg, TP_BROADCAST_N - 1u + tick, 1);
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
    uint32_t tx_a;
    uint32_t rx_a;
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
    /* BytePtr[6..7] is TX-ID-A, BytePtr[8..9] is RX-ID-A (SAE J2534-2 table
     * 78); bit 4 of the high byte says the id is not given. */
    tx_a = ((b[7] & 0x10) == 0) ? unpack_id(b[6], b[7]) : 0;
    rx_a = ((b[9] & 0x10) == 0) ? unpack_id(b[8], b[9]) : 0;

    if (dest_busy(c->tp20, dest) || id_busy(c->tp20, tx_a, NULL) ||
        id_busy(c->tp20, rx_a, NULL))
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
    /* The RX id asked for: a request that fails is reported with it (SAE
     * J2534-2 19.3.3.2). The channel acknowledge's own word replaces it. */
    k->rx_id = rx_a;
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
    k->deadline_us = esp_timer_get_time() + (int64_t)c->tp20->t_e * 1000;
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
