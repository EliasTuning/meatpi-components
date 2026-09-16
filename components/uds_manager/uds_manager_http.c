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
 * @file uds_manager_http.c
 * @brief The UDS terminal's HTTP surface: GET /api/uds (status: the live
 *        path, the exclusive switch, the last transaction), POST /api/uds
 *        (the runtime exclusive switch), POST /api/uds/request (one
 *        request), POST /api/uds/session (tester-present session).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "can_isotp_esp.h"
#include "http_server_manager.h"

#include "uds_manager.h"
#include "uds_proto.h"

/* Big buffers live on the HEAP (PSRAM), not the 4 KB httpd worker stack. */
#define UDS_RESP_CAP 4096
#define UDS_HEX_CAP  (3 * UDS_RESP_CAP + 1) /* the whole PDU, PSRAM */
#define UDS_REQ_MAX  64                      /* one AT line on the chip */

static const char *TAG = "uds_manager";

static esp_err_t send_json(httpd_req_t *req, cJSON *o)
{
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

static uint32_t id_of(const cJSON *root, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);

    if (cJSON_IsNumber(v))
    {
        return (uint32_t)v->valuedouble;
    }

    if (cJSON_IsString(v) && v->valuestring)
    {
        return (uint32_t)strtoul(v->valuestring, NULL, 16); /* "7E0" */
    }

    return 0;
}

static esp_err_t request_handler(httpd_req_t *req)
{
    size_t len = req->content_len;

    if (len == 0 || len > 1024)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing/big body");
        return ESP_FAIL;
    }

    /* everything heap-backed: the httpd worker stack is only ~4 KB and
     * the transport call chain (obd_chip / isotp) is deep. */
    char *body = malloc(len + 1);

    if (body == NULL)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    size_t got = 0;

    while (got < len)
    {
        int r = httpd_req_recv(req, body + got, len - got);

        if (r <= 0)
        {
            free(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv");
            return ESP_FAIL;
        }

        got += (size_t)r;
    }

    body[len] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);

    if (root == NULL)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_FAIL;
    }

    uds_addr_t addr =
    {
        .tx_id  = id_of(root, "tx_id"),
        .rx_id  = id_of(root, "rx_id"),
        .ext_id = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root,
                                                                "ext")),
    };

    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    uint8_t reqb[UDS_REQ_MAX];
    size_t reqn = 0;

    if (!cJSON_IsString(data) ||
        !uds_hex_to_bytes(data->valuestring, reqb, sizeof(reqb), &reqn) ||
        reqn == 0)
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "data must be a hex request (<=64 bytes)");
        return ESP_FAIL;
    }

    uds_opts_t opts = { 0 };
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "p2_ms");
    if (cJSON_IsNumber(v)) opts.p2_ms = (uint32_t)v->valueint;
    v = cJSON_GetObjectItemCaseSensitive(root, "p2star_ms");
    if (cJSON_IsNumber(v)) opts.p2star_ms = (uint32_t)v->valueint;
    /* the page's "final response timeout" — the same thing as P2* */
    v = cJSON_GetObjectItemCaseSensitive(root, "timeout_ms");
    if (cJSON_IsNumber(v) && opts.p2star_ms == 0)
    {
        opts.p2star_ms = (uint32_t)v->valueint;
    }
    bool session = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root,
                                                                 "session"));
    cJSON_Delete(root);

    uint8_t *respb = heap_caps_malloc(UDS_RESP_CAP,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *hex = heap_caps_malloc(UDS_HEX_CAP,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (respb == NULL || hex == NULL)
    {
        free(respb);
        free(hex);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    if (session)
    {
        (void)uds_session_begin(&addr);
    }

    size_t respn = 0;
    uds_result_t res;

    esp_err_t err = uds_request(&addr, reqb, reqn, respb, UDS_RESP_CAP,
                                &respn, &opts, &res);

    if (session)
    {
        (void)uds_session_end();
    }

    cJSON *o = cJSON_CreateObject();

    if (err == ESP_ERR_INVALID_STATE)
    {
        free(respb);
        free(hex);
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddStringToObject(o, "error", "busy or transport unavailable");
        httpd_resp_set_status(req, "409 Conflict");
        return send_json(req, o);
    }

    if (err != ESP_OK)
    {
        free(respb);
        free(hex);
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddStringToObject(o, "error", esp_err_to_name(err));
        cJSON_AddStringToObject(o, "backend", res.backend ? res.backend : "");
        return send_json(req, o);
    }

    uds_bytes_to_hex(respb, respn, hex, UDS_HEX_CAP); /* the whole PDU */
    free(respb);

    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "response", hex);
    free(hex); /* cJSON copied it */
    cJSON_AddNumberToObject(o, "length", (double)respn);
    cJSON_AddBoolToObject(o, "positive", !res.negative);
    cJSON_AddNumberToObject(o, "sid", res.sid);
    if (res.negative)
    {
        cJSON_AddNumberToObject(o, "nrc", res.nrc);
        cJSON_AddStringToObject(o, "nrc_name", res.nrc_name ? res.nrc_name
                                                            : "");
    }
    cJSON_AddNumberToObject(o, "pending", res.pending_count);
    cJSON_AddNumberToObject(o, "elapsed_ms", res.elapsed_ms);
    cJSON_AddStringToObject(o, "backend", res.backend ? res.backend : "");
    return send_json(req, o);
}

/* ---- GET /api/uds: the live path without sending anything ----------------- */

static void add_hex_id(cJSON *o, const char *key, uint32_t id)
{
    char s[12];

    snprintf(s, sizeof(s), "%lX", (unsigned long)id);
    cJSON_AddStringToObject(o, key, s);
}

static cJSON *status_json(void)
{
    uds_status_t st;

    uds_manager_get_status(&st);

    cJSON *o = cJSON_CreateObject();

    cJSON_AddStringToObject(o, "backend_setting",
                            uds_manager_backend_name(st.backend_setting));
    cJSON_AddStringToObject(o, "backend_active",
                            uds_manager_backend_name(st.backend_active));
    cJSON_AddStringToObject(o, "provider", st.provider);
    cJSON_AddBoolToObject(o, "can_running", st.can_running);
    cJSON_AddBoolToObject(o, "exclusive", st.exclusive);
    cJSON_AddBoolToObject(o, "exclusive_default", st.exclusive_default);
    cJSON_AddBoolToObject(o, "holding", st.holding);
    cJSON_AddBoolToObject(o, "autopid_paused", st.autopid_paused);
    cJSON_AddBoolToObject(o, "session_active", st.session_active);
    cJSON_AddNumberToObject(o, "exclusive_idle_ms", UDS_EXCLUSIVE_IDLE_MS);
    cJSON_AddNumberToObject(o, "max_request", UDS_REQ_MAX);
    cJSON_AddNumberToObject(o, "max_response", UDS_RESP_CAP);

    if (st.last_ts_us != 0)
    {
        cJSON *l = cJSON_AddObjectToObject(o, "last");

        cJSON_AddNumberToObject(l, "age_ms",
                                (double)((esp_timer_get_time() - st.last_ts_us)
                                         / 1000));
        cJSON_AddBoolToObject(l, "ok", st.last_err == ESP_OK);

        if (st.last_err != ESP_OK)
        {
            cJSON_AddStringToObject(l, "error", esp_err_to_name(st.last_err));
        }

        add_hex_id(l, "tx_id", st.last_tx_id);
        add_hex_id(l, "rx_id", st.last_rx_id);
        cJSON_AddNumberToObject(l, "req_sid", st.last_req_sid);
        cJSON_AddNumberToObject(l, "sid", st.last.sid);
        cJSON_AddBoolToObject(l, "positive", !st.last.negative);

        if (st.last.negative)
        {
            cJSON_AddNumberToObject(l, "nrc", st.last.nrc);
            cJSON_AddStringToObject(l, "nrc_name",
                                    st.last.nrc_name ? st.last.nrc_name : "");
        }

        cJSON_AddNumberToObject(l, "pending", st.last.pending_count);
        cJSON_AddNumberToObject(l, "elapsed_ms", st.last.elapsed_ms);
        cJSON_AddStringToObject(l, "backend",
                                st.last.backend ? st.last.backend : "");
    }
    else
    {
        cJSON_AddNullToObject(o, "last");
    }

    if (can_isotp_esp_active())
    {
        can_isotp_esp_stats_t ps;

        can_isotp_esp_get_stats(&ps);

        cJSON *p = cJSON_AddObjectToObject(o, "provider_stats");

        cJSON_AddNumberToObject(p, "sessions_open", ps.sessions_open);
        cJSON_AddNumberToObject(p, "pdus_tx", ps.pdus_tx);
        cJSON_AddNumberToObject(p, "pdus_rx", ps.pdus_rx);
        cJSON_AddNumberToObject(p, "frames_fed", ps.frames_fed);
        cJSON_AddNumberToObject(p, "tx_timeouts", ps.tx_timeouts);
        cJSON_AddNumberToObject(p, "rx_dropped", ps.rx_dropped);
    }

    return o;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    return send_json(req, status_json());
}

/* small JSON bodies (the control + session routes) */
static cJSON *read_small_body(httpd_req_t *req)
{
    char body[192];

    if (req->content_len == 0 || req->content_len >= sizeof(body))
    {
        return NULL;
    }

    int n = httpd_req_recv(req, body, req->content_len);

    if (n <= 0)
    {
        return NULL;
    }

    body[n] = '\0';
    return cJSON_Parse(body);
}

/* POST /api/uds {"exclusive":bool} — the runtime switch (boot default = the
 * setting); answers with the status like GET */
static esp_err_t control_handler(httpd_req_t *req)
{
    cJSON *root = read_small_body(req);
    const cJSON *v = (root != NULL)
        ? cJSON_GetObjectItemCaseSensitive(root, "exclusive") : NULL;

    if (!cJSON_IsBool(v))
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "expected {\"exclusive\":bool}");
        return ESP_FAIL;
    }

    uds_manager_set_exclusive(cJSON_IsTrue(v));
    cJSON_Delete(root);
    return send_json(req, status_json());
}

/* POST /api/uds/session {"action":"begin","tx_id","rx_id","ext"?} |
 * {"action":"end"} — tester present while the page holds the session;
 * requests inside it ride the held claim. 409 when another transaction/
 * session owns the bus. */
static esp_err_t session_handler(httpd_req_t *req)
{
    cJSON *root = read_small_body(req);
    const cJSON *a = (root != NULL)
        ? cJSON_GetObjectItemCaseSensitive(root, "action") : NULL;

    if (!cJSON_IsString(a) || a->valuestring == NULL)
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "expected {\"action\":\"begin\"|\"end\"}");
        return ESP_FAIL;
    }

    esp_err_t err;

    if (strcmp(a->valuestring, "begin") == 0)
    {
        uds_addr_t addr =
        {
            .tx_id  = id_of(root, "tx_id"),
            .rx_id  = id_of(root, "rx_id"),
            .ext_id = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ext")),
        };

        if (addr.tx_id == 0 || addr.rx_id == 0)
        {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "tx_id/rx_id");
            return ESP_FAIL;
        }

        err = uds_session_begin(&addr);
    }
    else if (strcmp(a->valuestring, "end") == 0)
    {
        err = uds_session_end();
    }
    else
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "action");
        return ESP_FAIL;
    }

    cJSON_Delete(root);

    if (err == ESP_ERR_INVALID_STATE)
    {
        httpd_resp_set_status(req, "409 Conflict");
    }

    cJSON *o = status_json();

    cJSON_AddBoolToObject(o, "ok", err == ESP_OK);

    if (err != ESP_OK)
    {
        cJSON_AddStringToObject(o, "error", (err == ESP_ERR_INVALID_STATE)
                                    ? "busy or transport unavailable"
                                    : esp_err_to_name(err));
    }

    return send_json(req, o);
}

esp_err_t uds_manager_register_http(void)
{
    static const httpd_uri_t URIS[] =
    {
        { .uri = "/api/uds", .method = HTTP_GET,
          .handler = status_handler },
        { .uri = "/api/uds", .method = HTTP_POST,
          .handler = control_handler },
        { .uri = "/api/uds/request", .method = HTTP_POST,
          .handler = request_handler },
        { .uri = "/api/uds/session", .method = HTTP_POST,
          .handler = session_handler },
    };

    esp_err_t err = http_server_manager_register_handlers(
        URIS, sizeof(URIS) / sizeof(URIS[0]));

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "/api/uds routes registered");
    }

    return err;
}
