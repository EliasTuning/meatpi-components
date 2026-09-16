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
 * @file can_isotp_esp.h
 * @brief The public build's native ISO-TP (ISO 15765-2) provider.
 *
 * Implements can_manager's provider slot (can_isotp.h) with the vendored
 * esp_isotp stack in externally-managed mode: can_manager keeps the TWAI
 * node, this component subscribes a frame queue on each session's rx id,
 * one task feeds the frames to the stack and drives its state machine,
 * and the stack's TX frames go out through can_manager_send(). Sessions
 * coexist with every other bus client (monitor, autopid on the MIC chip,
 * bridges): only frames on a session's rx id are consumed by it.
 *
 * Consumers (uds_manager's "isotp" transport, the J2534 ISO15765 channel)
 * keep using can_isotp() and know nothing about this component. A build
 * that carries an add-on pack with its own provider wins the slot: main
 * calls can_isotp_esp_init() AFTER ext_manager_init and it registers
 * only when the slot is still empty.
 *
 * Limits (esp_isotp 0.1.1): the flow control THIS side sends as receiver
 * uses the Kconfig defaults (BS 8, STmin 1 ms) — can_isotp_cfg_t's
 * block_size/stmin_ms are accepted and ignored; N_Bs/N_Cr are the Kconfig
 * response timeout (100 ms); one PDU is at most CAN_ISOTP_ESP_MAX_PDU
 * bytes (SAE J2534's 4128); at most 5 sessions (1 UDS + 4 J2534 channels).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    uint32_t sessions_open;  /**< open right now                          */
    uint32_t sessions_peak;
    uint32_t opens;
    uint32_t open_fails;     /**< bus down, cap, duplicate rx id, no mem   */
    uint32_t frames_fed;     /**< CAN frames handed to the ISO-TP stack    */
    uint32_t frames_orphan;  /**< frames for a session closed meanwhile    */
    uint32_t pdus_tx;        /**< complete PDUs sent                       */
    uint32_t pdus_rx;        /**< complete PDUs reassembled                */
    uint32_t tx_timeouts;    /**< peer flow control never came / CF stall  */
    uint32_t rx_dropped;     /**< mailbox full: oldest PDU discarded       */
    uint32_t rx_oversize;    /**< recv() cap too small: consumed, NO_MEM   */
} can_isotp_esp_stats_t;

/** Lifecycle: init-only (standard section 3 - like obd_gate). Registers as
 *  the ISO-TP provider iff the slot is empty (main's init pass, right after
 *  ext_manager_init). Nothing to start: the task and queue come up at the
 *  first open(); nothing to stop: sessions belong to the consumers, which
 *  close them in their own stop() (uds_manager, j2534_server), and the ops
 *  table must live forever (can_isotp.h). Idempotent; ESP_OK also when an
 *  add-on pack already provides ISO-TP (then this stands by). */
esp_err_t can_isotp_esp_init(void);

/** True when THIS component is the registered provider. */
bool can_isotp_esp_active(void);

void can_isotp_esp_get_stats(can_isotp_esp_stats_t *out);

#ifdef __cplusplus
}
#endif
