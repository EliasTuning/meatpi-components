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
 * @file sleep_manager_http.c
 * @brief The optional /api/sleep status route (§9.1): the UI's
 *        "sleep armed / countdown" banner reads this (`pending` and
 *        `sleep_in_s` since 2026-10-06: which rule sleeps the device
 *        first, the sleep delay or the critical floor, and when), and
 *        POST /api/sleep/hold, the "keep awake" button (same day).
 */
#include <stdio.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "http_server_manager.h"

#include "sleep_manager.h"

static const char *TAG = "sleep_manager";

static const char *state_str(sleep_manager_state_t st)
{
    switch (st)
    {
        case SLEEP_MANAGER_LOW_VOLTAGE:  return "low_voltage";
        case SLEEP_MANAGER_SLEEPING:     return "sleeping";
        case SLEEP_MANAGER_WAKE_PENDING: return "wake_pending";
        default:                         return "normal";
    }
}

static const char *pending_str(sleep_manager_pending_t p)
{
    switch (p)
    {
        case SLEEP_MANAGER_PENDING_DELAY:    return "delay";
        case SLEEP_MANAGER_PENDING_CRITICAL: return "critical";
        default:                             return "none";
    }
}

static int status_json(char *body, size_t len)
{
    sleep_manager_status_t st;

    (void)sleep_manager_status(&st);
    return snprintf(body, len,
                    "{\"enabled\":%s,\"state\":\"%s\",\"voltage\":%.2f,"
                    "\"sleep_v\":%.2f,\"wake_v\":%.2f,\"naps\":%lu,"
                    "\"pending\":\"%s\",\"sleep_in_s\":%lu,"
                    "\"critical_v\":%.2f,\"critical_s\":%lu,"
                    "\"hold_s\":%lu,\"holds_left\":%u,\"holds_max\":%u,"
                    "\"activity_s\":%lu,\"activity_why\":\"%s\"}",
                    st.enabled ? "true" : "false", state_str(st.state),
                    st.voltage, st.sleep_v, st.wake_v,
                    (unsigned long)st.naps,
                    pending_str(st.pending), (unsigned long)st.sleep_in_s,
                    st.critical_v, (unsigned long)st.critical_s,
                    (unsigned long)st.hold_s, (unsigned)st.holds_left,
                    (unsigned)st.holds_max, (unsigned long)st.activity_s,
                    st.activity_why != NULL ? st.activity_why : "");
}

static esp_err_t sleep_handler(httpd_req_t *req)
{
    char body[384];

    status_json(body, sizeof(body));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_error(httpd_req_t *req, const char *status,
                            const char *msg)
{
    char body[96];

    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

/* POST /api/sleep/hold {"minutes":1..30} (no body = 10): the "keep awake"
 * button (Ali, 2026-10-06). Answers the status with the new countdown;
 * 400 out of range, 409 when nothing is counting or the three holds of
 * this boot are used (the body says which). */
static esp_err_t hold_handler(httpd_req_t *req)
{
    char buf[96];
    int minutes = 10;
    int len = httpd_req_recv(req, buf, (req->content_len < sizeof(buf) - 1)
                                           ? req->content_len
                                           : sizeof(buf) - 1);

    if (len > 0)
    {
        buf[len] = '\0';

        cJSON *in = cJSON_Parse(buf);
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(in, "minutes");

        if (in == NULL || !cJSON_IsNumber(m))
        {
            cJSON_Delete(in);
            return send_error(req, "400 Bad Request", "need minutes 1..30");
        }

        minutes = m->valueint;
        cJSON_Delete(in);
    }

    if (minutes < 1 || minutes > 30)
    {
        return send_error(req, "400 Bad Request", "minutes must be 1..30");
    }

    esp_err_t err = sleep_manager_hold((uint32_t)minutes);

    if (err == ESP_ERR_NOT_ALLOWED)
    {
        return send_error(req, "409 Conflict", "hold limit reached");
    }

    if (err == ESP_ERR_INVALID_STATE)
    {
        return send_error(req, "409 Conflict", "nothing is counting");
    }

    if (err != ESP_OK)
    {
        return send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }

    return sleep_handler(req);
}

esp_err_t sleep_manager_register_http(void)
{
    static const httpd_uri_t URIS[] =
    {
        { .uri = "/api/sleep", .method = HTTP_GET,
          .handler = sleep_handler },
        { .uri = "/api/sleep/hold", .method = HTTP_POST,
          .handler = hold_handler },
    };

    esp_err_t err = http_server_manager_register_handlers(
        URIS, sizeof(URIS) / sizeof(URIS[0]));

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "/api/sleep registered");
    }

    return err;
}
