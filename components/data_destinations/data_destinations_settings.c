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
 * @file data_destinations_settings.c
 * @brief settings_manager descriptor for data_destinations: the field
 *        table (master `enabled` + the bounded `destinations[]` array of
 *        flat items), on_validate (the pure parser as the shape check),
 *        on_apply (parse into the applied table, register the CLI).
 *        Secrets (`auth_token`, `api_key`, `basic_password`) are redacted
 *        on GET / kept-on-empty on PUT by api_http's suffix rule.
 */
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "settings_manager.h"

#include "data_destinations.h"
#include "data_destinations_private.h"

static const char *TAG = "data_destinations";

/* clang-format off */
static const settings_field_t DEST_ITEMS[] =
{
    SETTINGS_STR_REQ     ("name",           1, DD_NAME_LEN - 1, ""),
    SETTINGS_STR_ENUM_REQ("type",           "mqtt,http,https,abrp", "mqtt"),
    SETTINGS_BOOL        ("enabled",        true),
    SETTINGS_STR         ("url",            DD_URL_LEN - 1, ""),
    SETTINGS_INT         ("period_s",       1, 86400, 5),
    SETTINGS_STR_ENUM    ("auth",           "none,bearer,api_key_header,"
                                            "api_key_query,basic", "none"),
    SETTINGS_STR         ("auth_token",     DD_SECRET_LEN - 1, ""),
    SETTINGS_STR         ("auth_name",      DD_SMALL_LEN - 1, ""),
    SETTINGS_STR         ("basic_username", DD_SMALL_LEN - 1, ""),
    SETTINGS_STR         ("basic_password", DD_SMALL_LEN - 1, ""),
    SETTINGS_STR         ("api_key",        DD_SECRET_LEN - 1, ""),
    SETTINGS_STR         ("query",          DD_QUERY_LEN - 1, ""),
    SETTINGS_STR         ("cert_set",       DD_CERTSET_LEN - 1, ""),
    SETTINGS_STR         ("car_model",      DD_SMALL_LEN - 1, ""),
    SETTINGS_BOOL        ("retain",         true),
    SETTINGS_BOOL        ("full_first",     true),
};

static const settings_field_t FIELDS[] =
{
    SETTINGS_BOOL ("enabled", true),   /* master; an empty list is quiet */
    SETTINGS_ARRAY("destinations", DD_MAX, DEST_ITEMS, NULL),
    SETTINGS_BOOL ("cli", true),
};
/* clang-format on */

/* the parse scratch is ~10 KB: PSRAM, never a stack frame (§2) */
static dd_config_t s_scratch EXT_RAM_BSS_ATTR;
static SemaphoreHandle_t s_scratch_lock;
static StaticSemaphore_t s_scratch_lock_buf;

static esp_err_t on_validate(const cJSON *settings, char *err, size_t err_len)
{
    if (s_scratch_lock == NULL ||
        xSemaphoreTake(s_scratch_lock, pdMS_TO_TICKS(2000)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t rc = dd_config_parse(settings, &s_scratch, err, err_len);

    xSemaphoreGive(s_scratch_lock);
    return rc;
}

static esp_err_t on_apply(const cJSON *settings)
{
    char err[DD_ERR_LEN] = "";

    if (s_scratch_lock == NULL ||
        xSemaphoreTake(s_scratch_lock, pdMS_TO_TICKS(2000)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t rc = dd_config_parse(settings, &s_scratch, err, sizeof(err));

    if (rc == ESP_OK)
    {
        dd_config_store(&s_scratch);
    }

    xSemaphoreGive(s_scratch_lock);

    if (rc != ESP_OK)
    {
        ESP_LOGW(TAG, "stored destinations rejected: %s", err);
        return rc; /* the manager retries with defaults (§4.3 step 4) */
    }

    /* CLI ownership: settings-gated self-registration (Standard §6b) */
    if (!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(settings, "cli")))
    {
        static bool s_cli_registered;

        if (!s_cli_registered && data_destinations_register_cli() == ESP_OK)
        {
            s_cli_registered = true;
        }
    }

    return ESP_OK;
}

esp_err_t dd_settings_register(void)
{
    static const settings_descriptor_t DESC =
    {
        .name        = "data_destinations",
        .version     = 1,
        .fields      = FIELDS,
        .field_count = sizeof(FIELDS) / sizeof(FIELDS[0]),
        .on_apply    = on_apply,
        .on_validate = on_validate,
    };

    if (s_scratch_lock == NULL)
    {
        s_scratch_lock = xSemaphoreCreateMutexStatic(&s_scratch_lock_buf);
    }

    return settings_manager_register(&DESC);
}
