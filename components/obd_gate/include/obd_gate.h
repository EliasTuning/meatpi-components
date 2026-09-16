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
 * @file obd_gate.h
 * @brief The vehicle-bus conversation gate (policy component).
 *
 * The WiCAN Pro has TWO OBD requesters on the SAME physical CAN bus: the
 * MIC3624 OBD chip (obd_chip, driven by apps over BLE/TCP/WS/USB) and any
 * ESP-side requester on the TWAI controller (an add-on pack's virtual ELM
 * jacks). Both address the
 * ECU functionally (0x7DF) and both hear every 0x7E8.. response — when two
 * request/response conversations OVERLAP, a requester can attribute the
 * other's response to its own request (bad data: the driving-app +
 * autopid clash, meatpi 2026-07-11).
 *
 * This gate serializes those conversations: one holder at a time, keyed by
 * an opaque owner pointer (the chip, or one engine instance). Enabled by
 * default (`obd_gate` settings, reboot-to-apply); disabled = every call is
 * a no-op.
 *
 * Semantics (fail-open by design — the gate is a data-quality guard, never
 * an availability hazard):
 *  - acquire() waits up to @p wait_ms for the gate, then TAKES it anyway
 *    (counted + warned) — a wedged holder can't brick the other side.
 *  - a hold auto-expires after OBD_GATE_HOLD_MS (a holder that never
 *    releases — e.g. a monitor command with no '>' prompt — self-clears).
 *  - re-acquire by the CURRENT holder just extends the hold (a multi-step
 *    conversation stays owned).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** How long a blocked requester waits before taking the gate anyway. */
#define OBD_GATE_WAIT_MS 3000
/** Failsafe hold expiry — an un-released hold self-clears after this. */
#define OBD_GATE_HOLD_MS 2000

typedef struct
{
    uint32_t acquires;  /* granted immediately or after a wait  */
    uint32_t waits;     /* had to wait at least one poll        */
    uint32_t steals;    /* wait expired -> took the gate anyway */
    uint32_t expiries;  /* stale hold reaped by a new acquirer  */
} obd_gate_stats_t;

/** Register the settings ("obd_gate") + log descriptors. */
esp_err_t obd_gate_init(void);

bool obd_gate_enabled(void); /* boot-applied `enabled` */

/**
 * @brief Serialize a bus conversation. Blocks (polling) up to @p wait_ms
 *        while another owner holds the gate, then takes it regardless
 *        (fail-open). Always ESP_OK when the gate is disabled.
 * @param owner  Opaque identity of the requester (stable pointer: the
 *               chip's token, an engine instance, ...).
 */
esp_err_t obd_gate_acquire(const void *owner, uint32_t wait_ms);

/** Release iff @p owner is the current holder (idempotent otherwise). */
void obd_gate_release(const void *owner);

void obd_gate_get_stats(obd_gate_stats_t *out);

/* ---- diagnostics exclusive hold -------------------------------------------
 * A diagnostic tool (the UDS Tool, the J2534 PassThru server) with its
 * "exclusive" option on holds this while it is in use; the background
 * pollers (autopid: PID polling + DTC scans) stay off the bus meanwhile
 * and acknowledge. Refcounted by owner; independent of the `enabled`
 * setting above (a different policy: not who speaks next, but who may
 * speak at all). */

/** Hold (@p on) or release the bus for @p owner (a stable pointer). */
void obd_gate_diag_hold(const void *owner, bool on);

/** True while any tool holds it. */
bool obd_gate_diag_held(void);

/** How many tools hold it right now. */
uint8_t obd_gate_diag_holders(void);

/** The poller's side: report every loop whether it is off the bus. */
void obd_gate_diag_ack(bool off_bus);

/** True when a poller acknowledged the current hold. */
bool obd_gate_diag_acked(void);

/** Wait (polling) up to @p wait_ms for the poller's acknowledgement.
 *  True when acknowledged — or when no poller ever reported (nothing
 *  to wait for, e.g. autopid disabled). */
bool obd_gate_diag_wait_ack(uint32_t wait_ms);

/* Ready-made hooks for an ESP-side engine's gate callbacks: ctx
 * is the engine instance pointer (the per-engine owner identity). */
void obd_gate_engine_acquire(void *ctx);
void obd_gate_engine_release(void *ctx);

#ifdef __cplusplus
}
#endif
