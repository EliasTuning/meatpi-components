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

/** Internals shared across the uds_manager .c files. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "uds_manager.h" /* uds_backend_t */

esp_err_t uds_manager_register_cli(void);

/* the exclusive option: a raw ISO-TP tx/rx (uds_transport_isotp.c) counts
 * as use of the tool, like a request */
void uds_excl_touch(void);

/* event_manager glue (uds_manager_events.c). */
void uds_events_register(void);

/* ---- settings (uds_manager_settings.c) --------------------------------- */

/** Boot-applied config (filled by on_apply). */
typedef struct
{
    uds_backend_t backend;
    uint32_t      p2_ms;
    uint32_t      p2star_ms;
    uint32_t      tester_present_ms; /* tester-present period */
    bool          exclusive;         /* boot default of the runtime switch */
} uds_config_t;

/** Register the "uds_manager" descriptor with settings_manager. */
esp_err_t uds_settings_register(void);

const uds_config_t *uds_settings_config(void);

bool uds_settings_is_configured(void); /* boot apply ran (standard §4.3) */
