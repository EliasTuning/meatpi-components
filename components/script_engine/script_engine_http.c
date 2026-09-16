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
 * @file script_engine_http.c
 * @brief The /api/scripts surface:
 *          GET  /api/scripts             list stored scripts (+ enabled/busy)
 *          POST /api/scripts/run         {src|name} -> {ok, output}
 *          POST /api/scripts/check       {src} -> {ok[, error]} (compile only)
 *          POST /api/scripts/stop        kill switch
 *          GET  /api/scripts/reference   the scripting reference (doc.c)
 *          GET  /api/scripts/examples    the example gallery (no sources);
 *                                        ?id=<id> -> that example's source
 *        File CRUD rides the generic /api/fs surface
 *        (upload?path=/data/scripts/x.be, download, DELETE file).
 */
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "berry.h"
#include "filesystem.h"
#include "http_server_manager.h"

#include "script_engine.h"
#include "script_engine_doc.h"
#include "script_engine_private.h"

static const char *TAG = "script_engine";

/* ---- helpers --------------------------------------------------------------- */

static esp_err_t send_json(httpd_req_t *req, cJSON *o)
{
    char *s = (o != NULL) ? cJSON_PrintUnformatted(o) : NULL;

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

/* Read a JSON body of at most SE_SRC_MAX bytes; NULL (error already sent)
 * when missing, oversized, truncated or not JSON. */
static cJSON *read_body(httpd_req_t *req)
{
    size_t len = req->content_len;

    if (len == 0 || len > SE_SRC_MAX)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing/big body");
        return NULL;
    }

    char *body = malloc(len + 1);

    if (body == NULL)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return NULL;
    }

    size_t got = 0;

    while (got < len)
    {
        int r = httpd_req_recv(req, body + got, len - got);

        if (r <= 0)
        {
            free(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv");
            return NULL;
        }

        got += (size_t)r;
    }

    body[len] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);

    if (root == NULL)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }

    return root;
}

/* ---- run / check ------------------------------------------------------------- */

static esp_err_t run_handler(httpd_req_t *req)
{
    cJSON *root = read_body(req);

    if (root == NULL)
    {
        return ESP_FAIL;
    }

    const cJSON *src = cJSON_GetObjectItemCaseSensitive(root, "src");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    bool by_name = cJSON_IsString(name) && name->valuestring[0] != '\0';

    if (!by_name &&
        (!cJSON_IsString(src) || src->valuestring[0] == '\0'))
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "need \"src\" or \"name\"");
        return ESP_FAIL;
    }

    char *out = heap_caps_malloc(SE_OUT_MAX,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (out == NULL)
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    out[0] = '\0';
    esp_err_t err = by_name
        ? script_engine_run_file(name->valuestring, out, SE_OUT_MAX)
        : script_engine_run(src->valuestring, out, SE_OUT_MAX);
    cJSON_Delete(root);

    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_ARG ||
        err == ESP_ERR_INVALID_SIZE)
    {
        free(out);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND,
                            "no such script (or bad name/size)");
        return ESP_FAIL;
    }

    cJSON *o = cJSON_CreateObject();

    if (err == ESP_ERR_INVALID_STATE)
    {
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddStringToObject(o, "error",
                                "busy or script_engine disabled");
        httpd_resp_set_status(req, "409 Conflict");
    }
    else
    {
        cJSON_AddBoolToObject(o, "ok", err == ESP_OK);
        cJSON_AddStringToObject(o, "output", out);
    }

    free(out);
    return send_json(req, o);
}

/* POST /api/scripts/check {"src"} — compile only, nothing runs: the editor's
 * syntax check. {"ok":true} or {"ok":false,"error":"syntax_error: string:3: …"} */
static esp_err_t check_handler(httpd_req_t *req)
{
    cJSON *root = read_body(req);

    if (root == NULL)
    {
        return ESP_FAIL;
    }

    const cJSON *src = cJSON_GetObjectItemCaseSensitive(root, "src");

    if (!cJSON_IsString(src) || src->valuestring[0] == '\0')
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need \"src\"");
        return ESP_FAIL;
    }

    char *out = heap_caps_malloc(SE_OUT_MAX,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (out == NULL)
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    out[0] = '\0';
    esp_err_t err = script_engine_check(src->valuestring, out, SE_OUT_MAX);
    cJSON_Delete(root);

    cJSON *o = cJSON_CreateObject();

    if (err == ESP_ERR_INVALID_STATE)
    {
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddStringToObject(o, "error",
                                "busy or script_engine disabled");
        httpd_resp_set_status(req, "409 Conflict");
    }
    else
    {
        cJSON_AddBoolToObject(o, "ok", err == ESP_OK);

        if (err != ESP_OK)
        {
            cJSON_AddStringToObject(o, "error", out);
        }
    }

    free(out);
    return send_json(req, o);
}

/* ---- list ---------------------------------------------------------------------- */

/* GET /api/scripts — the .be files in /data/scripts (CRUD via /api/fs). */
static esp_err_t list_cb(const char *name, bool is_dir, size_t size,
                         void *ctx)
{
    if (is_dir)
    {
        return ESP_OK;
    }

    size_t n = strlen(name);

    if (n < 4 || strcmp(name + n - 3, ".be") != 0)
    {
        return ESP_OK;
    }

    cJSON *e = cJSON_CreateObject();

    cJSON_AddStringToObject(e, "name", name);
    cJSON_AddNumberToObject(e, "size", (double)size);
    cJSON_AddItemToArray((cJSON *)ctx, e);
    return ESP_OK;
}

static esp_err_t list_handler(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(o, "scripts");

    (void)filesystem_list(SE_SCRIPTS_DIR, list_cb, arr); /* absent dir = empty */
    cJSON_AddStringToObject(o, "dir", SE_SCRIPTS_DIR);
    cJSON_AddBoolToObject(o, "busy", script_engine_busy());
    cJSON_AddBoolToObject(o, "enabled", se_settings_enabled());
    cJSON_AddNumberToObject(o, "max_runtime_ms", se_settings_max_runtime_ms());
    return send_json(req, o);
}

static esp_err_t stop_handler(httpd_req_t *req)
{
    script_engine_kill();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

/* ---- reference + examples (the engine describes itself) ------------------------ */

static esp_err_t reference_handler(httpd_req_t *req)
{
    const se_doc_ctx_t ctx =
    {
        .language       = "Berry " BERRY_VERSION,
        .enabled        = se_settings_enabled(),
        .allow_reflash  = se_settings_allow_reflash(),
        .exclusive      = se_settings_exclusive(),
        .max_runtime_ms = se_settings_max_runtime_ms(),
    };

    return send_json(req, se_doc_reference(&ctx));
}

/* GET /api/scripts/examples -> the gallery list (JSON);
 * GET /api/scripts/examples?id=<id> -> that example's Berry source
 * (text/plain), 404 unknown. One handler: the server's URI table is a
 * fixed budget shared by every component. */
static esp_err_t examples_handler(httpd_req_t *req)
{
    char query[96];
    char id[48];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "id", id, sizeof(id)) != ESP_OK)
    {
        return send_json(req, se_doc_examples());
    }

    const se_example_t *x = se_example_find(id);

    if (x == NULL)
    {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such example");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, x->src, HTTPD_RESP_USE_STRLEN);
}

esp_err_t script_engine_register_http(void)
{
    static const httpd_uri_t URIS[] =
    {
        { .uri = "/api/scripts", .method = HTTP_GET,
          .handler = list_handler },
        { .uri = "/api/scripts/run", .method = HTTP_POST,
          .handler = run_handler },
        { .uri = "/api/scripts/check", .method = HTTP_POST,
          .handler = check_handler },
        { .uri = "/api/scripts/stop", .method = HTTP_POST,
          .handler = stop_handler },
        { .uri = "/api/scripts/reference", .method = HTTP_GET,
          .handler = reference_handler },
        { .uri = "/api/scripts/examples", .method = HTTP_GET,
          .handler = examples_handler },
    };

    esp_err_t err = http_server_manager_register_handlers(
        URIS, sizeof(URIS) / sizeof(URIS[0]));

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "/api/scripts routes registered");
    }

    return err;
}
