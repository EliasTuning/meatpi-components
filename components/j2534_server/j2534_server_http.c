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
 * @file j2534_server_http.c
 * @brief GET /api/j2534 — J2534 PassThru server status (UI + bench).
 */
#include <stdlib.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "http_server_manager.h"

#include "j2534_server.h"

static const char *TAG = "j2534_server";

static esp_err_t status_handler(httpd_req_t *req)
{
    j2534_server_status_t st;

    if (j2534_server_status(&st) != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status");
        return ESP_FAIL;
    }

    cJSON *o = cJSON_CreateObject();

    cJSON_AddBoolToObject(o, "enabled", st.enabled);
    cJSON_AddNumberToObject(o, "port", st.port);
    cJSON_AddBoolToObject(o, "listening", st.listening);
    cJSON_AddBoolToObject(o, "client_connected", st.client_connected);
    cJSON_AddBoolToObject(o, "device_open", st.device_open);
    cJSON_AddNumberToObject(o, "channels", st.channel_count);
    cJSON_AddNumberToObject(o, "frames_rx", st.frames_rx);
    cJSON_AddNumberToObject(o, "frames_tx", st.frames_tx);
    cJSON_AddBoolToObject(o, "allow_reflash", st.allow_reflash);
    cJSON_AddBoolToObject(o, "allow_lan", st.allow_lan);
    cJSON_AddBoolToObject(o, "exclusive", st.exclusive);
    cJSON_AddBoolToObject(o, "autopid_paused", st.autopid_paused);
    cJSON_AddStringToObject(o, "phase", "2 (CAN + ISO15765 channels)");

    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);

    if (s == NULL)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_send(req, s, HTTPD_RESP_USE_STRLEN);
    free(s);
    return r;
}

/* POST /api/j2534 {"exclusive":bool} — the runtime switch (boot default =
 * the setting); answers with the status like GET */
static esp_err_t control_handler(httpd_req_t *req)
{
    char body[128];
    int n = (req->content_len > 0 && req->content_len < sizeof(body))
                ? httpd_req_recv(req, body, sizeof(body) - 1) : 0;

    if (n <= 0)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body");
        return ESP_FAIL;
    }

    body[n] = '\0';

    cJSON *root = cJSON_Parse(body);
    const cJSON *v = (root != NULL)
        ? cJSON_GetObjectItemCaseSensitive(root, "exclusive") : NULL;

    if (!cJSON_IsBool(v))
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "expected {\"exclusive\":bool}");
        return ESP_FAIL;
    }

    j2534_server_set_exclusive(cJSON_IsTrue(v));
    cJSON_Delete(root);
    return status_handler(req);
}

esp_err_t j2534_server_register_http(void)
{
    static const httpd_uri_t URIS[] =
    {
        { .uri = "/api/j2534", .method = HTTP_GET,
          .handler = status_handler },
        { .uri = "/api/j2534", .method = HTTP_POST,
          .handler = control_handler },
    };

    esp_err_t err = http_server_manager_register_handlers(
        URIS, sizeof(URIS) / sizeof(URIS[0]));

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "/api/j2534 registered");
    }

    return err;
}
