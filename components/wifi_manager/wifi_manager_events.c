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
 * @file wifi_manager_events.c
 * @brief event_manager glue (2026-09-17, the Rules Builder): the
 *        `wifi.sta {connected, ssid}` event on every STA link-up (got IP)
 *        and drop, plus the live values `${wifi.ssid}` / `${wifi.connected}`
 *        that rule conditions read ("while connected to HomeAP, poll the
 *        default group slower"). Publishing never blocks: event_manager
 *        copies the event into its queue. The radio code calls
 *        wm_events_sta() from its esp_event handlers.
 */
#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "event_manager.h"

#include "wifi_manager_private.h"

static char s_ssid[WM_SSID_LEN];
static bool s_connected;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t read_ssid(const char *name, char *out, size_t len)
{
    (void)name;

    portENTER_CRITICAL(&s_mux);
    snprintf(out, len, "%s", s_connected ? s_ssid : "");
    portEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

static esp_err_t read_connected(const char *name, char *out, size_t len)
{
    (void)name;

    snprintf(out, len, "%s", s_connected ? "true" : "false");
    return ESP_OK;
}

void wm_events_register(void)
{
    static const em_key_decl_t KEYS[] =
    {
        { "connected", EM_VAL_BOOL },
        { "ssid", EM_VAL_STR },
    };
    static const em_source_decl_t STA =
    {
        .source = "wifi", .name = "sta",
        .description = "the STA link came up (connected=true, ssid) or "
                       "dropped (connected=false)",
        .keys = KEYS, .n_keys = 2,
    };

    (void)event_manager_declare_source(&STA);
    (void)event_manager_register_value("wifi.ssid", read_ssid);
    (void)event_manager_register_value("wifi.connected", read_connected);
}

void wm_events_sta(bool connected, const char *ssid)
{
    portENTER_CRITICAL(&s_mux);
    s_connected = connected;
    snprintf(s_ssid, sizeof(s_ssid), "%s",
             (connected && ssid != NULL) ? ssid : "");
    portEXIT_CRITICAL(&s_mux);

    em_event_t ev = { 0 };

    snprintf(ev.source, sizeof(ev.source), "wifi");
    snprintf(ev.name, sizeof(ev.name), "sta");
    ev.kv[0] = em_kv_bool("connected", connected);
    ev.kv[1] = em_kv_str("ssid", connected ? s_ssid : "");
    ev.n = 2;
    (void)event_manager_publish(&ev);
}
