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
 * @file script_engine_settings.c
 * @brief settings_manager descriptor for script_engine: field-table schema
 *        (source of truth for shape/ranges/defaults) and on_apply (stores
 *        the boot-applied enabled flag + run budget; registers the CLI,
 *        settings-gated per standard §6b).
 */
#include "settings_manager.h"

#include "script_engine_private.h"

static const settings_field_t FIELDS[] =
{
    SETTINGS_BOOL("enabled", false),
    SETTINGS_INT("max_runtime_ms", 100, 120000, 10000),
    /* Gates the UDS reflash services (0x34/0x35/0x36/0x37 +
     * obd_transfer_file) from scripts — mirrors j2534's allow_reflash,
     * default OFF: a script can diagnose freely but cannot reprogram an
     * ECU unless this is explicitly enabled. */
    SETTINGS_BOOL("allow_reflash", false),
    /* Exclusive bus for scripts (Ali 2026-09-16, ON by default): from a
     * script's first ECU access (uds/uds_ext, obd_claim, obd_request,
     * obd_isotp_tx/rx) to the end of the run, the background pollers
     * (autopid: PID polling + DTC scans) stay off the bus — obd_gate's
     * diagnostics hold, independent of the UDS Tool page's switch. */
    SETTINGS_BOOL("exclusive", true),
    SETTINGS_BOOL("cli", true),
};

/* boot-applied settings */
static bool s_configured;
static bool s_enabled;
static bool s_allow_reflash;
static bool s_exclusive = true;
static uint32_t s_max_runtime_ms = 10000;

bool se_settings_enabled(void)
{
    return s_enabled;
}

bool se_settings_allow_reflash(void)
{
    return s_allow_reflash;
}

bool se_settings_exclusive(void)
{
    return s_exclusive;
}

uint32_t se_settings_max_runtime_ms(void)
{
    return s_max_runtime_ms;
}

bool se_settings_is_configured(void)
{
    return s_configured;
}

static esp_err_t on_migrate(uint32_t from_version, cJSON *settings)
{
    /* v1 -> v2: added allow_reflash (schema default false fills it) */
    (void)from_version;
    (void)settings;
    return ESP_OK;
}

static esp_err_t on_apply(const cJSON *settings)
{
    s_enabled = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(settings, "enabled"));

    const cJSON *v = cJSON_GetObjectItemCaseSensitive(settings,
                                                      "max_runtime_ms");
    s_max_runtime_ms = cJSON_IsNumber(v) ? (uint32_t)v->valueint : 10000;

    s_allow_reflash = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(settings, "allow_reflash"));
    s_exclusive = !cJSON_IsFalse(
        cJSON_GetObjectItemCaseSensitive(settings, "exclusive")); /* default on */

    if (!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(settings, "cli")))
    {
        static bool reg;

        if (!reg && script_engine_register_cli() == ESP_OK)
        {
            reg = true;
        }
    }

    s_configured = true;
    return ESP_OK;
}

esp_err_t se_settings_register(void)
{
    static const settings_descriptor_t DESC =
    {
        .name        = "script_engine",
        .version     = 2,
        .fields      = FIELDS,
        .field_count = sizeof(FIELDS) / sizeof(FIELDS[0]),
        .on_migrate  = on_migrate,
        .on_apply    = on_apply,
    };

    return settings_manager_register(&DESC);
}
