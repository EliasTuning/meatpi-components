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
 * @file http_client_manager_events.c
 * @brief event_manager glue: `http.post {url, body, content_type?}`,
 *        the webhook/ABRP-style action. Fire-and-forget with counted
 *        errors, NO retries (design §12.3). A blocking action: it runs
 *        on a worker of the event manager's pool, and the TLS handshake
 *        of an https:// URL runs on that worker's stack (the response
 *        buffer is heap, freed immediately).
 */
#include <string.h>

#include "event_manager.h"

#include "http_client_manager.h"

/* Issue 1067 (2026-11-16): the workers had 4 KB, and the first https://
   post of a rule overflowed the stack and reset the device. The lane this
   action runs on must hold what this component asks of every caller. */
_Static_assert(EVENT_MANAGER_WORKER_STACK >= HTTP_CLIENT_MANAGER_TLS_STACK_MIN,
               "http.post: the event manager's worker stack is below the "
               "https caller budget of http_client_manager.h");

static esp_err_t act_post(const cJSON *with, const em_event_t *ev)
{
    (void)ev;

    const cJSON *url = cJSON_GetObjectItemCaseSensitive(with, "url");
    const cJSON *body = cJSON_GetObjectItemCaseSensitive(with, "body");
    const cJSON *ct = cJSON_GetObjectItemCaseSensitive(with,
                                                       "content_type");

    if (!cJSON_IsString(url) || !cJSON_IsString(body))
    {
        return ESP_ERR_INVALID_ARG;
    }

    http_client_response_t resp = { 0 };
    esp_err_t err = http_client_manager_post(
        url->valuestring, body->valuestring, strlen(body->valuestring),
        cJSON_IsString(ct) ? ct->valuestring : "application/json",
        &resp);

    if (err == ESP_OK && (resp.status_code < 200 ||
                          resp.status_code >= 300))
    {
        err = ESP_FAIL;
    }

    http_client_manager_free(&resp);
    return err;
}

void hcm_events_register(void)
{
    static const em_action_t POST =
    {
        .name = "http.post",
        .params_schema =
            "{\"type\":\"object\",\"properties\":{"
            "\"url\":{\"type\":\"string\",\"minLength\":1},"
            "\"body\":{\"type\":\"string\"},"
            "\"content_type\":{\"type\":\"string\"}},"
            "\"required\":[\"url\",\"body\"]}",
        .run = act_post,
        .blocking = true, /* HTTP+TLS round-trip: off the dispatcher */
    };

    (void)event_manager_register_action(&POST);
}
