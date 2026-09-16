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
 * @file uds_transport.h
 * @brief The transport vtable behind uds_manager. ONE op: a single
 *        request→response transaction with NO UDS semantics (the 0x78
 *        loop / NRC decode / tester-present live in uds_manager above).
 *
 * Implementations:
 *   uds_transport_obd   — obd_chip AT (MIC3624), always available.
 *   uds_transport_isotp — raw UDS PDU over the registered ISO-TP
 *                         provider (can_isotp.h) on can_manager.
 *
 * The shared AT-hex helpers below are pure and host-tested.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "uds_manager.h" /* uds_addr_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct uds_transport_s
{
    const char *name;

    /** Bring the backend up (open engine / isotp handle). Idempotent.
     *  ESP_ERR_INVALID_STATE if the backend's bus isn't available. */
    esp_err_t (*open)(void);

    /** One request→FINAL response. Sets addressing as needed, sends
     *  @p req, and transparently consumes 0x78 responsePending frames
     *  (using @p p2star_ms for each) so the caller sees only the final
     *  UDS PDU (headers stripped). @p pending_out (optional) receives
     *  the number of 0x78 frames seen. ESP_ERR_TIMEOUT on no response. */
    esp_err_t (*transceive)(const uds_addr_t *addr,
                            const uint8_t *req, size_t req_len,
                            uint8_t *resp, size_t resp_cap, size_t *resp_len,
                            uint32_t p2_ms, uint32_t p2star_ms,
                            uint8_t *pending_out);
} uds_transport_t;

/* Backend singletons (open lazily). */
const uds_transport_t *uds_transport_obd(void);
const uds_transport_t *uds_transport_isotp(void);

/* The AT-hex transaction (the request fn is injected so the transaction
 * stays testable). */
typedef esp_err_t (*uds_at_request_fn)(const char *cmd, char *resp,
                                       size_t resp_len, uint32_t timeout_ms);

/** The same with @p skip_setup: the caller knows the chip is still in this
 *  address' setup (nobody else wrote to it since), so only the request
 *  line goes out — one request = one AT round-trip. */
esp_err_t uds_at_transceive_ex(uds_at_request_fn req_fn,
                               const uds_addr_t *addr,
                               const uint8_t *req, size_t req_len,
                               uint8_t *resp, size_t resp_cap,
                               size_t *resp_len, uint32_t p2_ms,
                               uint32_t p2star_ms, uint8_t *pending_out,
                               bool skip_setup);

esp_err_t uds_at_transceive(uds_at_request_fn req_fn,
                            const uds_addr_t *addr,
                            const uint8_t *req, size_t req_len,
                            uint8_t *resp, size_t resp_cap, size_t *resp_len,
                            uint32_t p2_ms, uint32_t p2star_ms,
                            uint8_t *pending_out);

/** PURE: parse an ELM/MIC headers-off hex response (single- or
 *  multi-frame, tolerant of index tokens and an ISO-TP length prefix)
 *  into UDS payload bytes. Host-tested. Returns false on no hex / a
 *  chip error token / overflow. */
/** The same, also counting the chip's '7F xx 78' responsePending lines
 *  that preceded the final answer (dropped from @p out). */
bool uds_at_parse_response_ex(const char *resp, uint8_t *out, size_t cap,
                              size_t *out_len, uint8_t *pending_out);

bool uds_at_parse_response(const char *resp, uint8_t *out, size_t cap,
                           size_t *out_len);

#ifdef __cplusplus
}
#endif
