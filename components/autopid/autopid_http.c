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
 * @file autopid_http.c
 * @brief The /api/autopid routes (own-routes pattern, §9.1) — main calls
 *        autopid_register_http() only in HTTP compositions.
 *
 * GET  /api/autopid          — live cache + groups + stats (the dashboard)
 * GET  /api/autopid/data     — the LEGACY-shape {"Name": value} snapshot
 * GET  /api/autopid/config   — the PID/filter tables file, verbatim
 * PUT  /api/autopid/config   — validate -> atomic save -> LIVE reload
 *                              (the config FILE applies live; settings
 *                              knobs stay reboot-to-apply)
 */
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "battery_monitor.h"
#include "filesystem.h"
#include "http_server_manager.h"

#include "expression_parser.h"

#include "autopid.h"
#include "autopid_private.h"

static const char *TAG = "autopid";

#define AP_HTTP_BODY_MAX (256 * 1024)

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

static esp_err_t send_error(httpd_req_t *req, const char *status,
                            const char *msg)
{
    cJSON *o = cJSON_CreateObject();

    httpd_resp_set_status(req, status);
    cJSON_AddStringToObject(o, "error", msg);
    return send_json(req, o);
}

static esp_err_t autopid_get_handler(httpd_req_t *req)
{
    autopid_stats_t st;

    autopid_stats(&st);

    cJSON *o = cJSON_CreateObject();
    cJSON *groups = cJSON_AddArrayToObject(o, "groups");

    ap_core_group_json(groups);
    cJSON_AddItemToObject(o, "params", ap_cache_detail(ap_core_config()));

    cJSON *stats = cJSON_AddObjectToObject(o, "stats");

    cJSON_AddBoolToObject(stats, "running", st.running);
    cJSON_AddBoolToObject(stats, "paused_voltage", st.paused_voltage);
    cJSON_AddBoolToObject(stats, "paused_client", st.paused_client);
    cJSON_AddBoolToObject(stats, "paused_diag", st.paused_diag);
    cJSON_AddNumberToObject(stats, "polls_ok", st.polls_ok);
    cJSON_AddNumberToObject(stats, "polls_failed", st.polls_failed);
    cJSON_AddNumberToObject(stats, "pids", st.pids_loaded);
    cJSON_AddNumberToObject(stats, "filters", st.filters_loaded);
    /* Phase 1b: ~53 ms/request measured floor; sub-floor periods are
       accepted but flagged so a UI can warn */
    cJSON_AddNumberToObject(stats, "period_floor_ms", AP_PERIOD_FLOOR_MS);
    cJSON_AddNumberToObject(stats, "sub_floor_pids",
                            ap_core_sub_floor_count());
    cJSON_AddNumberToObject(stats, "now_us",
                            (double)esp_timer_get_time());

    return send_json(req, o);
}

static esp_err_t autopid_data_handler(httpd_req_t *req)
{
    cJSON *snap = NULL;

    if (autopid_snapshot(&snap) != ESP_OK)
    {
        return send_error(req, "500 Internal Server Error", "oom");
    }

    return send_json(req, snap);
}

static esp_err_t send_file_or_default(httpd_req_t *req, const char *path,
                                      const char *dflt);

static esp_err_t config_get_handler(httpd_req_t *req)
{
    return send_file_or_default(req, autopid_config_path(),
                                "{\"groups\":[],\"pids\":[],"
                                "\"filters\":[]}");
}

static esp_err_t config_put_handler(httpd_req_t *req)
{
    size_t len = req->content_len;

    if (len == 0 || len > AP_HTTP_BODY_MAX)
    {
        return send_error(req, "400 Bad Request", "missing/oversized body");
    }

    char *body = heap_caps_malloc(len + 1,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (body == NULL)
    {
        return send_error(req, "500 Internal Server Error", "oom");
    }

    size_t got = 0;

    while (got < len)
    {
        int r = httpd_req_recv(req, body + got, len - got);

        if (r <= 0)
        {
            free(body);
            return send_error(req, "400 Bad Request", "body read failed");
        }

        got += (size_t)r;
    }

    body[len] = '\0';

    /* validate BEFORE persisting (expressions, groups, bounds) */
    static ap_config_t s_probe EXT_RAM_BSS_ATTR; /* too big for a stack */
    char perr[96] = "";

    if (ap_config_parse(body, &s_probe, perr, sizeof(perr)) != ESP_OK)
    {
        free(body);
        return send_error(req, "400 Bad Request", perr);
    }

    esp_err_t err = autopid_config_save(body, len);

    free(body);

    if (err != ESP_OK)
    {
        return send_error(req, "500 Internal Server Error", "save failed");
    }

    /* the config file applies LIVE (httpd task = internal stack: the
       reload's file read is §2-safe here) */
    err = autopid_reload_config();

    if (err != ESP_OK)
    {
        return send_error(req, "500 Internal Server Error",
                          "reload failed");
    }

    ESP_LOGI(TAG, "config updated + reloaded (%u pids, %u params)",
             s_probe.n_pids, s_probe.n_params);

    cJSON *o = cJSON_CreateObject();

    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddNumberToObject(o, "pids", s_probe.n_pids);
    cJSON_AddNumberToObject(o, "params", s_probe.n_params);
    return send_json(req, o);
}

static esp_err_t send_file_or_default(httpd_req_t *req, const char *path,
                                      const char *dflt) /* fwd-declared */
{
    size_t size = 0;

    if (filesystem_size(path, &size) != ESP_OK || size == 0)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, dflt);
    }

    char *buf = heap_caps_malloc(size + 1,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (buf == NULL)
    {
        return send_error(req, "500 Internal Server Error", "oom");
    }

    size_t got = 0;

    if (filesystem_read(path, buf, size, &got) != ESP_OK)
    {
        free(buf);
        return send_error(req, "500 Internal Server Error", "read failed");
    }

    buf[got] = '\0';
    httpd_resp_set_type(req, "application/json");

    esp_err_t err = httpd_resp_sendstr(req, buf);

    free(buf);
    return err;
}

static esp_err_t std_scan_post_handler(httpd_req_t *req)
{
    esp_err_t err = autopid_std_scan_start();

    if (err == ESP_ERR_INVALID_STATE)
    {
        return send_error(req, "409 Conflict", "scan already running");
    }

    if (err != ESP_OK)
    {
        return send_error(req, "500 Internal Server Error",
                          "scan start failed");
    }

    httpd_resp_set_status(req, "202 Accepted");

    cJSON *o = cJSON_CreateObject();

    cJSON_AddBoolToObject(o, "started", true);
    return send_json(req, o);
}

static esp_err_t std_scan_get_handler(httpd_req_t *req)
{
    cJSON *o = ap_std_scan_status_json();

    if (o == NULL)
    {
        return send_error(req, "500 Internal Server Error", "oom");
    }

    return send_json(req, o);
}

static esp_err_t std_scan_result_handler(httpd_req_t *req)
{
    return send_file_or_default(req, autopid_std_scan_path(),
                                "{\"supported\":[],\"found\":0}");
}

static esp_err_t std_table_handler(httpd_req_t *req)
{
    cJSON *arr = ap_std_table_json();

    if (arr == NULL)
    {
        return send_error(req, "500 Internal Server Error", "oom");
    }

    return send_json(req, arr);
}

/* test-a-PID (§11): one-shot through the REAL runner path — the ws
   console can't reproduce init/rxheader/expression handling */
static esp_err_t test_post_handler(httpd_req_t *req)
{
    /* PSRAM buffers: an "expressions" list for a 32-parameter DID does
       not fit a 512 B stack body, and the transcript is ~1 KB. Handlers
       run on the one httpd task, so plain statics are safe; the chip
       itself is guarded by the job lock below. */
    static char s_body[2048] EXT_RAM_BSS_ATTR;
    static char s_raw[AP_RESP_MAX] EXT_RAM_BSS_ATTR;
    static char s_tr[1536] EXT_RAM_BSS_ATTR;

    int len = httpd_req_recv(req, s_body, sizeof(s_body) - 1);

    if (len <= 0)
    {
        return send_error(req, "400 Bad Request", "missing body");
    }

    s_body[len] = '\0';

    cJSON *root = cJSON_Parse(s_body);
    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    const cJSON *init = cJSON_GetObjectItemCaseSensitive(root, "init");
    const cJSON *rxh = cJSON_GetObjectItemCaseSensitive(root,
                                                        "rxheader");
    const cJSON *expr = cJSON_GetObjectItemCaseSensitive(root,
                                                         "expression");
    const cJSON *exprs = cJSON_GetObjectItemCaseSensitive(root,
                                                          "expressions");
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");

    if (!cJSON_IsString(cmd) || cmd->valuestring[0] == '\0' ||
        strlen(cmd->valuestring) >= AP_CMD_LEN ||
        (cJSON_IsString(init) &&
         strlen(init->valuestring) >= AP_INIT_LEN) ||
        (cJSON_IsString(rxh) && strlen(rxh->valuestring) >= AP_HDR_LEN))
    {
        cJSON_Delete(root);
        return send_error(req, "400 Bad Request",
                          "cmd required (init/rxheader length caps)");
    }

    char eerr[64];

    if (cJSON_IsString(expr) && expr->valuestring[0] != '\0' &&
        expression_parser_check(expr->valuestring, NULL, eerr,
                                sizeof(eerr)) != ESP_OK)
    {
        cJSON_Delete(root);
        return send_error(req, "400 Bad Request", "bad expression");
    }

    /* "expressions": decode the ONE reply with every parameter of the
       PID (the UI used to fire one request per parameter) */
    if (cJSON_IsArray(exprs))
    {
        if (cJSON_GetArraySize(exprs) > AP_PARAMS_PER)
        {
            cJSON_Delete(root);
            return send_error(req, "400 Bad Request",
                              "too many expressions");
        }

        const cJSON *e = NULL;

        cJSON_ArrayForEach(e, exprs)
        {
            if (!cJSON_IsString(e) || e->valuestring[0] == '\0' ||
                expression_parser_check(e->valuestring, NULL, eerr,
                                        sizeof(eerr)) != ESP_OK)
            {
                cJSON_Delete(root);
                return send_error(req, "400 Bad Request",
                                  "bad expression in expressions[]");
            }
        }
    }

    /* "type": prepend the type init chain the poller sends when it
       switches to this PID's type — the shot then IS a poll of that PID */
    int tidx = -1;

    if (cJSON_IsString(type))
    {
        const char *t = type->valuestring;

        tidx = (strcmp(t, "std") == 0)      ? AP_PID_STD
             : (strcmp(t, "custom") == 0)   ? AP_PID_CUSTOM
             : (strcmp(t, "specific") == 0) ? AP_PID_SPECIFIC
                                            : -1;
    }

    if (!ap_core_job_acquire())  /* vs std scan / dtc jobs / other tests */
    {
        cJSON_Delete(root);
        return send_error(req, "409 Conflict", "another chip job runs");
    }

    ap_core_scan_pause(true);   /* park the poller around the one-shot */

    int64_t elapsed_us = 0;
    esp_err_t err = ap_runner_test(
        (tidx >= 0) ? ap_runner_type_init(tidx) : NULL,
        cJSON_IsString(init) ? init->valuestring : NULL,
        cJSON_IsString(rxh) ? rxh->valuestring : NULL, cmd->valuestring,
        s_raw, sizeof(s_raw), &elapsed_us, s_tr, sizeof(s_tr));

    ap_core_scan_pause(false);

    cJSON *o = cJSON_CreateObject();

    cJSON_AddBoolToObject(o, "ok", err == ESP_OK);
    cJSON_AddNumberToObject(o, "elapsed_ms",
                            (double)elapsed_us / 1000.0);

    if (err != ESP_OK)
    {
        cJSON_AddStringToObject(o, "error",
                                (err == ESP_ERR_INVALID_STATE)
                                    ? "chip busy (monitor/update)"
                                    : "request failed/timeout");
        s_raw[0] = '\0';
    }

    cJSON_AddStringToObject(o, "raw", s_raw);
    cJSON_AddStringToObject(o, "transcript", s_tr);

    uint8_t payload[AP_PAYLOAD_MAX];
    size_t n = 0;
    bool have_payload =
        err == ESP_OK &&
        ap_resp_to_payload(s_raw, payload, sizeof(payload), &n) == ESP_OK;

    if (have_payload)
    {
        char hex[AP_PAYLOAD_MAX * 3 + 1];
        size_t w = 0;

        for (size_t i = 0; i < n; i++)
        {
            w += snprintf(hex + w, sizeof(hex) - w, "%02X%s", payload[i],
                          (i + 1 < n) ? " " : "");
        }

        cJSON_AddStringToObject(o, "payload", hex);
    }
    else if (err == ESP_OK)
    {
        cJSON_AddStringToObject(o, "error", "no payload in response");
    }

    float volts = 0;

    (void)battery_monitor_voltage(&volts);

    if (have_payload && cJSON_IsString(expr) && expr->valuestring[0] != '\0')
    {
        double value = 0;

        if (expression_parser_eval(expr->valuestring, payload, n,
                                   (double)volts, &value) == ESP_OK)
        {
            cJSON_AddNumberToObject(o, "value", value);
        }
        else
        {
            cJSON_AddStringToObject(o, "error",
                                    "expression failed on payload");
        }
    }

    if (cJSON_IsArray(exprs))
    {
        /* one entry per expression, null where the reply gave nothing */
        cJSON *vals = cJSON_AddArrayToObject(o, "values");
        const cJSON *e = NULL;

        cJSON_ArrayForEach(e, exprs)
        {
            double value = 0;

            if (have_payload &&
                expression_parser_eval(e->valuestring, payload, n,
                                       (double)volts, &value) == ESP_OK)
            {
                cJSON_AddItemToArray(vals, cJSON_CreateNumber(value));
            }
            else
            {
                cJSON_AddItemToArray(vals, cJSON_CreateNull());
            }
        }
    }

    ap_core_job_release();
    cJSON_Delete(root);
    return send_json(req, o);
}

/* ---- DTC (TASK_dtc.md §8; HTTP_API.md §6e4b) ---------------------------- */

static esp_err_t dtc_get_handler(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *report = ap_dtc_report_json();

    if (o == NULL || report == NULL)
    {
        cJSON_Delete(o);
        cJSON_Delete(report);
        return send_error(req, "500 Internal Server Error", "oom");
    }

    /* database enrichment: {"desc":{"P0420":"Catalyst …"}} for every
     * report code with a hit (arrays stay untouched — no breakage) */
    {
        ap_dtc_report_t r;

        if (ap_dtc_report_get(&r) == ESP_OK && r.valid)
        {
            ap_dtc_db_desc_map(report, "desc", &r);
        }
    }

    cJSON_AddBoolToObject(o, "enabled", ap_dtc_enabled());
    cJSON_AddBoolToObject(o, "allow_clear", ap_dtc_clear_gate_open());
    cJSON_AddBoolToObject(o, "scanning", ap_dtc_busy());
    cJSON_AddItemToObject(o, "report", report);
    return send_json(req, o);
}

/* ---- DTC databases (TASK_dtc_db.md §4) ---------------------------------- */

/** ?key=value from the query string into @p out; false when absent. */
static bool query_param(httpd_req_t *req, const char *key, char *out,
                        size_t out_cap)
{
    char qs[160];

    out[0] = '\0';

    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) != ESP_OK)
    {
        return false;
    }

    return httpd_query_key_value(qs, key, out, out_cap) == ESP_OK;
}

static esp_err_t dtc_db_get_handler(httpd_req_t *req)
{
    cJSON *o = ap_dtc_db_list_json();

    if (o == NULL)
    {
        return send_error(req, "500 Internal Server Error", "oom");
    }

    return send_json(req, o);
}

static esp_err_t dtc_db_post_handler(httpd_req_t *req)
{
    char name[AP_DTC_DB_NAME_LEN];

    if (!query_param(req, "name", name, sizeof(name)))
    {
        return send_error(req, "400 Bad Request", "?name=<db> required");
    }

    size_t len = req->content_len;

    if (len == 0 || len > AP_DTC_DB_FILE_MAX)
    {
        return send_error(req, "400 Bad Request",
                          "body 1..1048576 bytes (raw database file)");
    }

    char *raw = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);

    if (raw == NULL)
    {
        return send_error(req, "500 Internal Server Error", "oom");
    }

    size_t got = 0;

    while (got < len)
    {
        int r = httpd_req_recv(req, raw + got, len - got);

        if (r <= 0)
        {
            heap_caps_free(raw);
            return send_error(req, "400 Bad Request", "body read failed");
        }

        got += (size_t)r;
    }

    int entries = 0;
    char fmt[12] = "";
    char err_text[96] = "";
    esp_err_t err = ap_dtc_db_store(name, raw, len, &entries, fmt,
                                    err_text, sizeof(err_text));

    heap_caps_free(raw);

    if (err == ESP_ERR_NO_MEM && err_text[0] == 'a')  /* slots full */
    {
        return send_error(req, "409 Conflict", err_text);
    }

    if (err != ESP_OK)
    {
        return send_error(req, "400 Bad Request",
                          err_text[0] ? err_text : "import failed");
    }

    cJSON *o = cJSON_CreateObject();

    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "name", name);
    cJSON_AddNumberToObject(o, "entries", entries);
    cJSON_AddStringToObject(o, "format", fmt);
    ESP_LOGI(TAG, "dtc db '%s' uploaded: %d entries (%s)", name, entries,
             fmt);
    return send_json(req, o);
}

static esp_err_t dtc_db_delete_handler(httpd_req_t *req)
{
    char name[AP_DTC_DB_NAME_LEN];

    if (!query_param(req, "name", name, sizeof(name)))
    {
        return send_error(req, "400 Bad Request", "?name=<db> required");
    }

    if (ap_dtc_db_delete(name) != ESP_OK)
    {
        return send_error(req, "404 Not Found", "no such database");
    }

    cJSON *o = cJSON_CreateObject();

    cJSON_AddBoolToObject(o, "ok", true);
    return send_json(req, o);
}

static esp_err_t dtc_db_search_handler(httpd_req_t *req)
{
    char q[64] = "", db[AP_DTC_DB_NAME_LEN] = "", num[12];
    int offset = 0, limit = 50;

    (void)query_param(req, "q", q, sizeof(q));
    (void)query_param(req, "db", db, sizeof(db));

    if (query_param(req, "offset", num, sizeof(num)))
    {
        offset = atoi(num);
    }

    if (query_param(req, "limit", num, sizeof(num)))
    {
        limit = atoi(num);
    }

    if (offset < 0 || limit < 1 || limit > 100)
    {
        return send_error(req, "400 Bad Request",
                          "offset>=0, 1<=limit<=100");
    }

    cJSON *o = ap_dtc_db_search_json(q, db, offset, limit);

    if (o == NULL)
    {
        return send_error(req, "500 Internal Server Error", "oom");
    }

    return send_json(req, o);
}

static esp_err_t dtc_lookup_handler(httpd_req_t *req)
{
    char codes[192];

    if (!query_param(req, "codes", codes, sizeof(codes)))
    {
        return send_error(req, "400 Bad Request",
                          "?codes=P0420,P0171 required");
    }

    cJSON *o = cJSON_CreateObject();
    char *p = codes;

    while (*p != '\0' && o != NULL)
    {
        char *sep = strchr(p, ',');

        if (sep != NULL)
        {
            *sep = '\0';
        }

        if (*p != '\0')
        {
            char desc[AP_DTC_DESC_MAX + 1];

            if (autopid_dtc_desc(p, desc, sizeof(desc)) == ESP_OK)
            {
                cJSON_AddStringToObject(o, p, desc);
            }
            else
            {
                cJSON_AddNullToObject(o, p);
            }
        }

        p = (sep != NULL) ? sep + 1 : p + strlen(p);
    }

    return send_json(req, o);
}

static esp_err_t dtc_scan_post_handler(httpd_req_t *req)
{
    esp_err_t err = ap_dtc_scan_start();

    if (err == ESP_ERR_NOT_ALLOWED)
    {
        return send_error(req, "403 Forbidden", "dtc_enabled is off");
    }

    if (err == ESP_ERR_INVALID_STATE)
    {
        return send_error(req, "409 Conflict", "another chip job runs");
    }

    if (err != ESP_OK)
    {
        return send_error(req, "500 Internal Server Error",
                          "scan start failed");
    }

    httpd_resp_set_status(req, "202 Accepted");

    cJSON *o = cJSON_CreateObject();

    cJSON_AddBoolToObject(o, "started", true);
    return send_json(req, o);
}

static esp_err_t dtc_clear_post_handler(httpd_req_t *req)
{
    char body[512];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);

    if (len <= 0)
    {
        return send_error(req, "400 Bad Request", "missing body");
    }

    body[len] = '\0';

    cJSON *root = cJSON_Parse(body);
    const cJSON *confirm = cJSON_GetObjectItemCaseSensitive(root,
                                                            "confirm");
    const cJSON *codes = cJSON_GetObjectItemCaseSensitive(root, "codes");
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");

    if (!cJSON_IsTrue(confirm))
    {
        cJSON_Delete(root);
        return send_error(req, "400 Bad Request",
                          "confirm:true required — mode 04 clears ALL "
                          "codes + readiness monitors");
    }

    bool cleared = false;
    uint8_t before = 0, after = 0;
    char err_text[48] = "";
    esp_err_t err = ap_dtc_clear(
        cJSON_IsString(codes) ? codes->valuestring : NULL,
        cJSON_IsString(mode) ? mode->valuestring : NULL, &cleared,
        &before, &after, err_text, sizeof(err_text));

    cJSON_Delete(root);

    if (err == ESP_ERR_NOT_ALLOWED)
    {
        return send_error(req, "403 Forbidden", err_text);
    }

    if (err == ESP_ERR_INVALID_ARG)
    {
        return send_error(req, "400 Bad Request", err_text);
    }

    if (err == ESP_ERR_INVALID_STATE)
    {
        return send_error(req, "409 Conflict", err_text);
    }

    cJSON *o = cJSON_CreateObject();

    cJSON_AddBoolToObject(o, "ok", err == ESP_OK);
    cJSON_AddBoolToObject(o, "cleared", cleared);
    cJSON_AddNumberToObject(o, "before", before);
    cJSON_AddNumberToObject(o, "after", after);

    if (err != ESP_OK && err_text[0] != '\0')
    {
        cJSON_AddStringToObject(o, "error", err_text);
    }

    return send_json(req, o);
}

/* ---- DBC files (TASK_dbc.md §5; HTTP_API.md §6e4c) ----------------------- */

static esp_err_t dbc_get_handler(httpd_req_t *req)
{
    cJSON *o = ap_dbc_list_json();

    return (o != NULL) ? send_json(req, o)
                       : send_error(req, "500 Internal Server Error",
                                    "oom");
}

static esp_err_t dbc_post_handler(httpd_req_t *req)
{
    char name[AP_DTC_DB_NAME_LEN];

    if (!query_param(req, "name", name, sizeof(name)))
    {
        return send_error(req, "400 Bad Request", "?name=<db> required");
    }

    size_t len = req->content_len;

    if (len == 0 || len > AP_DBC_FILE_MAX)
    {
        return send_error(req, "400 Bad Request",
                          "body 1..1048576 bytes (raw .dbc)");
    }

    char *raw = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);

    if (raw == NULL)
    {
        return send_error(req, "500 Internal Server Error", "oom");
    }

    size_t got = 0;

    while (got < len)
    {
        int r = httpd_req_recv(req, raw + got, len - got);

        if (r <= 0)
        {
            heap_caps_free(raw);
            return send_error(req, "400 Bad Request", "body read failed");
        }

        got += (size_t)r;
    }

    raw[len] = '\0';

    int n_msgs = 0, n_sigs = 0;
    char err_text[96] = "";
    esp_err_t err = ap_dbc_store(name, raw, len, &n_msgs, &n_sigs,
                                 err_text, sizeof(err_text));

    heap_caps_free(raw);

    if (err == ESP_ERR_NO_MEM && err_text[0] == 'a')  /* slots full */
    {
        return send_error(req, "409 Conflict", err_text);
    }

    if (err != ESP_OK)
    {
        return send_error(req, "400 Bad Request",
                          err_text[0] ? err_text : "import failed");
    }

    cJSON *o = cJSON_CreateObject();

    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "name", name);
    cJSON_AddNumberToObject(o, "messages", n_msgs);
    cJSON_AddNumberToObject(o, "signals", n_sigs);
    ESP_LOGI(TAG, "dbc '%s' uploaded: %d msgs / %d signals", name,
             n_msgs, n_sigs);
    return send_json(req, o);
}

static esp_err_t dbc_delete_handler(httpd_req_t *req)
{
    char name[AP_DTC_DB_NAME_LEN];

    if (!query_param(req, "name", name, sizeof(name)))
    {
        return send_error(req, "400 Bad Request", "?name=<db> required");
    }

    if (ap_dbc_delete(name) != ESP_OK)
    {
        return send_error(req, "404 Not Found", "no such DBC");
    }

    cJSON *o = cJSON_CreateObject();

    cJSON_AddBoolToObject(o, "ok", true);
    return send_json(req, o);
}

static esp_err_t dbc_signals_handler(httpd_req_t *req)
{
    char q[64] = "", db[AP_DTC_DB_NAME_LEN] = "", num[12];
    int offset = 0, limit = 50;

    (void)query_param(req, "q", q, sizeof(q));
    (void)query_param(req, "db", db, sizeof(db));

    if (query_param(req, "offset", num, sizeof(num)))
    {
        offset = atoi(num);
    }

    if (query_param(req, "limit", num, sizeof(num)))
    {
        limit = atoi(num);
    }

    if (offset < 0 || limit < 1 || limit > 100)
    {
        return send_error(req, "400 Bad Request",
                          "offset>=0, 1<=limit<=100");
    }

    cJSON *o = ap_dbc_signals_json(db, q, offset, limit);

    return (o != NULL) ? send_json(req, o)
                       : send_error(req, "500 Internal Server Error",
                                    "oom");
}

static esp_err_t dbc_add_handler(httpd_req_t *req)
{
    /* heap, not stack: 2 KB of locals on the httpd task is how the
     * 2026-07-08 heap-corruption hunt started */
    char *body = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);

    if (body == NULL)
    {
        return send_error(req, "500 Internal Server Error", "oom");
    }

    int len = httpd_req_recv(req, body, 2047);

    if (len <= 0)
    {
        heap_caps_free(body);
        return send_error(req, "400 Bad Request", "missing body");
    }

    body[len] = '\0';

    cJSON *root = cJSON_Parse(body);

    heap_caps_free(body);
    const cJSON *db = cJSON_GetObjectItemCaseSensitive(root, "db");
    const cJSON *signals = cJSON_GetObjectItemCaseSensitive(root,
                                                            "signals");
    const cJSON *group = cJSON_GetObjectItemCaseSensitive(root, "group");
    const cJSON *mon = cJSON_GetObjectItemCaseSensitive(root,
                                                        "monitor_ms");
    const cJSON *per = cJSON_GetObjectItemCaseSensitive(root,
                                                        "period_ms");

    if (!cJSON_IsString(db) || !cJSON_IsArray(signals) ||
        cJSON_GetArraySize(signals) == 0)
    {
        cJSON_Delete(root);
        return send_error(req, "400 Bad Request",
                          "need \"db\" + non-empty \"signals\" array");
    }

    cJSON *result = NULL;
    char err_text[128] = "";
    esp_err_t err = ap_dbc_add(
        db->valuestring, signals,
        cJSON_IsString(group) ? group->valuestring : NULL,
        cJSON_IsNumber(mon) ? mon->valueint : 0,
        cJSON_IsNumber(per) ? per->valueint : 0, &result, err_text,
        sizeof(err_text));

    cJSON_Delete(root);

    if (err == ESP_ERR_NOT_FOUND)
    {
        return send_error(req, "404 Not Found", err_text);
    }

    if (err == ESP_ERR_INVALID_ARG)
    {
        return send_error(req, "400 Bad Request", err_text);
    }

    if (err != ESP_OK)
    {
        return send_error(req, "500 Internal Server Error",
                          err_text[0] ? err_text : "add failed");
    }

    return send_json(req, result);
}

/* POST /api/autopid/group {"name":"...","enabled":bool[,"period_ms":N]}
 * Runtime (non-persisted) group toggle — the HTTP twin of the
 * `autopid.group` event action (API-first §1b: anything a rule can do,
 * a client can do). Used by the bench to claim OBD exclusivity. */
static esp_err_t group_post_handler(httpd_req_t *req)
{
    size_t len = req->content_len;

    if (len == 0 || len > 256)
    {
        return send_error(req, "400 Bad Request", "missing/oversized body");
    }

    char body[257];
    size_t got = 0;

    while (got < len)
    {
        int r = httpd_req_recv(req, body + got, len - got);

        if (r <= 0)
        {
            return send_error(req, "400 Bad Request", "body read failed");
        }

        got += (size_t)r;
    }

    body[len] = '\0';

    cJSON *root = cJSON_Parse(body);

    if (root == NULL)
    {
        return send_error(req, "400 Bad Request", "invalid json");
    }

    const cJSON *name = cJSON_GetObjectItem(root, "name");
    const cJSON *enabled = cJSON_GetObjectItem(root, "enabled");
    const cJSON *period = cJSON_GetObjectItem(root, "period_ms");

    if (!cJSON_IsString(name) || !cJSON_IsBool(enabled))
    {
        cJSON_Delete(root);
        return send_error(req, "400 Bad Request",
                          "need string 'name' + bool 'enabled'");
    }

    int32_t override_ms = -1; /* -1 = leave the period alone */

    if (cJSON_IsNumber(period) && period->valuedouble >= 0)
    {
        override_ms = (int32_t)period->valuedouble;
    }

    esp_err_t err = autopid_group_set(name->valuestring,
                                      cJSON_IsTrue(enabled), override_ms);
    cJSON_Delete(root);

    if (err == ESP_ERR_NOT_FOUND)
    {
        return send_error(req, "404 Not Found", "no such group");
    }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", err == ESP_OK);
    return send_json(req, o);
}

esp_err_t autopid_register_http(void)
{
    static const httpd_uri_t URIS[] =
    {
        { .uri = "/api/autopid", .method = HTTP_GET,
          .handler = autopid_get_handler },
        { .uri = "/api/autopid/data", .method = HTTP_GET,
          .handler = autopid_data_handler },
        { .uri = "/api/autopid/config", .method = HTTP_GET,
          .handler = config_get_handler },
        { .uri = "/api/autopid/config", .method = HTTP_PUT,
          .handler = config_put_handler },
        { .uri = "/api/autopid/std_scan", .method = HTTP_POST,
          .handler = std_scan_post_handler },
        { .uri = "/api/autopid/std_scan", .method = HTTP_GET,
          .handler = std_scan_get_handler },
        { .uri = "/api/autopid/std_scan/result", .method = HTTP_GET,
          .handler = std_scan_result_handler },
        { .uri = "/api/autopid/std_table", .method = HTTP_GET,
          .handler = std_table_handler },
        { .uri = "/api/autopid/test", .method = HTTP_POST,
          .handler = test_post_handler },
        { .uri = "/api/autopid/group", .method = HTTP_POST,
          .handler = group_post_handler },
        { .uri = "/api/autopid/dtc", .method = HTTP_GET,
          .handler = dtc_get_handler },
        { .uri = "/api/autopid/dtc/scan", .method = HTTP_POST,
          .handler = dtc_scan_post_handler },
        { .uri = "/api/autopid/dtc/clear", .method = HTTP_POST,
          .handler = dtc_clear_post_handler },
        { .uri = "/api/autopid/dtc/db", .method = HTTP_GET,
          .handler = dtc_db_get_handler },
        { .uri = "/api/autopid/dtc/db", .method = HTTP_POST,
          .handler = dtc_db_post_handler },
        { .uri = "/api/autopid/dtc/db", .method = HTTP_DELETE,
          .handler = dtc_db_delete_handler },
        { .uri = "/api/autopid/dtc/db/search", .method = HTTP_GET,
          .handler = dtc_db_search_handler },
        { .uri = "/api/autopid/dtc/lookup", .method = HTTP_GET,
          .handler = dtc_lookup_handler },
        { .uri = "/api/autopid/dbc", .method = HTTP_GET,
          .handler = dbc_get_handler },
        { .uri = "/api/autopid/dbc", .method = HTTP_POST,
          .handler = dbc_post_handler },
        { .uri = "/api/autopid/dbc", .method = HTTP_DELETE,
          .handler = dbc_delete_handler },
        { .uri = "/api/autopid/dbc/signals", .method = HTTP_GET,
          .handler = dbc_signals_handler },
        { .uri = "/api/autopid/dbc/add", .method = HTTP_POST,
          .handler = dbc_add_handler },
    };

    esp_err_t err = http_server_manager_register_handlers(
        URIS, sizeof(URIS) / sizeof(URIS[0]));

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "/api/autopid routes registered");
    }

    return err;
}
