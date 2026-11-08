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
 * @file wifi_manager_http.c
 * @brief The component's own HTTP routes (HTTP_API.md §7): /api/wifi/status
 *        and /api/wifi/scan. Feature components register their routes
 *        themselves: main calls wifi_manager_register_http() only in
 *        compositions that include http_server_manager, keeping the radio
 *        code free of any HTTP dependency.
 *
 * Config stays strictly on /api/settings/wifi_manager (api_http glue);
 * there are deliberately no bespoke config routes here.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "cJSON.h"
#include "http_server_manager.h"

#include "wifi_manager.h"
#include "wifi_manager_private.h" /* wm_settings_config, the factory password */

static const char *TAG = "wifi_manager";

static esp_err_t send_json(httpd_req_t *req, cJSON *obj)
{
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

static esp_err_t wifi_status_handler(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    char ip[16] = "";
    char dns_main[16] = "";
    char dns_backup[16] = "";

    wifi_manager_get_sta_ip(ip, sizeof(ip));
    wifi_manager_get_sta_dns(dns_main, sizeof(dns_main),
                             dns_backup, sizeof(dns_backup));

    cJSON_AddBoolToObject(resp, "enabled", wifi_manager_is_enabled());
    cJSON_AddBoolToObject(resp, "sta_connected",
                          wifi_manager_is_sta_connected());
    cJSON_AddStringToObject(resp, "ip", ip);

    /* the network the station is on (2026-11-08: the destinations and the
       Home Assistant card name it beside their "Active on" choice); ""
       while disconnected */
    {
        char ssid[33] = "";

        (void)wifi_manager_get_sta_ssid(ssid, sizeof(ssid));
        cJSON_AddStringToObject(resp, "ssid", ssid);
    }
    cJSON_AddBoolToObject(resp, "ap_started", wifi_manager_is_ap_started());
    /* the factory AP password is public: the web UI warns and gates Submit */
    {
        const wm_config_t *wc = wm_settings_config();

        cJSON_AddBoolToObject(resp, "ap_default_password",
                              wc != NULL &&
                              strcmp(wc->ap.password,
                                     WM_AP_PASSWORD_DEFAULT) == 0);
    }
    cJSON_AddNumberToObject(resp, "clients",
                            wifi_manager_get_ap_station_count());

    char ap_ip[16] = "";

    if (wifi_manager_get_ap_ip(ap_ip, sizeof(ap_ip)) == ESP_OK)
    {
        cJSON_AddStringToObject(resp, "ap_ip", ap_ip);
    }

    cJSON *dns = cJSON_AddArrayToObject(resp, "dns");

    cJSON_AddItemToArray(dns, cJSON_CreateString(dns_main));
    cJSON_AddItemToArray(dns, cJSON_CreateString(dns_backup));

    /* the last station attempt: lets the UI say WHY it is not connected */
    wifi_manager_sta_attempt_t at;

    wifi_manager_get_sta_attempt(&at);

    cJSON *o = cJSON_AddObjectToObject(resp, "sta_attempt");

    cJSON_AddStringToObject(o, "ssid", at.ssid);
    cJSON_AddNumberToObject(o, "reason", at.last_reason);
    cJSON_AddNumberToObject(o, "fail_count", at.fail_count);
    cJSON_AddBoolToObject(o, "deprioritised", at.deprioritised);
    return send_json(req, resp);
}

static esp_err_t send_error(httpd_req_t *req, const char *status,
                            const char *msg)
{
    cJSON *o = cJSON_CreateObject();

    cJSON_AddStringToObject(o, "error", msg);
    httpd_resp_set_status(req, status);
    return send_json(req, o);
}

/* the connection trial's status: what the wizard polls once a second */
static cJSON *try_json(void)
{
    wifi_manager_try_t t;
    cJSON *o = cJSON_CreateObject();

    wifi_manager_sta_try_status(&t);
    cJSON_AddStringToObject(o, "state", t.state);
    cJSON_AddStringToObject(o, "ssid", t.ssid);
    cJSON_AddStringToObject(o, "result", t.result);
    cJSON_AddNumberToObject(o, "reason", t.reason);

    if (t.ip[0] != '\0')
    {
        cJSON_AddStringToObject(o, "ip", t.ip);
        cJSON_AddNumberToObject(o, "rssi", t.rssi);
    }

    if (t.channel != 0)
    {
        cJSON_AddNumberToObject(o, "channel", t.channel);
    }

    cJSON_AddNumberToObject(o, "took_ms", t.took_ms);
    cJSON_AddNumberToObject(o, "age_s", t.age_s);
    return o;
}

static esp_err_t wifi_try_get_handler(httpd_req_t *req)
{
    return send_json(req, try_json());
}

/* POST /api/wifi/try {"ssid": "...", "password": "..."}: start a trial.
   Asynchronous on purpose: the phone sits on our access point, and the
   radio moves to the network's channel for the trial, so a request that
   waited for the result would die with that blink. 202 and poll. */
static esp_err_t wifi_try_post_handler(httpd_req_t *req)
{
    char buf[192];
    int len = httpd_req_recv(req, buf, (req->content_len < sizeof(buf) - 1)
                                           ? req->content_len
                                           : sizeof(buf) - 1);

    if (len <= 0)
    {
        return send_error(req, "400 Bad Request", "need ssid and password");
    }

    buf[len] = '\0';

    cJSON *in = cJSON_Parse(buf);
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(in, "ssid");
    const cJSON *pw = cJSON_GetObjectItemCaseSensitive(in, "password");
    const cJSON *ch = cJSON_GetObjectItemCaseSensitive(in, "channel");

    if (in == NULL || !cJSON_IsString(ssid) ||
        (pw != NULL && !cJSON_IsString(pw)))
    {
        cJSON_Delete(in);
        return send_error(req, "400 Bad Request", "need ssid and password");
    }

    /* the scan row's channel, optional (2026-10-08): scanned first and
       alone; anything but 1..13 means all channels */
    uint8_t channel = (cJSON_IsNumber(ch) && ch->valuedouble >= 1 &&
                       ch->valuedouble <= 13)
                          ? (uint8_t)ch->valuedouble : 0;

    esp_err_t err = wifi_manager_sta_try(ssid->valuestring,
                                         pw != NULL ? pw->valuestring : "",
                                         channel);

    cJSON_Delete(in);

    if (err == ESP_ERR_INVALID_ARG)
    {
        return send_error(req, "400 Bad Request",
                          "need ssid (1..32) and password (8..63)");
    }

    if (err == ESP_ERR_INVALID_STATE)
    {
        return send_error(req, "409 Conflict", "a test is already running");
    }

    if (err == ESP_ERR_NOT_SUPPORTED)
    {
        return send_error(req, "409 Conflict",
                          "the station is off: the test needs Access point "
                          "+ Station");
    }

    if (err != ESP_OK)
    {
        return send_error(req, "500 Internal Server Error",
                          esp_err_to_name(err));
    }

    httpd_resp_set_status(req, "202 Accepted");
    return send_json(req, try_json());
}

static esp_err_t wifi_scan_handler(httpd_req_t *req)
{
    /* blocking ≈2 s (the UI shows a spinner): the JSON goes out verbatim */
    char *json = wifi_manager_scan_networks();

    if (json == NULL)
    {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"error\":\"scan unavailable\"}");
    }

    httpd_resp_set_type(req, "application/json");

    esp_err_t err = httpd_resp_sendstr(req, json);

    free(json);
    return err;
}

esp_err_t wifi_manager_register_http(void)
{
    static const httpd_uri_t URIS[] =
    {
        { .uri = "/api/wifi/status", .method = HTTP_GET,
          .handler = wifi_status_handler },
        { .uri = "/api/wifi/scan", .method = HTTP_GET,
          .handler = wifi_scan_handler },
        { .uri = "/api/wifi/try", .method = HTTP_GET,
          .handler = wifi_try_get_handler },
        { .uri = "/api/wifi/try", .method = HTTP_POST,
          .handler = wifi_try_post_handler },
    };

    esp_err_t err = http_server_manager_register_handlers(
        URIS, sizeof(URIS) / sizeof(URIS[0]));

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "/api/wifi routes registered");
    }

    return err;
}
