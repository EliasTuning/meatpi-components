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
 * @file obd_chip_settings.c
 * @brief Settings descriptor ("obd_chip", field table, reboot-to-apply §4.2):
 *        auto-sleep, the monitor arbitration policy (reserved; v1 implements
 *        "manual" only: task §5 open question) and the chip's stored sleep
 *        thresholds. The UART baud is NOT a setting since v3 (OBD_CHIP_BAUD,
 *        meatpi 2026-09-07), and the web UI shows none of these fields:
 *        they stay reachable through the settings API / CLI / backup.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"

#include <stdio.h>

#include "settings_manager.h"

#include "obd_chip.h"
#include "obd_chip_private.h"

/* clang-format off */
static const settings_field_t OBD_FIELDS[] =
{
    /* no "baud": the UART speed is fixed (OBD_CHIP_BAUD), v3, 2026-09-07 */
    SETTINGS_BOOL    ("auto_sleep",     false),
    /* legacy parity (Ali 2026-07-26): a chip not at the packaged fw
       version is auto-flashed to it after bring-up */
    SETTINGS_BOOL    ("auto_update",    true),
    /* the `obd_chip` console command (standard §6b; issue #1023,
       2026-10-28: a stuck chip could neither be probed nor reflashed
       from the device). Optional key with a default: no version bump. */
    SETTINGS_BOOL    ("cli",            true),
    SETTINGS_STR_ENUM("monitor_policy", "manual,auto_interrupt", "manual"),
    /* RESERVED since 2026-11-07: the chip's autonomous voltage controls
       stay OFF (the ESP32 owns the sleep policy), and the sleep triggers
       bench proved the stored thresholds inert while they are off, so the
       bring-up neither writes these to the chip nor compares them with it
       any more (STSLVLW/STSLVLS/STSLUIT gone from the provisioning). Kept
       in the schema, validated and parsed as before, for a chip-side
       policy that may want them one day (no version bump, no migration).
       The ranges (meatpi 2026-09-06, 12 V battery): wake while charging
       (12.0 to 15.0 V), sleep once the engine is off (12.0 to 14.0 V), hold
       at most an hour, wake must sit above sleep (on_validate). */
    SETTINGS_INT     ("wake_voltage_mv",  12000, 15000, 13500),
    SETTINGS_INT     ("sleep_voltage_mv", 12000, 14000, 13200),
    SETTINGS_INT     ("sleep_time_min",   1, 60, 2),
};
/* clang-format on */

static obd_config_t s_config EXT_RAM_BSS_ATTR;
static bool s_configured;

static esp_err_t obd_on_apply(const cJSON *settings)
{
    const cJSON *auto_sleep =
        cJSON_GetObjectItemCaseSensitive(settings, "auto_sleep");
    const cJSON *policy =
        cJSON_GetObjectItemCaseSensitive(settings, "monitor_policy");

    const cJSON *wake_mv =
        cJSON_GetObjectItemCaseSensitive(settings, "wake_voltage_mv");
    const cJSON *sleep_mv =
        cJSON_GetObjectItemCaseSensitive(settings, "sleep_voltage_mv");
    const cJSON *sleep_min =
        cJSON_GetObjectItemCaseSensitive(settings, "sleep_time_min");

    s_config.auto_sleep = cJSON_IsTrue(auto_sleep);
    s_config.auto_update = !cJSON_IsFalse(
        cJSON_GetObjectItemCaseSensitive(settings, "auto_update"));
    s_config.monitor_auto_interrupt =
        cJSON_IsString(policy) && policy->valuestring != NULL &&
        strcmp(policy->valuestring, "auto_interrupt") == 0;
    s_config.wake_voltage =
        (cJSON_IsNumber(wake_mv) ? (float)wake_mv->valueint : 13500.0f) /
        1000.0f;
    s_config.sleep_voltage =
        (cJSON_IsNumber(sleep_mv) ? (float)sleep_mv->valueint : 13200.0f) /
        1000.0f;
    /* legacy: minutes*60 + 30 s guard (the ESP sleeps first) */
    s_config.sleep_time_s =
        (uint32_t)(cJSON_IsNumber(sleep_min) ? sleep_min->valueint : 2) *
            60 + 30;

    s_configured = true;

    /* CLI ownership: settings-gated self-registration (standard §6b) */
    if (!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(settings, "cli")))
    {
        static bool s_cli_registered;

        if (!s_cli_registered && obd_chip_register_cli() == ESP_OK)
        {
            s_cli_registered = true;
        }
    }

    return ESP_OK;
}


/* Clamp a stored integer into the current schema range (migration helper). */
static void clamp_int(cJSON *settings, const char *key, int lo, int hi)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(settings, key);

    if (cJSON_IsNumber(v) && (v->valueint < lo || v->valueint > hi))
    {
        cJSON_SetNumberValue(v, (v->valueint < lo) ? lo : hi);
    }
}

static esp_err_t obd_on_validate(const cJSON *settings, char *err,
                                 size_t err_len)
{
    /* cross-field: the chip wakes when the rail climbs past the wake
       threshold, so it must sit above the sleep threshold */
    const cJSON *w = cJSON_GetObjectItemCaseSensitive(settings, "wake_voltage_mv");
    const cJSON *sl = cJSON_GetObjectItemCaseSensitive(settings, "sleep_voltage_mv");

    if (cJSON_IsNumber(w) && cJSON_IsNumber(sl) && w->valueint <= sl->valueint)
    {
        snprintf(err, err_len, "wake_voltage_mv must be above sleep_voltage_mv");
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static esp_err_t obd_settings_migrate(uint32_t from_version, cJSON *settings)
{
    /* v1 -> v2 (2026-09-06): tighter user-facing ranges + wake > sleep,
       clamp / lift what a device has stored instead of degrading it */
    if (from_version < 2 && settings != NULL)
    {
        clamp_int(settings, "wake_voltage_mv", 12000, 15000);
        clamp_int(settings, "sleep_voltage_mv", 12000, 14000);
        clamp_int(settings, "sleep_time_min", 1, 60);

        cJSON *w = cJSON_GetObjectItemCaseSensitive(settings, "wake_voltage_mv");
        cJSON *sl = cJSON_GetObjectItemCaseSensitive(settings, "sleep_voltage_mv");

        if (cJSON_IsNumber(w) && cJSON_IsNumber(sl) && w->valueint <= sl->valueint)
        {
            int lifted = sl->valueint + 300;

            cJSON_SetNumberValue(w, (lifted > 15000) ? 15000 : lifted);
        }
    }

    /* v2 -> v3 (2026-09-07): the UART baud is fixed (OBD_CHIP_BAUD), drop a
       stored value so nothing can ever feed a stale one back */
    if (from_version < 3 && settings != NULL)
    {
        cJSON_DeleteItemFromObjectCaseSensitive(settings, "baud");
    }

    return ESP_OK;
}

esp_err_t obd_settings_register(void)
{
    static const settings_descriptor_t desc =
    {
        .name        = "obd_chip",
        .version     = 3, /* v3: baud dropped, fixed OBD_CHIP_BAUD (2026-09-07) */
        .fields      = OBD_FIELDS,
        .field_count = sizeof(OBD_FIELDS) / sizeof(OBD_FIELDS[0]),
        .on_apply    = obd_on_apply,
        .on_validate = obd_on_validate,
        .on_migrate  = obd_settings_migrate,
    };

    return settings_manager_register(&desc);
}

const obd_config_t *obd_settings_config(void)
{
    return &s_config;
}

bool obd_settings_is_configured(void)
{
    return s_configured;
}
