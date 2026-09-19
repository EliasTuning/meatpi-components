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
 * @file data_destinations_post.c
 * @brief The transports: payload builders (snapshot + timestamp, the
 *        status block, the legacy first-push {config,status,autopid_data})
 *        and one delivery per type — MQTT publish (direct path: the
 *        poster is a dedicated task and snapshots can exceed the async
 *        ring's 4 KB), HTTP(S) JSON POST with the auth modes / extra
 *        query / cert set, ABRP form POST with api_key query or header.
 *        Every buffer here is heap (PSRAM by size) — the poster's stack
 *        carries only the TLS handshake.
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "cJSON.h"

#include "autopid.h"
#include "battery_monitor.h"
#include "dev_status_manager.h"
#include "http_client_manager.h"
#include "mqtt_manager.h"

#include "data_destinations_private.h"

static const char *TAG = "data_destinations";

/* ---- payloads --------------------------------------------------------------- */

static cJSON *snapshot_with_ts(void)
{
    cJSON *snap = NULL;

    if (autopid_snapshot(&snap) != ESP_OK || snap == NULL)
    {
        snap = cJSON_CreateObject();
    }

    if (snap != NULL)
    {
        cJSON_AddNumberToObject(snap, "timestamp", (double)time(NULL));
    }

    return snap;
}

char *dd_build_data_json(void)
{
    cJSON *snap = snapshot_with_ts();

    if (snap == NULL)
    {
        return NULL;
    }

    char *s = cJSON_PrintUnformatted(snap);

    cJSON_Delete(snap);
    return s;
}

/** The small status block of the first HTTP push: identity + what a
 *  receiver needs to interpret the data (no secrets, no wifi details). */
static cJSON *build_status(void)
{
    cJSON *s = cJSON_CreateObject();

    if (s == NULL)
    {
        return NULL;
    }

    cJSON_AddStringToObject(s, "device_id", dev_status_manager_device_id());
    cJSON_AddStringToObject(s, "fw_version",
                            dev_status_manager_app_version());
    cJSON_AddStringToObject(s, "hw_version", CONFIG_WICAN_HW_VERSION);
    cJSON_AddStringToObject(s, "device_type", CONFIG_WICAN_DEVICE_TYPE);

    char uptime[DD_TS_LEN] = "";

    dev_status_manager_format_uptime(uptime, sizeof(uptime));
    cJSON_AddStringToObject(s, "uptime", uptime);
    cJSON_AddBoolToObject(s, "network_connected",
                          dev_status_manager_any_set(
                              DEV_STATUS_NETWORK_CONNECTED_MASK));
    cJSON_AddBoolToObject(s, "mqtt_connected", mqtt_manager_connected());
    cJSON_AddStringToObject(s, "ecu_status",
                            autopid_ecu_online() ? "online" : "offline");

    float volts = 0;

    if (battery_monitor_voltage(&volts) == ESP_OK)
    {
        cJSON_AddNumberToObject(s, "batt_voltage",
                                (double)((int)(volts * 100)) / 100.0);
    }

    cJSON_AddNumberToObject(s, "timestamp", (double)time(NULL));
    return s;
}

/** `{"autopid_data":{...}}`, or with `config` + `status` in front. */
static char *build_http_body(bool full)
{
    cJSON *root = cJSON_CreateObject();

    if (root == NULL)
    {
        return NULL;
    }

    if (full)
    {
        char *cfg_json = NULL;

        if (autopid_config_json_dup(&cfg_json) == ESP_OK && cfg_json != NULL)
        {
            cJSON *cfg = cJSON_Parse(cfg_json);

            free(cfg_json);
            cJSON_AddItemToObject(root, "config",
                                  cfg != NULL ? cfg : cJSON_CreateObject());
        }
        else
        {
            cJSON_AddItemToObject(root, "config", cJSON_CreateObject());
        }

        cJSON *st = build_status();

        cJSON_AddItemToObject(root, "status",
                              st != NULL ? st : cJSON_CreateObject());
    }

    cJSON *snap = snapshot_with_ts();

    cJSON_AddItemToObject(root, "autopid_data",
                          snap != NULL ? snap : cJSON_CreateObject());

    char *body = cJSON_PrintUnformatted(root);

    cJSON_Delete(root);
    return body;
}

/* ---- transports ------------------------------------------------------------- */

static void set_err(data_destinations_result_t *r, const char *fmt,
                    const char *a)
{
    snprintf(r->error, sizeof(r->error), fmt, a);
}

static bool post_mqtt(const dd_dest_t *d, data_destinations_result_t *r)
{
    char topic[128];

    if (!dd_topic_expand(d->url, mqtt_manager_topic_prefix(), topic,
                         sizeof(topic)))
    {
        set_err(r, "mqtt: topic too long%s", "");
        return false;
    }

    char *body = dd_build_data_json();

    if (body == NULL)
    {
        set_err(r, "mqtt: no payload%s", "");
        return false;
    }

    esp_err_t rc = mqtt_manager_publish(topic, body, strlen(body), 0,
                                        d->retain);

    cJSON_free(body);

    if (rc != ESP_OK)
    {
        set_err(r, "mqtt: %s", esp_err_to_name(rc));
        return false;
    }

    return true;
}

/** Shared HTTP request driver: fills @p r from the outcome. */
static bool http_send(const dd_dest_t *d, const char *url, const void *body,
                      size_t body_len, const char *content_type,
                      const http_client_auth_t *auth,
                      const char *const *headers, size_t n_headers,
                      data_destinations_result_t *r, char **resp_body)
{
    http_client_request_t req =
    {
        .url = url,
        .method = HTTP_CLIENT_POST,
        .body = body,
        .body_len = body_len,
        .content_type = content_type,
        .auth = auth,
        .extra_headers = headers,
        .extra_header_count = n_headers,
        .cert_set = d->cert_set[0] != '\0' ? d->cert_set : NULL,
        .skip_common_name = dd_url_host_is_ip(url),
        .timeout_ms = DD_HTTP_TIMEOUT_MS,
        .max_response = 4096,
    };
    http_client_response_t resp = { 0 };
    esp_err_t rc = http_client_manager_request(&req, &resp);

    if (rc != ESP_OK)
    {
        r->status = 0;
        set_err(r, "esp_err=%s", esp_err_to_name(rc));
        http_client_manager_free(&resp);
        return false;
    }

    r->status = resp.status_code;

    bool ok = resp.status_code >= 200 && resp.status_code < 300;

    if (!ok)
    {
        char code[16];

        snprintf(code, sizeof(code), "%d", resp.status_code);
        set_err(r, "http=%s", code);
    }

    if (resp_body != NULL && resp.data != NULL)
    {
        *resp_body = resp.data; /* ownership to the caller */
        resp.data = NULL;
    }

    http_client_manager_free(&resp);
    return ok;
}

static bool post_http(const dd_dest_t *d, dd_state_t *st,
                      data_destinations_result_t *r)
{
    bool full = d->full_first && !st->settings_sent;
    char *body = build_http_body(full);

    if (body == NULL)
    {
        set_err(r, "http: no payload%s", "");
        return false;
    }

    http_client_auth_t auth = { 0 };
    const char *qkey = NULL;
    const char *qval = NULL;

    switch (d->auth)
    {
    case DD_AUTH_BEARER:
        auth.bearer_token = d->auth_token;
        break;
    case DD_AUTH_API_KEY_HEADER:
        auth.api_key = d->auth_token;
        auth.api_key_header = d->auth_name;
        break;
    case DD_AUTH_API_KEY_QUERY:
        qkey = d->auth_name;
        qval = d->auth_token;
        break;
    case DD_AUTH_BASIC:
        auth.basic_username = d->basic_username;
        auth.basic_password = d->basic_password;
        break;
    default:
        break;
    }

    char *url = heap_caps_malloc(DD_URL_LEN + DD_QUERY_LEN + DD_SECRET_LEN * 3,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (url == NULL ||
        !dd_url_compose(d->url, d->query, qkey, qval, url,
                        DD_URL_LEN + DD_QUERY_LEN + DD_SECRET_LEN * 3))
    {
        free(url);
        cJSON_free(body);
        set_err(r, "http: url too long%s", "");
        return false;
    }

    bool ok = http_send(d, url, body, strlen(body), "application/json",
                        &auth, NULL, 0, r, NULL);

    if (ok && full)
    {
        st->settings_sent = true; /* legacy: config+status once, then data */
    }

    free(url);
    cJSON_free(body);
    return ok;
}

static bool post_abrp(const dd_dest_t *d, data_destinations_result_t *r)
{
    cJSON *snap = NULL;

    if (autopid_snapshot(&snap) != ESP_OK || snap == NULL)
    {
        snap = cJSON_CreateObject();
    }

    cJSON *tlm = dd_abrp_tlm(snap, d->car_model, (int64_t)time(NULL));

    cJSON_Delete(snap);

    char *tlm_json = tlm != NULL ? cJSON_PrintUnformatted(tlm) : NULL;

    cJSON_Delete(tlm);

    if (tlm_json == NULL)
    {
        set_err(r, "abrp: no payload%s", "");
        return false;
    }

    size_t body_cap = (strlen(tlm_json) + strlen(d->auth_token)) * 3 + 32;
    char *body = heap_caps_malloc(body_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool built = body != NULL &&
                 dd_abrp_form(d->auth_token, tlm_json, body, body_cap);

    cJSON_free(tlm_json);

    if (!built)
    {
        free(body);
        set_err(r, "abrp: form too long%s", "");
        return false;
    }

    /* api_key: query parameter (default, Iternio's documented form) or
       an `Authorization: APIKEY <key>` header when auth says so */
    char hdr[DD_SMALL_LEN + DD_SECRET_LEN + 16]; /* "<name>: APIKEY <key>" */
    const char *headers[1] = { hdr };
    size_t n_headers = 0;
    const char *qkey = NULL;
    const char *qval = NULL;

    if (d->api_key[0] != '\0')
    {
        if (d->auth == DD_AUTH_API_KEY_HEADER)
        {
            char value[DD_SECRET_LEN + 8];

            if (dd_abrp_auth_value(d->api_key, value, sizeof(value)))
            {
                snprintf(hdr, sizeof(hdr), "%s: %s", d->auth_name, value);
                n_headers = 1;
            }
        }
        else
        {
            qkey = d->auth == DD_AUTH_API_KEY_QUERY ? d->auth_name : "api_key";
            qval = d->api_key;
        }
    }
    else
    {
        ESP_LOGD(TAG, "%s: no ABRP api_key configured", d->name);
    }

    size_t url_cap = DD_URL_LEN + DD_QUERY_LEN + DD_SECRET_LEN * 3;
    char *url = heap_caps_malloc(url_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (url == NULL ||
        !dd_url_compose(d->url, d->query, qkey, qval, url, url_cap))
    {
        free(url);
        free(body);
        set_err(r, "abrp: url too long%s", "");
        return false;
    }

    char *resp = NULL;
    bool http_ok = http_send(d, url, body, strlen(body),
                             "application/x-www-form-urlencoded", NULL,
                             n_headers ? headers : NULL, n_headers, r,
                             &resp);
    bool ok = http_ok;

    if (r->status != 0) /* the endpoint answered: read Iternio's verdict */
    {
        char err[DD_ERR_LEN] = "";

        ok = dd_abrp_response_ok(resp, http_ok, err, sizeof(err));

        if (!ok && err[0] != '\0')
        {
            set_err(r, "%s", err);
        }
    }

    free(resp);
    free(url);
    free(body);
    return ok;
}

bool dd_post_one(const dd_dest_t *d, dd_state_t *st,
                 data_destinations_result_t *out)
{
    int64_t t0 = esp_timer_get_time();

    memset(out, 0, sizeof(*out));

    switch (d->type)
    {
    case DD_TYPE_MQTT:
        out->ok = post_mqtt(d, out);
        break;
    case DD_TYPE_HTTP:
    case DD_TYPE_HTTPS:
        out->ok = post_http(d, st, out);
        break;
    case DD_TYPE_ABRP:
        out->ok = post_abrp(d, out);
        break;
    }

    out->elapsed_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    return out->ok;
}
