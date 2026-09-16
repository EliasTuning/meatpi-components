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
 * @file uds_transport_isotp.c
 * @brief backend "isotp" — RAW UDS PDU over the registered ISO-TP
 *        provider (can_isotp.h) on can_manager's native CAN. The most
 *        capable path: multi-frame UDS, no ELM round-trips. One
 *        session, re-opened when the address changes. Without a
 *        provider (stock build) open() fails and uds_manager falls
 *        back to the obd_chip transport. An ESP-side requester on the
 *        shared bus: every transaction holds obd_gate (request ->
 *        final response) so the MIC chip (autopid) cannot interleave.
 */
#include "uds_transport.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "can_isotp.h"
#include "can_manager.h"
#include "obd_gate.h"
#include "uds_manager_private.h"
#include "uds_proto.h"

static const char *TAG = "uds_manager";

static can_isotp_session_t s_sess;
static uds_addr_t s_addr;   /* the address the session is bound to */
static bool s_bound;

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static bool s_up;
static const int s_gate_token; /* obd_gate owner identity */

/* The provider allows ONE session per rx id. Holding ours forever after
 * the last request would lock a J2534 ISO15765 channel out of that ECU
 * (bench 2026-09-16: a UDS probe of 7E2/7EA made the J2534 CONNECT to
 * the same pair fail). So the session is released after this much
 * idle; the tester-present timer re-binds within it during a UDS
 * session, a new request re-binds on demand (~1 ms). */
#define ISOTP_IDLE_CLOSE_MS 5000
static esp_timer_handle_t s_idle_timer;

static esp_err_t isotp_open(void)
{
    if (s_up)
    {
        return ESP_OK;
    }

    if (can_isotp() == NULL)
    {
        ESP_LOGE(TAG, "isotp backend not available in this build "
                      "(no ISO-TP provider registered)");
        return ESP_ERR_INVALID_STATE;
    }

    if (can_manager_core_handle() == NULL)
    {
        ESP_LOGE(TAG, "isotp backend needs can_manager enabled + running");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }

    s_up = true;
    return ESP_OK;
}

static void unbind_locked(void)
{
    if (s_bound)
    {
        can_isotp()->close(s_sess);
        s_bound = false;
    }
}

/* esp_timer task: skip (and retry later) while a transaction holds
   the lock — never block the timer task */
static void idle_close_cb(void *arg)
{
    (void)arg;

    if (s_lock == NULL || xSemaphoreTake(s_lock, 0) != pdTRUE)
    {
        if (s_idle_timer != NULL)
        {
            (void)esp_timer_start_once(
                s_idle_timer, (uint64_t)ISOTP_IDLE_CLOSE_MS * 1000);
        }

        return;
    }

    unbind_locked();
    xSemaphoreGive(s_lock);
}

/* (re)arm the idle close after every transaction */
static void touch_idle_timer(void)
{
    if (s_idle_timer == NULL)
    {
        const esp_timer_create_args_t a =
        {
            .callback = idle_close_cb,
            .name     = "uds_isotp_idle",
        };

        if (esp_timer_create(&a, &s_idle_timer) != ESP_OK)
        {
            return;
        }
    }

    (void)esp_timer_stop(s_idle_timer);
    (void)esp_timer_start_once(s_idle_timer,
                               (uint64_t)ISOTP_IDLE_CLOSE_MS * 1000);
}

static esp_err_t bind_addr(const uds_addr_t *addr)
{
    if (s_bound && s_addr.tx_id == addr->tx_id &&
        s_addr.rx_id == addr->rx_id && s_addr.ext_id == addr->ext_id)
    {
        return ESP_OK; /* already bound to this ECU */
    }

    unbind_locked();

    can_isotp_cfg_t cfg =
    {
        .tx_id       = addr->tx_id,
        .rx_id       = addr->rx_id,
        .ext_id      = addr->ext_id,
        .block_size  = 0,
        .stmin_ms    = 0,
        .padding_byte = 0x00,
        .use_padding = true,
    };

    if (can_isotp()->open(&cfg, &s_sess) != ESP_OK)
    {
        ESP_LOGE(TAG, "isotp open failed (tx %03lX rx %03lX)",
                 (unsigned long)addr->tx_id, (unsigned long)addr->rx_id);
        return ESP_FAIL;
    }

    s_addr = *addr;
    s_bound = true;
    return ESP_OK;
}

static esp_err_t isotp_transceive(const uds_addr_t *addr,
                                  const uint8_t *req, size_t req_len,
                                  uint8_t *resp, size_t resp_cap,
                                  size_t *resp_len, uint32_t p2_ms,
                                  uint32_t p2star_ms, uint8_t *pending_out)
{
    if (!s_up)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (pending_out != NULL)
    {
        *pending_out = 0;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(p2_ms + p2star_ms + 500)) !=
        pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = bind_addr(addr);

    if (err != ESP_OK)
    {
        xSemaphoreGive(s_lock);
        return err;
    }

    /* an ESP-side requester on the shared bus: hold the conversation
       gate for the whole request -> final response so the MIC chip
       (autopid) does not interleave; fail-open + self-expiring
       (obd_gate.h), so a stuck side can never brick the other */
    obd_gate_acquire(&s_gate_token, OBD_GATE_WAIT_MS);

    /* Drain any stale reassembled message before sending — otherwise a
     * late/previous response (or bus cross-talk on this rx_id) is read
     * back immediately and every reply comes out shifted by one. A large
     * stale message returns OVERFLOW into our small buffer but is still
     * consumed, so keep draining on OVERFLOW too; stop on TIMEOUT. */
    {
        uint8_t junk[16];
        size_t jn = 0;

        for (int i = 0; i < 8; i++)
        {
            esp_err_t dr = can_isotp()->recv(s_sess, junk,
                                             sizeof(junk), &jn, 0);

            if (dr != ESP_OK && dr != ESP_ERR_NO_MEM)
            {
                break;
            }
        }
    }

    esp_err_t r = can_isotp()->send(s_sess, req, req_len, p2_ms);

    if (r != ESP_OK)
    {
        obd_gate_release(&s_gate_token);
        touch_idle_timer();
        xSemaphoreGive(s_lock);
        return (r == ESP_ERR_TIMEOUT) ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }

    /* receive the response; consume 0x78 responsePending frames (the
     * ECU sends the final response after them, within P2*) */
    size_t got = 0;
    uint32_t to = p2_ms;
    uint8_t pending = 0;

    for (;;)
    {
        r = can_isotp()->recv(s_sess, resp, resp_cap, &got, to);

        if (r != ESP_OK)
        {
            break;
        }

        if (!uds_is_pending(resp, got) || pending >= 20)
        {
            break;
        }

        pending++;
        to = p2star_ms; /* extended window while pending */
    }

    obd_gate_release(&s_gate_token);
    touch_idle_timer();
    xSemaphoreGive(s_lock);

    if (pending_out != NULL)
    {
        *pending_out = pending;
    }

    if (r != ESP_OK)
    {
        /* ESP_ERR_TIMEOUT / ESP_ERR_NO_MEM (overflow) pass through */
        return (r == ESP_ERR_TIMEOUT || r == ESP_ERR_NO_MEM) ? r : ESP_FAIL;
    }

    *resp_len = got;
    return ESP_OK;
}

const uds_transport_t *uds_transport_isotp(void)
{
    static const uds_transport_t T =
    {
        .name       = "isotp",
        .open       = isotp_open,
        .transceive = isotp_transceive,
    };

    return &T;
}

/* ---- raw PDU surface (script obd_isotp_tx/rx — no UDS semantics) ----------- */

esp_err_t uds_isotp_tx(const uds_addr_t *addr,
                       const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (addr == NULL || data == NULL || len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = isotp_open();

    if (err != ESP_OK)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(timeout_ms + 500)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    err = bind_addr(addr);

    if (err == ESP_OK)
    {
        uds_excl_touch(); /* exclusive option: a raw PDU is use too */
        obd_gate_acquire(&s_gate_token, OBD_GATE_WAIT_MS);

        esp_err_t r = can_isotp()->send(s_sess, data, len, timeout_ms);

        err = (r == ESP_OK)          ? ESP_OK :
              (r == ESP_ERR_TIMEOUT) ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }

    touch_idle_timer();
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t uds_isotp_rx(const uds_addr_t *addr,
                       uint8_t *out, size_t cap, size_t *out_len,
                       uint32_t timeout_ms)
{
    if (addr == NULL || out == NULL || cap == 0 || out_len == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = isotp_open();

    if (err != ESP_OK)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(timeout_ms + 500)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    err = bind_addr(addr);

    if (err == ESP_OK)
    {
        size_t got = 0;
        esp_err_t r = can_isotp()->recv(s_sess, out, cap, &got,
                                        timeout_ms);

        if (r == ESP_OK)
        {
            *out_len = got;
            err = ESP_OK;
        }
        else
        {
            err = (r == ESP_ERR_TIMEOUT) ? ESP_ERR_TIMEOUT :
                  (r == ESP_ERR_NO_MEM)  ? ESP_ERR_NO_MEM : ESP_FAIL;
        }
    }

    obd_gate_release(&s_gate_token); /* the raw conversation is over */
    touch_idle_timer();
    xSemaphoreGive(s_lock);
    return err;
}
