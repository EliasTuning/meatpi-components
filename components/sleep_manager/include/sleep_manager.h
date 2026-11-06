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
 * @file sleep_manager.h
 * @brief WiCAN low-power manager (feature component): rewrite of
 *        legacy `sleep_mode.c` (TASK_sleep_manager.md).
 *
 * Model: battery voltage (from battery_monitor, this component owns
 * NO ADC) drives a pure policy ladder: NORMAL → LOW_VOLTAGE (below
 * sleep_voltage, a countdown of sleep_delay_min) → SLEEPING →
 * WAKE_PENDING (voltage recovered and stable) → **reboot**
 * (restart_tracker POWER_WAKE) rather than resume-in-place: every
 * manager restarts clean, the legacy-proven approach.
 *
 * SLEEPING = repeated LIGHT sleep with a 2 s timer wake (voltage still
 * sampled between naps; the timer wake doubles as meatpi's bench
 * failsafe: the device can never be stranded in a state only an
 * external signal can leave). Entry sequence: `sleep.entering` event →
 * wait for autopid idle → main's prepare callback (ordered component
 * stops, composition-root glue, so this component doesn't depend on
 * wifi/ble/mqtt/...) → CAN transceiver standby → OBD chip sleep
 * (verified each nap, ≤6 re-sleeps then recovery reboot) → USB power
 * rail held low.
 *
 * Boot-loop guard (legacy parity): ≥3 unexpected resets while the
 * battery reads below the error threshold forces sleep instead of
 * another crash lap. Critical floor (2026-10-01): a battery under
 * 11.90 V for 5 min (300 s since 2026-10-06, 120 s before) sleeps
 * regardless of the `enabled` setting and of a longer sleep delay; the
 * normal wake rules apply afterwards.
 *
 * Bench-safety (meatpi 2026-07-07): the state task arms only after a
 * 15 s boot grace period (a bootloop still leaves a flash window),
 * and all sleeping is timer-woken light sleep: no deep sleep, no
 * wake source that can silently never fire.
 *
 * Settings schema v3 (2026-10-01): `wake_mv` is a setting of its own
 * (12100..15000, default 13200; documents from v2 get sleep + 100 mV).
 * The applied wake threshold is never under sleep + 0.1 V.
 * Schema v4 (2026-10-01): `wake_delay_ms` (100..5000, default 500) is how
 * long the battery must stay above the wake voltage before the wake
 * reboot (was a fixed 1 s); older documents get the default.
 *
 * The countdown (2026-10-06): the status says which rule will put the
 * awake device to sleep first (the sleep delay, or the critical floor,
 * which overtakes a longer delay) and in how many seconds, so the UI can
 * count it down instead of the device going quiet on the user. The hold
 * (same day): the UI offers ten more minutes, three times per boot
 * (sleep_manager_hold(), `POST /api/sleep/hold`, `sleep hold`).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    SLEEP_MANAGER_NORMAL = 0,
    SLEEP_MANAGER_LOW_VOLTAGE,   /**< below threshold, countdown runs */
    SLEEP_MANAGER_SLEEPING,
    SLEEP_MANAGER_WAKE_PENDING,  /**< recovered, stability hold       */
} sleep_manager_state_t;

/** The rule that will put an awake device to sleep first (2026-10-06). */
typedef enum
{
    SLEEP_MANAGER_PENDING_NONE = 0,
    SLEEP_MANAGER_PENDING_DELAY,    /**< under sleep_v: the sleep delay runs   */
    SLEEP_MANAGER_PENDING_CRITICAL, /**< under the critical floor: fixed delay */
} sleep_manager_pending_t;

typedef struct
{
    bool                  enabled;
    sleep_manager_state_t state;
    float                 voltage;      /**< latest battery reading   */
    float                 sleep_v;      /**< boot-applied threshold   */
    float                 wake_v;       /**< boot-applied wake (v3 setting, >= sleep + 0.1) */
    uint32_t              naps;         /**< light-sleep cycles       */
    uint32_t              chip_resleeps;/**< OBD re-sleep retries     */
    sleep_manager_pending_t pending;    /**< what sleeps it first     */
    uint32_t              sleep_in_s;   /**< seconds to that entry (0 with none) */
    float                 critical_v;   /**< the critical floor (fixed) */
    uint32_t              critical_s;   /**< the floor's fixed delay  */
    uint32_t              hold_s;       /**< seconds left of a hold (0: none) */
    uint8_t               holds_left;   /**< holds still allowed this boot */
    uint8_t               holds_max;    /**< the cap per boot (3)      */
    uint32_t              activity_s;   /**< seconds left of the extension a
                                             log read gave (0: none) */
} sleep_manager_status_t;

/** Ordered shutdown glue the composition root provides (stop radios,
 *  loggers, tunnels... in main's dependency order). Runs in the sleep
 *  task right before the hardware is powered down. */
typedef void (*sleep_manager_prepare_cb_t)(void);

/** Register descriptors (settings/log/events). No hardware access. */
esp_err_t sleep_manager_init(void);

/** Create the state task when enabled (15 s boot grace first). */
esp_err_t sleep_manager_start(void);

/** Park the state task (no more sleep entries; never wakes hardware). */
esp_err_t sleep_manager_stop(void);

esp_err_t sleep_manager_status(sleep_manager_status_t *out);

/** Composition-root glue (ONE callback, main wires it). */
esp_err_t sleep_manager_set_prepare_cb(sleep_manager_prepare_cb_t cb);

/** Keep the device awake for @p minutes more (1..30) on a person's request
 *  (Ali, 2026-10-06): the sleep delay and the critical floor both move out
 *  to now + minutes where that is later. Runtime only, never saved, three
 *  per boot. ESP_ERR_INVALID_ARG out of range, ESP_ERR_INVALID_STATE when
 *  nothing is counting (healthy battery, or asleep), ESP_ERR_NOT_ALLOWED
 *  past the third. */
esp_err_t sleep_manager_hold(uint32_t minutes);

/** Something is reading the device's log (the Home Assistant backfill after
 *  a drive, the dashboard's history; the composition root wires
 *  data_logger's reader hook here, 2026-11-06): a counting device stays
 *  awake @p seconds more from now where that is later than its deadline,
 *  but never longer than 10 minutes past the first such read of the
 *  countdown. Runtime only, counts against nothing. ESP_ERR_INVALID_ARG out
 *  of 1..600, ESP_ERR_INVALID_STATE when nothing counts. */
esp_err_t sleep_manager_activity(uint32_t seconds);

/** Force a sleep entry for the bench: enters the full sequence, then
 *  auto-reboots after @p wake_after_s regardless of voltage (so a
 *  USB-powered bench unit always comes back). */
esp_err_t sleep_manager_test_sleep(uint32_t wake_after_s);

/** The board-level half of the sleep entry, on its own: CAN transceiver to
 *  standby, OBD chip's sleep pin, USB power rail off and held. Pins only:
 *  it needs no init, no settings and no task, so the composition root's
 *  crash park can call it from a boot that started nothing (one copy of
 *  the sequence, the sleep entry calls it too). A restart undoes it the
 *  way a wake does. */
void sleep_manager_board_down(void);

/** Optional /api/sleep route (§9.1; main wires it). */
esp_err_t sleep_manager_register_http(void);

/** `sleep` CLI; registered internally on the settings apply. */
esp_err_t sleep_manager_register_cli(void);

#ifdef __cplusplus
}
#endif
