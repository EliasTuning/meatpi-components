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
 * @file uds_manager.c
 * @brief UDS protocol over a selectable transport: backend select, the
 *        0x78 responsePending loop, NRC decode, tester-present, and the
 *        one-transaction-at-a-time claim.
 */
#include "uds_manager.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "can_isotp.h"
#include "can_isotp_esp.h"
#include "can_manager.h"
#include "obd_gate.h"
#include "log_manager.h"

#include "uds_manager_private.h"
#include "uds_proto.h"
#include "uds_transport.h"

static const char *TAG = "uds_manager";

/* ---- state ---------------------------------------------------------------- */

static bool s_started;

/* one transaction / session at a time */
static SemaphoreHandle_t s_claim;
static StaticSemaphore_t s_claim_buf;

/* active tester-present session */
static esp_timer_handle_t s_tp_timer;
static uds_addr_t s_tp_addr;
static bool s_session;

/* the "exclusive" option (obd_gate's diagnostics hold): asserted on use,
 * released UDS_EXCLUSIVE_IDLE_MS after the last request unless a session
 * (tester present) is open; runtime-switchable, boot default = setting */
static volatile bool s_exclusive;
static volatile bool s_holding;
static const int s_diag_token;
static esp_timer_handle_t s_excl_timer;

/* the last transaction, for GET /api/uds */
static int64_t s_last_ts_us;
static esp_err_t s_last_err;
static uint32_t s_last_tx_id;
static uint32_t s_last_rx_id;
static uint8_t s_last_req_sid;
static uds_result_t s_last_res;

/* ---- backend resolution ---------------------------------------------------- */

const char *uds_manager_backend_name(uds_backend_t b)
{
    switch (b)
    {
    case UDS_BACKEND_OBD_CHIP: return "obd_chip";
    case UDS_BACKEND_ISOTP:    return "isotp";
    default:                   return "auto";
    }
}

static const uds_transport_t *resolve_transport(void)
{
    bool can_up = (can_manager_core_handle() != NULL);
    /* fw ISO-TP needs the bus AND a registered provider (can_isotp.h) */
    bool isotp_ok = can_up && (can_isotp() != NULL);

    switch (uds_settings_config()->backend)
    {
    case UDS_BACKEND_OBD_CHIP:
        return uds_transport_obd();

    case UDS_BACKEND_ISOTP:
        if (isotp_ok) return uds_transport_isotp();
        ESP_LOGW(TAG, "isotp backend forced but unavailable (CAN down "
                      "or no ISO-TP provider) -> obd_chip");
        return uds_transport_obd();

    case UDS_BACKEND_AUTO:
    default:
        /* isotp when available (most capable), else the MIC */
        return isotp_ok ? uds_transport_isotp() : uds_transport_obd();
    }
}

uds_backend_t uds_manager_active_backend(void)
{
    const uds_transport_t *t = resolve_transport();

    if (t == uds_transport_isotp())                          return UDS_BACKEND_ISOTP;
    return UDS_BACKEND_OBD_CHIP;
}

/* ---- one transaction (assumes the claim is held) --------------------------- */

static esp_err_t do_transaction(const uds_transport_t *t,
                                const uds_addr_t *addr,
                                const uint8_t *req, size_t req_len,
                                uint8_t *resp, size_t resp_cap,
                                size_t *resp_len, const uds_opts_t *opts,
                                uds_result_t *result)
{
    const uds_config_t *cfg = uds_settings_config();
    uint32_t p2 = (opts && opts->p2_ms) ? opts->p2_ms : cfg->p2_ms;
    uint32_t p2star = (opts && opts->p2star_ms) ? opts->p2star_ms
                                                : cfg->p2star_ms;
    uint8_t max_pending = (opts && opts->max_pending) ? opts->max_pending
                                                      : 20;

    (void)max_pending; /* the transport caps its own pending loop */

    int64_t t0 = esp_timer_get_time();
    uint8_t pending = 0;
    esp_err_t err;

    /* the shared bus has a second requester (autopid through the MIC): a
       reply that does not belong to OUR SID is a stray — a frame left on
       the bus by another conversation that landed in our window (bench
       2026-09-16: `41 0C ..` for `3E 00`, `62 01 02` for a `22 01 01`).
       The transaction hold stops NEW interleaving; a stray from a frame
       already in flight when we grabbed the chip is retried within p2*,
       with a fresh window each time. */
    for (int attempt = 0; ; attempt++)
    {
        /* the transport delivers the FINAL response (consuming 0x78
         * responsePending frames itself, with p2star per frame) */
        err = t->transceive(addr, req, req_len, resp, resp_cap, resp_len,
                            p2, p2star, &pending);

        if (err != ESP_OK || req_len < 1 ||
            uds_response_matches(req[0], resp, *resp_len))
        {
            break; /* our answer, or a hard transport error */
        }

        int64_t elapsed_us = esp_timer_get_time() - t0;

        ESP_LOGW(TAG, "stray response %02X.. to request %02X (attempt %d, "
                      "another requester on the bus?)",
                 (*resp_len >= 1) ? resp[0] : 0u, req[0], attempt + 1);

        if (attempt >= 2 || elapsed_us > (int64_t)p2star * 1000)
        {
            err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
    }

    if (result != NULL)
    {
        memset(result, 0, sizeof(*result));
        result->backend = t->name;
        result->pending_count = pending;
        result->elapsed_ms =
            (uint32_t)((esp_timer_get_time() - t0) / 1000);

        if (err == ESP_OK && *resp_len >= 1)
        {
            result->sid = resp[0];
            result->negative = uds_is_negative(resp, *resp_len);

            if (result->negative)
            {
                result->nrc = uds_nrc_of(resp, *resp_len);
                result->nrc_name = uds_nrc_name(result->nrc);
                result->sid = (*resp_len >= 2) ? resp[1] : resp[0];
            }
        }
    }

    return err;
}

/* ---- the exclusive option --------------------------------------------------- */

static void excl_timer_cb(void *arg)
{
    (void)arg;

    if (s_session)
    {
        /* a held session keeps the bus: look again later */
        (void)esp_timer_start_once(s_excl_timer,
                                   (uint64_t)UDS_EXCLUSIVE_IDLE_MS * 1000);
        return;
    }

    if (s_holding)
    {
        s_holding = false;
        obd_gate_diag_hold(&s_diag_token, false);
    }
}

void uds_excl_touch(void)
{
    if (!s_exclusive)
    {
        return;
    }

    if (s_excl_timer == NULL)
    {
        const esp_timer_create_args_t a =
        {
            .callback = excl_timer_cb,
            .name     = "uds_excl",
        };

        if (esp_timer_create(&a, &s_excl_timer) != ESP_OK)
        {
            return;
        }
    }

    if (!s_holding)
    {
        s_holding = true;
        obd_gate_diag_hold(&s_diag_token, true);
        /* the poller confirms it is off the bus (it loops within 500 ms)
           before our first request goes out */
        (void)obd_gate_diag_wait_ack(700);
    }

    (void)esp_timer_stop(s_excl_timer);
    (void)esp_timer_start_once(s_excl_timer,
                               (uint64_t)UDS_EXCLUSIVE_IDLE_MS * 1000);
}

static void excl_release_now(void)
{
    if (s_excl_timer != NULL)
    {
        (void)esp_timer_stop(s_excl_timer);
    }

    if (s_holding)
    {
        s_holding = false;
        obd_gate_diag_hold(&s_diag_token, false);
    }
}

void uds_manager_set_exclusive(bool on)
{
    s_exclusive = on;

    if (!on)
    {
        excl_release_now();
    }
    else if (s_session)
    {
        uds_excl_touch(); /* an open session takes the bus at once */
    }
}

bool uds_manager_exclusive(void)
{
    return s_exclusive;
}

void uds_manager_get_status(uds_status_t *out)
{
    if (out == NULL)
    {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->backend_setting = uds_settings_config()->backend;
    out->backend_active = uds_manager_active_backend();
    out->can_running = (can_manager_core_handle() != NULL);
    out->provider = can_isotp_esp_active() ? "esp_isotp"
                    : (can_isotp() != NULL) ? "add-on" : "none";
    out->exclusive = s_exclusive;
    out->exclusive_default = uds_settings_config()->exclusive;
    out->holding = s_holding;
    out->autopid_paused = obd_gate_diag_held() && obd_gate_diag_acked();
    out->session_active = s_session;
    out->last_ts_us = s_last_ts_us;
    out->last_err = s_last_err;
    out->last_tx_id = s_last_tx_id;
    out->last_rx_id = s_last_rx_id;
    out->last_req_sid = s_last_req_sid;
    out->last = s_last_res;
}

/* ---- public request -------------------------------------------------------- */

esp_err_t uds_request(const uds_addr_t *addr,
                      const uint8_t *req, size_t req_len,
                      uint8_t *resp, size_t resp_cap, size_t *resp_len,
                      const uds_opts_t *opts, uds_result_t *result)
{
    if (!s_started)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (addr == NULL || req == NULL || req_len == 0 || resp == NULL ||
        resp_len == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    uds_result_t local;

    if (result == NULL)
    {
        result = &local; /* recorded as the last transaction below */
    }

    /* a held session owns the claim already (uds_session_begin) */
    bool own_claim = !s_session;

    if (own_claim &&
        xSemaphoreTake(s_claim, pdMS_TO_TICKS(200)) != pdTRUE)
    {
        return ESP_ERR_INVALID_STATE; /* busy */
    }

    uds_excl_touch(); /* exclusive option: autopid off the bus, now */

    const uds_transport_t *t = resolve_transport();
    esp_err_t err = t->open();

    if (err == ESP_OK)
    {
        err = do_transaction(t, addr, req, req_len, resp, resp_cap,
                             resp_len, opts, result);
    }
    else
    {
        memset(result, 0, sizeof(*result));
        result->backend = t->name;
    }

    s_last_ts_us = esp_timer_get_time();
    s_last_err = err;
    s_last_tx_id = addr->tx_id;
    s_last_rx_id = addr->rx_id;
    s_last_req_sid = req[0];
    s_last_res = *result;

    if (own_claim)
    {
        xSemaphoreGive(s_claim);
    }

    return err;
}

/* ---- tester-present session ------------------------------------------------ */

static void tp_timer_cb(void *arg)
{
    (void)arg;

    if (!s_session)
    {
        return;
    }

    /* 3E 80 = TesterPresent, suppressPositiveResponse — fire and forget */
    const uint8_t tp[] = { 0x3E, 0x80 };
    uint8_t r[8];
    size_t rn = 0;
    const uds_transport_t *t = resolve_transport();

    (void)t->transceive(&s_tp_addr, tp, sizeof(tp), r, sizeof(r), &rn,
                        100, 100, NULL);
}

esp_err_t uds_session_begin(const uds_addr_t *addr)
{
    if (!s_started || addr == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_claim, pdMS_TO_TICKS(200)) != pdTRUE)
    {
        return ESP_ERR_INVALID_STATE; /* busy */
    }

    const uds_transport_t *t = resolve_transport();

    if (t->open() != ESP_OK)
    {
        xSemaphoreGive(s_claim);
        return ESP_ERR_INVALID_STATE;
    }

    s_tp_addr = *addr;
    s_session = true;
    uds_excl_touch(); /* exclusive option: the session holds the bus */

    if (s_tp_timer != NULL)
    {
        esp_timer_start_periodic(
            s_tp_timer,
            (uint64_t)uds_settings_config()->tester_present_ms * 1000);
    }

    return ESP_OK;
}

esp_err_t uds_session_end(void)
{
    if (!s_session)
    {
        return ESP_OK;
    }

    if (s_tp_timer != NULL)
    {
        esp_timer_stop(s_tp_timer);
    }

    s_session = false;
    xSemaphoreGive(s_claim);

    if (s_holding)
    {
        uds_excl_touch(); /* re-arm the idle release */
    }

    return ESP_OK;
}

/* ---- lifecycle ------------------------------------------------------------- */

esp_err_t uds_manager_init(void)
{
    static const log_descriptor_t LOG_DESC = { "uds_manager", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);
    uds_events_register();  /* uds.request action + uds.response source */

    if (s_claim == NULL)
    {
        s_claim = xSemaphoreCreateMutexStatic(&s_claim_buf);
    }

    return uds_settings_register();
}

esp_err_t uds_manager_start(void)
{
    const uds_config_t *cfg = uds_settings_config();

    if (!uds_settings_is_configured())
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_started)
    {
        return ESP_OK;
    }

    const esp_timer_create_args_t targs =
    {
        .callback = tp_timer_cb,
        .name     = "uds_tp",
    };

    (void)esp_timer_create(&targs, &s_tp_timer);

    s_exclusive = cfg->exclusive;
    s_started = true;
    ESP_LOGI(TAG, "started (backend=%s, p2=%lu p2*=%lu, exclusive=%d)",
             uds_manager_backend_name(cfg->backend),
             (unsigned long)cfg->p2_ms, (unsigned long)cfg->p2star_ms,
             cfg->exclusive);
    return ESP_OK;
}

esp_err_t uds_manager_stop(void)
{
    (void)uds_session_end();
    excl_release_now();
    s_started = false;
    return ESP_OK;
}
