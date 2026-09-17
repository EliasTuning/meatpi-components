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
 * @file event_manager_http.c
 * @brief Discovery + observability routes (own-routes pattern §9.1):
 *        the UI builds its rule editor from these — zero hardcoded
 *        dropdowns (API-first §1b).
 *
 * GET /api/events/sources — declared events + their keys/types
 * GET /api/events/actions — registered actions + params_schema
 * GET /api/events/values  — pull-value names (trailing '.' = prefix)
 * GET /api/events/log     — stats + the last 32 events + fired rules
 * GET /api/events/rules   — per-rule runtime: fired count, last fired,
                             active (undo rules) — the list's badges
 */
#include <stdlib.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "http_server_manager.h"

#include "event_manager_private.h"

static const char *TAG = "event_manager";

static esp_err_t send_json(httpd_req_t *req, cJSON *obj)
{
    if (obj == NULL)
    {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "oom");
    }

    char *body = cJSON_PrintUnformatted(obj);

    cJSON_Delete(obj);

    if (body == NULL)
    {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "oom");
    }

    httpd_resp_set_type(req, "application/json");

    esp_err_t err = httpd_resp_sendstr(req, body);

    free(body);
    return err;
}

static esp_err_t sources_handler(httpd_req_t *req)
{
    return send_json(req, em_core_sources_json());
}

static esp_err_t actions_handler(httpd_req_t *req)
{
    return send_json(req, em_core_actions_json());
}

static esp_err_t values_handler(httpd_req_t *req)
{
    return send_json(req, em_core_values_json());
}

static esp_err_t log_handler(httpd_req_t *req)
{
    return send_json(req, em_core_log_json());
}

static esp_err_t rules_handler(httpd_req_t *req)
{
    return send_json(req, em_core_rules_json());
}

esp_err_t event_manager_register_http(void)
{
    static const httpd_uri_t URIS[] =
    {
        { .uri = "/api/events/sources", .method = HTTP_GET,
          .handler = sources_handler },
        { .uri = "/api/events/actions", .method = HTTP_GET,
          .handler = actions_handler },
        { .uri = "/api/events/values", .method = HTTP_GET,
          .handler = values_handler },
        { .uri = "/api/events/log", .method = HTTP_GET,
          .handler = log_handler },
        { .uri = "/api/events/rules", .method = HTTP_GET,
          .handler = rules_handler },
    };

    esp_err_t err = http_server_manager_register_handlers(
        URIS, sizeof(URIS) / sizeof(URIS[0]));

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "/api/events routes registered");
    }

    return err;
}
