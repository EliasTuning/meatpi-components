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
 * @file can_isotp.h
 * @brief ISO-TP (ISO 15765-2) session provider slot on the shared bus.
 *
 * can_manager owns the native-CAN bus but not an ISO-TP stack; a provider
 * registers one here: an add-on component pack at ext init (see
 * ext_manager.h), else the public build's own can_isotp_esp (esp_isotp
 * over this bus; main calls it right after ext_manager_init and it
 * registers only when the slot is still empty — single writer). The
 * provider decides how many sessions per rx id (can_isotp_esp: one).
 * Consumers — uds_manager's "isotp" transport, the
 * J2534 ISO15765 channel — fetch the ops with can_isotp() and treat a
 * NULL return as "not available in this build" (they degrade with a
 * log line; UDS falls back to the obd_chip transport).
 *
 * Provider contract:
 *   - Sessions coexist with normal CAN monitoring: only frames matching
 *     the session's rx_id are consumed by the ISO-TP stack, everything
 *     else keeps flowing to the other bus clients.
 *   - open() fails with ESP_ERR_INVALID_STATE while the bus is down.
 *   - recv() returns ESP_OK (+*len), ESP_ERR_TIMEOUT (nothing arrived
 *     within timeout_ms), or ESP_ERR_NO_MEM (the reassembled message
 *     was larger than @p cap and has been CONSUMED — callers use this
 *     to drain stale traffic).
 *   - send() blocks the full transfer (segmentation + flow control);
 *     ESP_ERR_TIMEOUT when the peer's flow control never arrives.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque per-link session (one tx_id/rx_id pair). */
typedef struct can_isotp_session *can_isotp_session_t;

typedef struct
{
    uint32_t tx_id;        /**< physical request id (tester -> ECU)   */
    uint32_t rx_id;        /**< physical response id (ECU -> tester)  */
    bool     ext_id;       /**< 29-bit ids                            */
    uint8_t  block_size;   /**< flow control BS (0 = no limit)        */
    uint8_t  stmin_ms;     /**< flow control STmin, milliseconds      */
    uint8_t  padding_byte; /**< pad value when use_padding            */
    bool     use_padding;  /**< pad frames to 8 bytes                 */
} can_isotp_cfg_t;

typedef struct
{
    esp_err_t (*open)(const can_isotp_cfg_t *cfg, can_isotp_session_t *out);
    void      (*close)(can_isotp_session_t s);
    esp_err_t (*send)(can_isotp_session_t s, const uint8_t *data,
                      size_t len, uint32_t timeout_ms);
    esp_err_t (*recv)(can_isotp_session_t s, uint8_t *buf, size_t cap,
                      size_t *len, uint32_t timeout_ms);
} can_isotp_ops_t;

/** Register the provider (an add-on pack calls this once at ext init,
 *  before any consumer starts). The ops table must live forever. */
esp_err_t can_isotp_provide(const can_isotp_ops_t *ops);

/** The registered provider, or NULL when this build has none. */
const can_isotp_ops_t *can_isotp(void);

#ifdef __cplusplus
}
#endif
