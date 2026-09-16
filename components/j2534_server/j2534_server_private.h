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

/** Internals shared across the j2534_server .c files. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t j2534_server_register_cli(void);

/* ---- settings (j2534_server_settings.c) -------------------------------- */

/** Boot-applied config (filled by on_apply). */
typedef struct
{
    bool     enabled;
    bool     allow_reflash;    /* ECU-flashing gate (mirrors the setting) */
    bool     allow_lan;        /* accept on STA / USB-eth uplinks too */
    bool     exclusive;        /* boot default: pause autopid while attached */
    uint16_t port;
} j2534_config_t;

/** Register the "j2534_server" descriptor with settings_manager. */
esp_err_t j2534_settings_register(void);

const j2534_config_t *j2534_settings_config(void);

bool j2534_settings_is_configured(void); /* boot apply ran (standard §4.3) */
