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

/** Internals shared across the obd_gate .c files + the host suite. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- pure core (obd_gate_core.c — host-tested) ----------------------------- */

/** How long a waiter's next-turn reservation survives without a re-poll
 *  (the wrapper polls every 10 ms — 500 ms means "the waiter gave up"). */
#define OG_RESERVE_MS 500

typedef struct
{
    const void *owner;        /* NULL = free                         */
    int64_t     deadline_ms;  /* hold expiry (valid while owner)     */
    const void *waiter;       /* blocked owner holding the NEXT turn */
    int64_t     waiter_deadline_ms;
    uint32_t    acquires;
    uint32_t    waits;
    uint32_t    steals;
    uint32_t    expiries;
} og_core_t;

/**
 * Try to take the gate for @p owner at @p now_ms. Grants when the gate is
 * free, already held by @p owner (extends the hold), or the current hold
 * has expired (reaped + counted). Returns true when @p owner holds it.
 *
 * FAIRNESS: the first owner refused while the gate is held becomes the
 * WAITER and owns the next turn — after a release, other owners are
 * refused until the waiter collects (a tight requester loop can otherwise
 * starve a slow poller indefinitely; seen live: the MIC chip's TCP client
 * re-won the gate for 14 consecutive cycles while the engine polled at
 * 10 ms). The reservation lapses OG_RESERVE_MS after the waiter's last
 * poll, so an abandoned wait can't wedge the gate.
 */
bool og_core_try(og_core_t *g, const void *owner, int64_t now_ms,
                 uint32_t hold_ms);

/** Unconditional take (the fail-open path after a wait timeout). */
void og_core_force(og_core_t *g, const void *owner, int64_t now_ms,
                   uint32_t hold_ms);

/** Release iff held by @p owner; no-op otherwise. */
void og_core_release(og_core_t *g, const void *owner);

/* ---- diagnostics hold (obd_gate_diag.c — host-tested) ---------------------
 * An ESP-side diagnostic tool (the UDS Tool, the J2534 PassThru server)
 * with its "exclusive" option on asks the background bus pollers (autopid:
 * PID polling + DTC scans) to stay off the bus while it is in use.
 * Refcounted by owner identity; the poller acknowledges once it is off
 * the bus so a tool can wait for that before its first request. */
#define OG_DIAG_MAX_HOLDERS 4

typedef struct
{
    const void *holders[OG_DIAG_MAX_HOLDERS];
    uint8_t     n_holders;
    bool        acked;       /* the poller confirmed it is off the bus   */
    bool        poller_seen; /* a poller ever reported (else no waiting) */
    uint32_t    holds;
    uint32_t    releases;
    uint32_t    overflow;    /* a 5th holder refused                     */
} og_diag_t;

void og_diag_init(og_diag_t *d);

/** Add (@p on) or remove @p owner. Idempotent per owner. Returns true
 *  when the HELD state (any holder at all) changed. */
bool og_diag_set(og_diag_t *d, const void *owner, bool on);

bool og_diag_held(const og_diag_t *d);

/* ---- settings (obd_gate_settings.c) ----------------------------------------- */

/** Register the "obd_gate" descriptor with settings_manager. */
esp_err_t og_settings_register(void);

bool og_settings_enabled(void);       /* boot-applied `enabled`          */
bool og_settings_is_configured(void); /* boot apply ran (standard §4.3)  */

#ifdef __cplusplus
}
#endif
