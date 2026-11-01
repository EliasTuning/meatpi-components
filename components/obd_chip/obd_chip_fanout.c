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
 * @file obd_chip_fanout.c
 * @brief The subscriber registry, the RX fan-out and the raw TX path
 *        (obd_chip_send with the EEPROM guard). Split out of obd_chip.c on
 *        2026-11-01 (700-line rule) together with the app-reset interception
 *        the two paths share.
 *
 * Fan-out contract (README): every subscriber gets every chunk; a full queue
 * drops that subscriber's copy (counted, rate-limited WARN) and never stalls
 * the RX task or other subscribers. The one exception is deliberate: the
 * answer to the ATM0 the driver writes behind an app's reset (below) reaches
 * nobody.
 */
#include "obd_chip.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

#include "obd_gate.h"

#include "obd_chip_private.h"

static const char *TAG = "obd_chip"; /* one TAG per component (§10) */

#define OBD_MAX_SUBSCRIBERS 8

typedef struct
{
    QueueHandle_t q;
    const char   *name;
    uint32_t      dropped;
    uint32_t      dropped_logged;
} obd_sub_t;

/* registry + state: PSRAM .bss (§2) */
static obd_sub_t s_subs[OBD_MAX_SUBSCRIBERS] EXT_RAM_BSS_ATTR;
static size_t s_sub_count;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED; /* internal: spinlock (the registry) */

/* RX fan-out accounting (GET /api/obd_chip): what the chip printed vs
 * what each subscriber managed to take - the first hop of any "missed
 * frames" question (ATMA floods, 8 KB VT responses). RX-task-only writes. */
static uint32_t s_rx_bytes;
static uint32_t s_rx_chunks;
static uint16_t s_rx_max_chunk;

/* EEPROM guard accounting (obd_chip_guard.h) */
static uint32_t s_guard_rewrites;
static uint32_t s_guard_blocked;
static uint32_t s_memory_off; /* ATM0 sent and acknowledged (issue #1023) */

/* An app's reset word on the bridge path (issue #1023, 2026-11-01, Ali):
 * the chip comes out of ATZ / ATWS / ATD with its memory function ON, and
 * the next protocol switch the app makes and connects on would then be
 * written to the chip's non-volatile protocol store. The bridge path cannot
 * carry an extra word the app would see answered, so: the app's ATZ goes
 * out as ATWS (the same banner, no 1 s LED test; what 4.x did with every
 * ATZ), and when the reset's prompt passes through the fan-out the driver
 * writes ATM0 and HIDES its answer from every subscriber up to the next
 * prompt, the obd_gate held across both. RX-task state but for the arming,
 * which the sending task does right before its write. */
typedef enum
{
    INJECT_IDLE = 0,
    INJECT_ARMED,   /* a reset word went out: ATM0 behind its prompt   */
    INJECT_HIDING,  /* ATM0 went out: its answer is nobody's business  */
} inject_t;

static volatile inject_t s_inject;
static int64_t  s_inject_us;    /* armed / hiding since                    */
static uint8_t  s_hide_prev;    /* the hidden byte before this one         */
static bool     s_hide_ok;      /* the hidden answer carried OK            */
static uint32_t s_app_resets;   /* reset words intercepted (stats)         */
static uint32_t s_hidden_bytes; /* kept from the fan-out (stats)           */

#define INJECT_ARM_MAX_US  (3 * 1000 * 1000) /* the reset never answered  */
#define INJECT_HIDE_MAX_US (1 * 1000 * 1000) /* the ATM0 never answered   */

/* ---- fan-out ------------------------------------------------------------------ */

/** One chunk to every subscriber (the broadcast contract). */
static void fanout_chunk(const obd_chunk_t *chunk)
{
    s_rx_bytes += chunk->len;
    s_rx_chunks++;

    if (chunk->len > s_rx_max_chunk)
    {
        s_rx_max_chunk = chunk->len;
    }

    for (size_t i = 0; i < s_sub_count; i++)
    {
        if (s_subs[i].q == NULL)
        {
            continue;
        }

        if (xQueueSend(s_subs[i].q, chunk, 0) != pdTRUE)
        {
            s_subs[i].dropped++;

            /* rate limit: one WARN per 100 drops per subscriber (§10) */
            if (s_subs[i].dropped - s_subs[i].dropped_logged >= 100)
            {
                s_subs[i].dropped_logged = s_subs[i].dropped;
                ESP_LOGW(TAG, "subscriber '%s' dropped %lu chunks",
                         s_subs[i].name, (unsigned long)s_subs[i].dropped);
            }
        }
    }
}

void obd_core_fanout(const uint8_t *data, size_t len)
{
    static uint8_t s_prev = '\n'; /* RX-task-only: the byte before */
    obd_chunk_t chunk;
    bool gate = obd_gate_enabled();
    int64_t now = esp_timer_get_time();

    /* the reset interception's failsafes: a reset the chip never answered
       (arm again at the next one), an ATM0 whose prompt never came (the
       chip went away under it: let the bytes through again) */
    if (s_inject == INJECT_ARMED && now - s_inject_us > INJECT_ARM_MAX_US)
    {
        s_inject = INJECT_IDLE;
    }
    else if (s_inject == INJECT_HIDING &&
             now - s_inject_us > INJECT_HIDE_MAX_US)
    {
        ESP_LOGW(TAG, "the ATM0 behind an app's reset got no prompt in "
                      "%d ms", INJECT_HIDE_MAX_US / 1000);
        s_inject = INJECT_IDLE;

        if (gate)
        {
            obd_gate_release(obd_chip_gate_owner);
        }
    }

    chunk.len = 0;

    for (size_t i = 0; i < len; i++)
    {
        uint8_t b = data[i];

        /* the '>' prompt at LINE START only: response DATA may contain
           '>' mid-line (STSLCS, see obd_chip_parse.c) */
        bool prompt = (b == '>' && (s_prev == '\r' || s_prev == '\n'));

        s_prev = b;

        if (s_inject == INJECT_HIDING)
        {
            /* the ATM0's echo and its OK: kept from every subscriber */
            s_hidden_bytes++;

            if (b == 'K' && s_hide_prev == 'O')
            {
                s_hide_ok = true;
            }

            s_hide_prev = b;

            if (prompt)
            {
                s_inject = INJECT_IDLE;

                if (s_hide_ok)
                {
                    s_memory_off++;
                }
                else
                {
                    ESP_LOGD(TAG, "the ATM0 behind an app's reset: no OK");
                }

                if (gate)
                {
                    obd_gate_release(obd_chip_gate_owner);
                }
            }

            continue;
        }

        if (chunk.len == OBD_CHIP_CHUNK_SIZE)
        {
            fanout_chunk(&chunk);
            chunk.len = 0;
        }

        chunk.data[chunk.len++] = b;

        if (!prompt)
        {
            continue;
        }

        if (s_inject == INJECT_ARMED)
        {
            /* the app's reset answered (the banner, or ATD's OK): ATM0 at
               once, before the app can react to the prompt, the gate kept
               until the ATM0's own prompt has passed */
            static const uint8_t m0[] = "ATM0\r";

            s_inject = INJECT_HIDING;
            s_inject_us = now;
            s_hide_prev = 0;
            s_hide_ok = false;
            (void)obd_uart_write(m0, sizeof(m0) - 1);
        }
        else if (gate)
        {
            /* obd_gate: the chip's conversation window ends here. Release
               is idempotent; the max-hold expiry covers monitor-class
               commands that never print a prompt. */
            obd_gate_release(obd_chip_gate_owner);
        }
    }

    if (chunk.len > 0)
    {
        fanout_chunk(&chunk);
    }
}

/* ---- the registry --------------------------------------------------------------- */

esp_err_t obd_chip_subscribe(QueueHandle_t q, const char *name)
{
    if (q == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_ERR_NO_MEM;

    portENTER_CRITICAL(&s_lock);

    for (size_t i = 0; i < OBD_MAX_SUBSCRIBERS; i++)
    {
        if (s_subs[i].q == NULL)
        {
            s_subs[i].q = q;
            s_subs[i].name = (name != NULL) ? name : "?";
            s_subs[i].dropped = 0;
            s_subs[i].dropped_logged = 0;

            if (i >= s_sub_count)
            {
                s_sub_count = i + 1;
            }

            err = ESP_OK;
            break;
        }
    }

    portEXIT_CRITICAL(&s_lock);
    return err;
}

esp_err_t obd_chip_unsubscribe(QueueHandle_t q)
{
    esp_err_t err = ESP_ERR_NOT_FOUND;

    portENTER_CRITICAL(&s_lock);

    for (size_t i = 0; i < s_sub_count; i++)
    {
        if (s_subs[i].q == q)
        {
            s_subs[i].q = NULL;
            err = ESP_OK;
            break;
        }
    }

    portEXIT_CRITICAL(&s_lock);
    return err;
}

uint32_t obd_chip_dropped(QueueHandle_t q)
{
    for (size_t i = 0; i < s_sub_count; i++)
    {
        if (s_subs[i].q == q)
        {
            return s_subs[i].dropped;
        }
    }

    return 0;
}

size_t obd_chip_get_subscribers(obd_chip_sub_stats_t *out, size_t max)
{
    size_t n = 0;

    for (size_t i = 0; i < s_sub_count && n < max; i++)
    {
        QueueHandle_t q = s_subs[i].q;

        if (q == NULL)
        {
            continue;
        }

        out[n].name = s_subs[i].name;
        out[n].dropped = s_subs[i].dropped;
        out[n].queued = (uint32_t)uxQueueMessagesWaiting(q);
        out[n].depth = out[n].queued + (uint32_t)uxQueueSpacesAvailable(q);
        n++;
    }

    return n;
}

/* ---- the accounting (obd_chip_get_stats) ---------------------------------------- */

void obd_core_fanout_stats(obd_fanout_stats_t *out)
{
    out->rx_bytes = s_rx_bytes;
    out->rx_chunks = s_rx_chunks;
    out->rx_max_chunk = s_rx_max_chunk;
    out->guard_rewrites = s_guard_rewrites;
    out->guard_blocked = s_guard_blocked;
    out->memory_off = s_memory_off;
    out->app_resets = s_app_resets;
    out->hidden_bytes = s_hidden_bytes;
}

void obd_core_guard_note(obd_guard_t v, const char *cmd, size_t len)
{
    char head[40];
    size_t n = 0;

    for (size_t i = 0; i < len && n < sizeof(head) - 1; i++)
    {
        char c = cmd[i];

        head[n++] = (c == '\r' || c == '\n') ? ' ' : c;
    }

    head[n] = '\0';

    if (v == OBD_GUARD_BLOCKED)
    {
        s_guard_blocked++;
        ESP_LOGW(TAG, "EEPROM guard refused '%s' (ATPP/ATSD/ATCV/STWBR "
                      "write the chip's EEPROM)", head);
    }
    else if (v == OBD_GUARD_REWRITTEN)
    {
        s_guard_rewrites++;
        ESP_LOGD(TAG, "EEPROM guard rewrote '%s' (ATSP->ATTP, ATM1->ATM0)",
                 head);
    }
}

void obd_core_memory_off_note(void)
{
    s_memory_off++;
}

/* ---- TX ------------------------------------------------------------------------- */

esp_err_t obd_chip_send(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* a write mid-bring-up would interleave with the bare-UART probes:
       hold early boot-window clients until the chip is negotiated (a
       zero-cost flag test any time after) */
    if (!obd_core_bringup_wait(OBD_BRINGUP_WAIT_MS))
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (obd_core_exclusive_held())
    {
        return ESP_ERR_INVALID_STATE; /* fw update owns the wire */
    }

    /* EEPROM guard (obd_chip_guard.h): apps re-send ATSP on every connect
       and a terminal user can type ATPP, rewrite the two with RAM twins
       in place, refuse the rest with the ELM "?" the sender expects, so
       the chip never sees an EEPROM write from a bridge. */
    uint8_t copy[256];
    obd_guard_t gv = obd_chip_guard_check((const char *)data, len);

    if (gv == OBD_GUARD_REWRITTEN && len > sizeof(copy))
    {
        gv = OBD_GUARD_BLOCKED; /* no scratch for a rewrite that long */
    }

    if (gv != OBD_GUARD_PASS)
    {
        obd_core_guard_note(gv, (const char *)data, len);
    }

    if (gv == OBD_GUARD_BLOCKED)
    {
        /* straight to the subscribers: this reply never went through the
           chip, so neither the gate (not taken for it) nor the reset
           interception's window (RX-task state) is its business */
        static const obd_chunk_t refused = { 4, "?\r\r>" };

        fanout_chunk(&refused);
        return ESP_OK;
    }

    if (gv == OBD_GUARD_REWRITTEN)
    {
        memcpy(copy, data, len);
        (void)obd_chip_guard_cmd((char *)copy, len);
        data = copy;
    }

    /* an app's reset word as the whole chunk (ATZ / ATWS / ATD): ATZ goes
       out as ATWS (the same banner, about 40 ms instead of 1.2 s on the
       bench chip), and the fan-out puts a hidden ATM0 behind the answer
       (obd_core_fanout above) so the app's session runs with the chip's
       memory function off, as 4.x's did */
    static const uint8_t ATWS_CR[] = "ATWS\r";
    obd_reset_t rw = obd_parse_reset_word((const char *)data, len);

    if (rw == OBD_RESET_ATZ)
    {
        data = ATWS_CR;
        len = sizeof(ATWS_CR) - 1;
    }

    if (rw != OBD_RESET_NONE)
    {
        s_app_resets++;
    }

    /* obd_gate: a CR submits a command to the chip, that opens a bus
       conversation, so serialize against the ESP-side ELM engines. May
       block up to OBD_GATE_WAIT_MS; released when the RX fan-out sees
       the '>' prompt (or by the max-hold failsafe). */
    if (memchr(data, '\r', len) != NULL)
    {
        (void)obd_gate_acquire(obd_chip_gate_owner, OBD_GATE_WAIT_MS);
    }

    if (rw != OBD_RESET_NONE)
    {
        /* armed before the write: the answer's prompt can pass through
           the fan-out before this task is back from the UART */
        s_inject_us = esp_timer_get_time();
        s_inject = INJECT_ARMED;
    }

    return obd_uart_write(data, len);
}
