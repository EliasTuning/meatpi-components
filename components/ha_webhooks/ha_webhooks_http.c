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
 * @file ha_webhooks_http.c
 * @brief `/api/webhook` GET/POST/DELETE: the HA HACS integration's
 *        discovery/registration endpoint (kept verbatim for compat).
 *        Feature-component route (§9.1); the network-trust gate wraps it.
 */
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "cJSON.h"
#include "http_server_manager.h"
#include "wifi_manager.h" /* "active on" (2026-11-08) */

#include "ha_webhooks_private.h"

static const char *TAG = "ha_webhooks";

#define HW_BODY_MAX 2048

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

static cJSON *response_obj(void)
{
    hw_config_t c = { 0 };
    hw_stats_t s;

    (void)hw_config_get(&c);
    hw_stats_get(&s);

    cJSON *o = cJSON_CreateObject();

    if (o == NULL)
    {
        return NULL;
    }

    cJSON_AddStringToObject(o, "url", c.url);

    if (c.url2[0] != '\0')
    {
        cJSON *urls = cJSON_AddArrayToObject(o, "urls");

        cJSON_AddItemToArray(urls, cJSON_CreateString(c.url));
        cJSON_AddItemToArray(urls, cJSON_CreateString(c.url2));
    }

    cJSON_AddBoolToObject(o, "enabled", c.enabled);
    cJSON_AddNumberToObject(o, "interval", (double)c.interval_s);
    cJSON_AddBoolToObject(o, "manual_override", c.manual_override);
    cJSON_AddStringToObject(o, "data_mode",
                            c.data_mode_full ? "full" : "changed");
    cJSON_AddBoolToObject(o, "gzip", c.gzip);
    /* "active on" (2026-11-08): the list and whether the station is on
       one of its networks now (true with an empty list) */
    cJSON_AddStringToObject(o, "networks", c.networks);
    cJSON_AddBoolToObject(o, "on_network",
                          wifi_manager_sta_on_network(c.networks));
    cJSON_AddStringToObject(o, "status",
                            s.status[0] ? s.status : "disabled");
    cJSON_AddStringToObject(o, "last_post", s.last_post);
    cJSON_AddNumberToObject(o, "retries", (double)s.retries);
    cJSON_AddNumberToObject(o, "success_count", (double)s.success_count);
    cJSON_AddNumberToObject(o, "fail_count", (double)s.fail_count);
    cJSON_AddStringToObject(o, "last_error", s.last_error);
    cJSON_AddStringToObject(o, "last_error_time", s.last_error_time);

    /* settings in the push reply (2026-11-14): the revision the pushes
       report ("" = none, or the settings were changed since it was stored)
       and what became of the reply bodies since boot */
    cJSON_AddStringToObject(o, "rev", hw_rev_reported(&c));

    cJSON *reply = cJSON_AddObjectToObject(o, "reply");

    if (reply != NULL)
    {
        cJSON_AddNumberToObject(reply, "applied", (double)s.reply_applied);
        cJSON_AddNumberToObject(reply, "ignored", (double)s.reply_ignored);
        cJSON_AddStringToObject(reply, "last", s.reply_last);
    }

    return o;
}

static esp_err_t get_handler(httpd_req_t *req)
{
    return send_json(req, response_obj(), "200 OK");
}

static esp_err_t post_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > HW_BODY_MAX)
    {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "length");
    }

    char *buf = malloc(req->content_len + 1);

    if (buf == NULL)
    {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "mem");
    }

    size_t got = 0;

    while (got < req->content_len)
    {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);

        if (r <= 0)
        {
            free(buf);
            return httpd_resp_send_err(req,
                                       HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "recv");
        }

        got += (size_t)r;
    }

    buf[got] = '\0';

    cJSON *root = cJSON_Parse(buf);

    free(buf);

    if (root == NULL)
    {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
    }

    hw_config_t cur = { 0 };
    bool have = hw_config_get(&cur);
    bool first = !have || cur.url[0] == '\0';

    const cJSON *mo = cJSON_GetObjectItemCaseSensitive(root,
                                                       "manual_override");
    bool has_mo = cJSON_IsBool(mo);

    /* user pinned the URL: ignore an external push that doesn't itself
       carry manual_override */
    if (have && cur.manual_override && !has_mo)
    {
        cJSON_Delete(root);
        ESP_LOGI(TAG, "manual_override on; ignoring external push");
        return send_json(req, response_obj(), "200 OK");
    }

    /* the fields: shared with the `webhook` object of a push reply. A body
       with a `rev` is held to every field (2026-11-14); one without keeps
       the older, forgiving rules and leaves the device with no revision */
    hw_config_t c;
    hw_reg_err_t bad = hw_reg_parse(root, &cur, false, &c);

    if (bad != HW_REG_OK)
    {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   hw_reg_err_str(bad));
    }

    if (has_mo)
    {
        c.manual_override = cJSON_IsTrue(mo);
    }

    cJSON_Delete(root);

    esp_err_t r = hw_config_apply_live(&c, true);

    if (r != ESP_OK)
    {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "save");
    }

    ESP_LOGI(TAG, "webhook %s: %s (enabled=%d)",
             first ? "created" : "updated", c.url, c.enabled);
    return send_json(req, response_obj(), first ? "201 Created" : "200 OK");
}

static esp_err_t delete_handler(httpd_req_t *req)
{
    hw_config_t c = { 0 };

    (void)hw_config_get(&c);
    hw_config_unregister(&c);
    c.manual_override = false;

    (void)hw_config_apply_live(&c, true);

    hw_stats_t s = { 0 };

    strlcpy(s.status, "disabled", sizeof(s.status));
    hw_stats_set(&s);

    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

esp_err_t ha_webhooks_register_http(void)
{
    static const httpd_uri_t URIS[] = {
        { .uri = "/api/webhook", .method = HTTP_GET, .handler = get_handler },
        { .uri = "/api/webhook", .method = HTTP_POST, .handler = post_handler },
        { .uri = "/api/webhook", .method = HTTP_DELETE,
          .handler = delete_handler },
    };

    return http_server_manager_register_handlers(URIS,
                                                 sizeof(URIS) /
                                                     sizeof(URIS[0]));
}
