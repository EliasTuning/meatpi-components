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
 * @file uds_manager.h
 * @brief UDS (ISO 14229) request/response over a SELECTABLE transport —
 *        the MIC3624 (obd_chip) or firmware ISO-TP over native CAN
 *        (isotp / can_manager).
 *
 * Model: uds_manager owns the UDS protocol (0x78 responsePending loop,
 * NRC decode, tester-present keepalive, one-transaction-at-a-time
 * claim); the transport is a thin vtable so both backends get the
 * protocol for free. Consumers: the UDS terminal (`uds` CLI +
 * POST /api/uds/request) and the Berry `script_engine`.
 *
 * Settings ("uds_manager", reboot-to-apply): backend
 * (auto|obd_chip|isotp, default auto = isotp when can_manager is
 * running else obd_chip), p2_ms, p2star_ms, tester_present_ms, cli.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** UDS addressing for one request (a UDS PDU carries its own header). */
typedef struct
{
    uint32_t tx_id;   /**< request CAN ID (tester → ECU)   */
    uint32_t rx_id;   /**< response CAN ID (ECU → tester)  */
    bool     ext_id;  /**< true = 29-bit extended IDs      */
} uds_addr_t;

/** Per-request options (0 = use the component defaults). */
typedef struct
{
    uint32_t p2_ms;       /**< first-response timeout (default 250)     */
    uint32_t p2star_ms;   /**< timeout after each 0x78 (default 5000)   */
    uint8_t  max_pending; /**< max 0x78 before giving up (default 20)   */
} uds_opts_t;

/** Outcome of a UDS transaction (the response bytes go in the caller's
 *  buffer; this describes them). */
typedef struct
{
    bool     negative;      /**< true = 7F <sid> <nrc> final response  */
    uint8_t  sid;           /**< echoed service id (request[0])        */
    uint8_t  nrc;           /**< negative response code (if negative)  */
    const char *nrc_name;   /**< ISO 14229 name (static; "" if none)   */
    uint8_t  pending_count; /**< number of 0x78 responsePending seen   */
    uint32_t elapsed_ms;    /**< wall time of the transaction          */
    const char *backend;    /**< which transport served it             */
} uds_result_t;

typedef enum
{
    UDS_BACKEND_AUTO = 0,
    UDS_BACKEND_OBD_CHIP,
    UDS_BACKEND_ISOTP,
} uds_backend_t;

/* ---- lifecycle (§3) ------------------------------------------------------- */

esp_err_t uds_manager_init(void);
esp_err_t uds_manager_start(void);
esp_err_t uds_manager_stop(void);

/* ---- the one transaction --------------------------------------------------- */

/**
 * Send a UDS request and collect the (final) response. Handles the 0x78
 * responsePending loop and decodes the outcome into @p result. Serialized:
 * ESP_ERR_INVALID_STATE if another UDS transaction/claim is in flight.
 *
 * @param addr     tx/rx/ext for this ECU.
 * @param req      request bytes (e.g. {0x22,0xF1,0x90}).
 * @param req_len  request length (>=1).
 * @param resp     buffer for the response bytes.
 * @param resp_cap capacity of @p resp.
 * @param resp_len receives the response length.
 * @param opts     optional timing overrides (NULL = defaults).
 * @param result   optional outcome decode (NULL to ignore).
 * @return ESP_OK when a final response arrived (positive OR negative —
 *         a negative response is still a completed transaction; check
 *         result->negative), ESP_ERR_TIMEOUT, ESP_ERR_INVALID_STATE
 *         (busy or transport down), ESP_ERR_INVALID_ARG.
 */
esp_err_t uds_request(const uds_addr_t *addr,
                      const uint8_t *req, size_t req_len,
                      uint8_t *resp, size_t resp_cap, size_t *resp_len,
                      const uds_opts_t *opts, uds_result_t *result);

/* ---- session (tester-present keepalive) ------------------------------------ */

/** Begin a session to @p addr: arms a periodic tester-present (3E 80)
 *  and HOLDS the transport claim so a whole exchange stays on one ECU.
 *  ESP_ERR_INVALID_STATE if a session/transaction is already active. */
esp_err_t uds_session_begin(const uds_addr_t *addr);

/** End the active session (disarm tester-present, release the claim). */
esp_err_t uds_session_end(void);

/* ---- raw ISO-TP (script bindings; no UDS semantics) ------------------------ */

/** Send one raw ISO-TP PDU to @p addr (no 0x78 loop, no NRC decode).
 *  isotp backend only — ESP_ERR_NOT_SUPPORTED when can_manager is not
 *  the active transport's bus. Used by the script `obd_isotp_tx`. */
esp_err_t uds_isotp_tx(const uds_addr_t *addr,
                       const uint8_t *data, size_t len, uint32_t timeout_ms);

/** Receive one raw ISO-TP PDU from @p addr (whatever arrives next on the
 *  bound rx_id, UDS or not). Used by the script `obd_isotp_rx`. */
esp_err_t uds_isotp_rx(const uds_addr_t *addr,
                       uint8_t *out, size_t cap, size_t *out_len,
                       uint32_t timeout_ms);

/* ---- status + the exclusive option ---------------------------------------- */

/** Idle after the last request before an exclusive hold lets autopid
 *  back on the bus (a held session keeps it). */
#define UDS_EXCLUSIVE_IDLE_MS 10000

/** Snapshot for GET /api/uds. */
typedef struct
{
    uds_backend_t backend_setting;
    uds_backend_t backend_active;
    const char   *provider;          /**< "esp_isotp" | "add-on" | "none" */
    bool          can_running;
    bool          exclusive;         /**< the runtime switch                */
    bool          exclusive_default; /**< the boot setting                  */
    bool          holding;           /**< our diagnostics hold is on now     */
    bool          autopid_paused;    /**< the pollers acknowledged the hold  */
    bool          session_active;    /**< tester-present session open        */
    /* the last transaction (valid when last_ts_us != 0) */
    int64_t       last_ts_us;
    esp_err_t     last_err;
    uint32_t      last_tx_id;
    uint32_t      last_rx_id;
    uint8_t       last_req_sid;
    uds_result_t  last;
} uds_status_t;

void uds_manager_get_status(uds_status_t *out);

/** Runtime "exclusive" (boot default = the `exclusive` setting): while
 *  the tool is in use — a request, then UDS_EXCLUSIVE_IDLE_MS of idle,
 *  or an open session — the background pollers (autopid: PID polling +
 *  DTC scans) stay off the bus (obd_gate's diagnostics hold). Applies
 *  at once. */
void uds_manager_set_exclusive(bool on);
bool uds_manager_exclusive(void);

/* ---- surfaces -------------------------------------------------------------- */

esp_err_t uds_manager_register_http(void); /* /api/uds, /request, /session */

/** Which backend is effectively active right now (for status/CLI). */
uds_backend_t uds_manager_active_backend(void);
const char *uds_manager_backend_name(uds_backend_t b);

#ifdef __cplusplus
}
#endif
