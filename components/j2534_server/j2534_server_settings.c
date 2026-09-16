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
 * @file j2534_server_settings.c
 * @brief settings_manager descriptor for j2534_server: field-table schema
 *        (source of truth for shape/ranges/defaults — including the
 *        allow_reflash / allow_lan safety gates) and on_apply (fills the
 *        boot-applied config and pushes the reflash gate into
 *        j2534_channel).
 */
#include "settings_manager.h"

#include "j2534_channel.h"
#include "j2534_server_private.h"

static const settings_field_t FIELDS[] =
{
    /* Whole feature off by default — the port is opened only when a user
     * deliberately enables it. */
    SETTINGS_BOOL("enabled", false),
    SETTINGS_INT("port", 1, 65535, 6809),
    /* ECU-flashing gate. Default false = the device rejects UDS
     * reprogramming services (RequestDownload/Upload, TransferData,
     * TransferExit), so a connected tool can read/diagnose but cannot
     * write ECU firmware. The safe default; flip on only for a real
     * reflash job. */
    SETTINGS_BOOL("allow_reflash", false),
    /* Interface exposure. Default false = accept tester connections only
     * on WiCAN's own SoftAP and the USB-device (NCM) link — i.e. someone
     * physically on WiCAN's AP or plugged into the USB cable. When false a
     * connection arriving over WiFi-STA or a USB-Ethernet uplink (the
     * "shop LAN") is dropped, so the unauthenticated reflash channel is
     * not reachable across a shared network. Set true only if you
     * understand that exposes the port to the whole LAN. */
    SETTINGS_BOOL("allow_lan", false),
    /* Exclusive bus (boot default; runtime switch on the J2534 page /
     * POST /api/j2534). While a tester is attached the background pollers
     * (autopid: PID polling + DTC scans) stay off the bus — obd_gate's
     * diagnostics hold — so the tool's conversations never interleave
     * with ours. ON by default (Ali 2026-09-16): a connected tool wants the
     * bus to itself; turn off to keep AutoPID telemetry flowing meanwhile. */
    SETTINGS_BOOL("exclusive", true),
    SETTINGS_BOOL("cli", true),
};

static j2534_config_t s_cfg = { .port = 6809, .exclusive = true };
static bool s_configured;

const j2534_config_t *j2534_settings_config(void)
{
    return &s_cfg;
}

bool j2534_settings_is_configured(void)
{
    return s_configured;
}

static esp_err_t on_apply(const cJSON *settings)
{
    s_cfg.enabled = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(settings, "enabled"));

    const cJSON *p = cJSON_GetObjectItemCaseSensitive(settings, "port");
    if (cJSON_IsNumber(p) && p->valueint > 0 && p->valueint <= 65535)
    {
        s_cfg.port = (uint16_t)p->valueint;
    }

    s_cfg.allow_reflash = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(settings, "allow_reflash"));
    j2534_channel_set_allow_reflash(s_cfg.allow_reflash);

    s_cfg.allow_lan = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(settings, "allow_lan"));
    s_cfg.exclusive = !cJSON_IsFalse(
        cJSON_GetObjectItemCaseSensitive(settings, "exclusive")); /* default on */

    if (!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(settings, "cli")))
    {
        static bool reg;
        if (!reg && j2534_server_register_cli() == ESP_OK)
        {
            reg = true;
        }
    }

    s_configured = true;
    return ESP_OK;
}

esp_err_t j2534_settings_register(void)
{
    static const settings_descriptor_t DESC =
    {
        .name        = "j2534_server",
        .version     = 1,
        .fields      = FIELDS,
        .field_count = sizeof(FIELDS) / sizeof(FIELDS[0]),
        .on_apply    = on_apply,
    };

    return settings_manager_register(&DESC);
}
