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
 * @file data_destinations_http.c
 * @brief `/api/destinations` (GET: status + counters per destination) and
 *        `/api/destinations/test` (POST {"name"}: deliver once, now).
 *        Own-routes pattern (§9.1); HTTP_API.md §6e14.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"

#include "cJSON.h"
#include "http_server_manager.h"

#include "data_destinations.h"
#include "data_destinations_private.h"

#define DD_BODY_MAX 256

static esp_err_t send_json(httpd_req_t *req, cJSON *o, const char *status)
{
    char *js = cJSON_PrintUnformatted(o);

    cJSON_Delete(o);

    if (js == NULL)
    {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "serialize");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, status);
    httpd_resp_send(req, js, strlen(js));
    free(js);
    return ESP_OK;
}

static esp_err_t send_error(httpd_req_t *req, const char *status,
                            const char *msg)
{
    cJSON *o = cJSON_CreateObject();

    if (o == NULL)
    {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "mem");
    }

    cJSON_AddStringToObject(o, "error", msg);
    return send_json(req, o, status);
}

static esp_err_t get_handler(httpd_req_t *req)
{
    cJSON *o = NULL;

    if (data_destinations_status(&o) != ESP_OK || o == NULL)
    {
        return send_error(req, "503 Service Unavailable", "not configured");
    }

    return send_json(req, o, "200 OK");
}

static esp_err_t test_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > DD_BODY_MAX)
    {
        return send_error(req, "400 Bad Request", "body");
    }

    char buf[DD_BODY_MAX + 1];
    size_t got = 0;

    while (got < req->content_len)
    {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);

        if (r <= 0)
        {
            return send_error(req, "400 Bad Request", "recv");
        }

        got += (size_t)r;
    }

    buf[got] = '\0';

    cJSON *root = cJSON_Parse(buf);
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");

    if (!cJSON_IsString(name) || name->valuestring[0] == '\0')
    {
        cJSON_Delete(root);
        return send_error(req, "400 Bad Request", "name required");
    }

    data_destinations_result_t res = { 0 };
    esp_err_t rc = data_destinations_test(name->valuestring, &res);

    cJSON_Delete(root);

    if (rc == ESP_ERR_NOT_FOUND)
    {
        return send_error(req, "404 Not Found", "unknown destination");
    }

    if (rc == ESP_ERR_INVALID_STATE)
    {
        return send_error(req, "409 Conflict",
                          "another test is running or the poster is off");
    }

    if (rc == ESP_ERR_TIMEOUT)
    {
        return send_error(req, "504 Gateway Timeout", "poster did not answer");
    }

    if (rc != ESP_OK)
    {
        return send_error(req, "500 Internal Server Error",
                          esp_err_to_name(rc));
    }

    cJSON *o = cJSON_CreateObject();

    if (o == NULL)
    {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "mem");
    }

    cJSON_AddBoolToObject(o, "ok", res.ok);
    cJSON_AddNumberToObject(o, "status", res.status);
    cJSON_AddNumberToObject(o, "elapsed_ms", (double)res.elapsed_ms);
    cJSON_AddStringToObject(o, "error", res.error);
    return send_json(req, o, "200 OK");
}

esp_err_t data_destinations_register_http(void)
{
    static const httpd_uri_t URIS[] =
    {
        { .uri = "/api/destinations", .method = HTTP_GET,
          .handler = get_handler },
        { .uri = "/api/destinations/test", .method = HTTP_POST,
          .handler = test_handler },
    };

    return http_server_manager_register_handlers(
        URIS, sizeof(URIS) / sizeof(URIS[0]));
}
