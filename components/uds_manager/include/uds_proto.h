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
 * @file uds_proto.h
 * @brief PURE UDS (ISO 14229) helpers — no I/O, host-tested. Predicates
 *        on response bytes, the NRC name table, and hex⇄bytes for the
 *        terminal.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UDS_NR_SID          0x7Fu  /* negative-response service id       */
#define UDS_NRC_PENDING     0x78u  /* requestCorrectlyReceived-RspPending */
#define UDS_POS_OFFSET      0x40u  /* positive response = sid + 0x40      */

/** true when resp is a negative response (7F <sid> <nrc>). */
bool uds_is_negative(const uint8_t *resp, size_t len);

/** true when resp is a 0x78 responsePending negative response. */
bool uds_is_pending(const uint8_t *resp, size_t len);

/** true when resp is the positive response to request SID @p req_sid. */
bool uds_is_positive_for(uint8_t req_sid, const uint8_t *resp, size_t len);

/** true when resp answers request SID @p req_sid at all: its positive
 *  response (sid + 0x40) or a negative response naming it (7F sid nrc).
 *  Anything else is a stray from another requester on the shared bus
 *  (autopid's `41 0C ..` landing in a UDS window, bench 2026-09-16). */
bool uds_response_matches(uint8_t req_sid, const uint8_t *resp, size_t len);

/** NRC byte of a negative response, or 0 if @p resp is not negative. */
uint8_t uds_nrc_of(const uint8_t *resp, size_t len);

/** Static ISO 14229 name for an NRC (never NULL; "unknown" if unmapped). */
const char *uds_nrc_name(uint8_t nrc);

/** Parse a hex string (spaces/':'/'-' ignored, optional 0x, case-
 *  insensitive) into bytes. Returns false on odd nibble count, a bad
 *  char, or overflow. */
bool uds_hex_to_bytes(const char *str, uint8_t *out, size_t cap,
                      size_t *out_len);

/** Format bytes as space-separated uppercase hex into @p out (needs
 *  len*3 + 1). Returns the string length written (0 if it wouldn't
 *  fit). */
size_t uds_bytes_to_hex(const uint8_t *bytes, size_t len,
                        char *out, size_t cap);

#ifdef __cplusplus
}
#endif
