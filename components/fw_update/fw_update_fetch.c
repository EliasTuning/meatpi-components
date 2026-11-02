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
 * @file fw_update_fetch.c
 * @brief The fetch path: the manifest (with its ETag), the release notes
 *        and the image, all through http_client_manager's streamed GET on
 *        the component's PSRAM-stack task. The image flows into
 *        ota_manager's pipe, whose internal-stack writer owns the flash;
 *        size, sha256 and product are checked before anything is activated.
 */
#include "fw_update_private.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "http_client_manager.h"
#include "ota_manager.h"

static const char *TAG = FWU_TAG;

#define FWU_MANIFEST_TIMEOUT_MS 15000
#define FWU_IMAGE_TIMEOUT_MS    20000   /* per read                           */
#define FWU_IMAGE_ATTEMPTS      3
#define FWU_PIPE_WAIT_MS        20000
#define FWU_PIPE_END_WAIT_MS    90000   /* esp_ota_end verifies the image     */

static const uint32_t RETRY_WAIT_S[FWU_IMAGE_ATTEMPTS] = { 5, 20, 60 };

/* ---- a bounded document into PSRAM -------------------------------------------- */

typedef struct
{
    char  *buf;
    size_t cap;
    size_t len;
    bool   overflow;
} doc_sink_t;

static bool doc_headers(int status, int64_t content_length, void *ctx)
{
    doc_sink_t *d = ctx;

    (void)status;
    return content_length <= (int64_t)d->cap; /* bigger: not even started */
}

static esp_err_t doc_data(const uint8_t *data, size_t len, void *ctx)
{
    doc_sink_t *d = ctx;

    if (d->len + len > d->cap)
    {
        d->overflow = true;
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(d->buf + d->len, data, len);
    d->len += len;
    return ESP_OK;
}

/** GET @p url into a PSRAM buffer of @p cap bytes. */
static esp_err_t fetch_doc(const fwu_config_t *cfg, const char *url,
                           const char *etag, size_t cap, int attempts,
                           fwu_fetch_result_t *out)
{
    char inm[80];
    const char *headers[2] = { "Accept: application/json", inm };
    size_t n_headers = 1;

    memset(out, 0, sizeof(*out));
    out->status = -1;

    if (etag != NULL && etag[0] != '\0')
    {
        snprintf(inm, sizeof(inm), "If-None-Match: %s", etag);
        n_headers = 2;
    }

    doc_sink_t sink = { .cap = cap };

    sink.buf = heap_caps_malloc(cap + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (sink.buf == NULL)
    {
        snprintf(out->code, sizeof(out->code), "no_memory");
        return ESP_ERR_NO_MEM;
    }

    http_client_request_t req =
    {
        .url = url,
        .extra_headers = headers,
        .extra_header_count = n_headers,
        .cert_set = (cfg->cert_set[0] != '\0') ? cfg->cert_set : NULL,
        .timeout_ms = FWU_MANIFEST_TIMEOUT_MS,
    };
    http_client_stream_cb_t cb = { .on_headers = doc_headers, .on_data = doc_data };
    http_client_stream_info_t info;
    esp_err_t err = ESP_FAIL;

    for (int i = 0; i < attempts; i++)
    {
        sink.len = 0;
        sink.overflow = false;
        err = http_client_manager_stream(&req, &cb, &sink, &info);

        if (err == ESP_OK || err == ESP_ERR_INVALID_RESPONSE ||
            err == ESP_ERR_INVALID_SIZE)
        {
            break; /* an answer, or a document too big: no retry helps */
        }

        if (i + 1 < attempts)
        {
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }

    out->status = info.status_code;
    strlcpy(out->etag, info.etag, sizeof(out->etag));

    if (err == ESP_ERR_INVALID_RESPONSE || err == ESP_ERR_INVALID_SIZE ||
        sink.overflow)
    {
        heap_caps_free(sink.buf);
        snprintf(out->code, sizeof(out->code), "too_large");
        return ESP_ERR_INVALID_SIZE;
    }

    if (err != ESP_OK)
    {
        heap_caps_free(sink.buf);
        snprintf(out->code, sizeof(out->code), "fetch_failed");
        return err;
    }

    if (info.status_code == 304)
    {
        heap_caps_free(sink.buf);
        out->etag_hit = true;
        return ESP_OK;
    }

    if (info.status_code < 200 || info.status_code >= 300)
    {
        heap_caps_free(sink.buf);
        snprintf(out->code, sizeof(out->code),
                 (info.status_code == 404) ? "not_found" : "http_%d",
                 info.status_code);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (!info.complete)
    {
        heap_caps_free(sink.buf);
        snprintf(out->code, sizeof(out->code), "truncated");
        return ESP_ERR_INVALID_SIZE;
    }

    sink.buf[sink.len] = '\0';
    out->body = sink.buf;
    out->len = sink.len;
    return ESP_OK;
}

esp_err_t fwu_fetch_manifest(const fwu_config_t *cfg, const char *device_type,
                             const char *etag, fwu_fetch_result_t *out)
{
    char url[FWU_URL_LEN + 64];
    char path[80];

    snprintf(path, sizeof(path), "%s/firmware/manifest.json", device_type);

    if (!fwu_compose_url(cfg->base_url, path, url, sizeof(url)))
    {
        memset(out, 0, sizeof(*out));
        snprintf(out->code, sizeof(out->code), "bad_url");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = fetch_doc(cfg, url, etag, FWU_MANIFEST_CAP, 2, out);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "manifest %s: %s (%s, status %d)", url, out->code,
                 esp_err_to_name(err), out->status);
    }

    return err;
}

/* ---- the image ---------------------------------------------------------------- */

typedef struct
{
    const fwu_release_t *rel;
    fwu_progress_fn progress;
    volatile bool *abort;
    size_t received;
    bool size_mismatch;
    esp_err_t pipe_err;
    int64_t last_progress_us;
} image_sink_t;

static bool image_headers(int status, int64_t content_length, void *ctx)
{
    image_sink_t *s = ctx;

    if (status < 200 || status >= 300)
    {
        return false;
    }

    if (content_length > 0 && (uint64_t)content_length != s->rel->size)
    {
        s->size_mismatch = true; /* the manifest and the server disagree */
        return false;
    }

    return true;
}

static esp_err_t image_data(const uint8_t *data, size_t len, void *ctx)
{
    image_sink_t *s = ctx;

    if (s->abort != NULL && *s->abort)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (s->received + len > s->rel->size)
    {
        s->size_mismatch = true;
        return ESP_ERR_INVALID_SIZE;
    }

    s->pipe_err = ota_manager_pipe_write(data, len, FWU_PIPE_WAIT_MS);

    if (s->pipe_err != ESP_OK)
    {
        return s->pipe_err;
    }

    s->received += len;

    if (s->progress != NULL &&
        esp_timer_get_time() - s->last_progress_us > 250000)
    {
        s->last_progress_us = esp_timer_get_time();
        s->progress(s->received, s->rel->size);
    }

    return ESP_OK;
}

static bool hex_to_sha(const char *hex, uint8_t out[32])
{
    for (int i = 0; i < 32; i++)
    {
        unsigned v;

        if (sscanf(hex + 2 * i, "%2x", &v) != 1)
        {
            return false;
        }

        out[i] = (uint8_t)v;
    }

    return true;
}

/** The session's refusal text as a code. */
static void code_from_session(char *code, char *detail)
{
    ota_manager_status_t st;

    (void)ota_manager_status(&st);
    snprintf(detail, FW_UPDATE_DETAIL_LEN, "%s", st.error);

    if (strcmp(st.error, "sha256 mismatch") == 0)
    {
        snprintf(code, FW_UPDATE_CODE_LEN, "sha256_mismatch");
    }
    else if (strncmp(st.error, "not a ", 6) == 0)
    {
        snprintf(code, FW_UPDATE_CODE_LEN, "wrong_product");
    }
    else if (strncmp(st.error, "not an", 6) == 0 ||
             strncmp(st.error, "no application", 14) == 0 ||
             strncmp(st.error, "an image for", 12) == 0 ||
             strncmp(st.error, "short image", 11) == 0)
    {
        snprintf(code, FW_UPDATE_CODE_LEN, "wrong_image");
    }
    else if (strcmp(st.error, "image validation failed") == 0)
    {
        snprintf(code, FW_UPDATE_CODE_LEN, "image_invalid");
    }
    else
    {
        snprintf(code, FW_UPDATE_CODE_LEN, "flash_failed");
    }
}

/** One attempt: a fresh pipe session, the stream, the end. */
static esp_err_t image_attempt(const fwu_release_t *r, const fwu_config_t *cfg,
                               fwu_progress_fn progress, volatile bool *abort,
                               const uint8_t sha[32], char *code, char *detail,
                               bool *retry)
{
    image_sink_t sink =
    {
        .rel = r, .progress = progress, .abort = abort,
    };
    http_client_request_t req =
    {
        .url = r->url,
        .cert_set = (cfg->cert_set[0] != '\0') ? cfg->cert_set : NULL,
        .timeout_ms = FWU_IMAGE_TIMEOUT_MS,
    };
    http_client_stream_cb_t cb = { .on_headers = image_headers,
                                   .on_data = image_data };
    http_client_stream_info_t info;

    *retry = false;
    code[0] = '\0';
    detail[0] = '\0';

    esp_err_t err = ota_manager_pipe_begin(r->size, sha);

    if (err == ESP_ERR_INVALID_STATE)
    {
        snprintf(code, FW_UPDATE_CODE_LEN, "busy_upload");
        snprintf(detail, FW_UPDATE_DETAIL_LEN, "another update session is open");
        return err;
    }

    if (err != ESP_OK)
    {
        code_from_session(code, detail);
        return err;
    }

    err = http_client_manager_stream(&req, &cb, &sink, &info);

    if (abort != NULL && *abort)
    {
        (void)ota_manager_pipe_abort();
        snprintf(code, FW_UPDATE_CODE_LEN, "aborted");
        return ESP_ERR_INVALID_STATE;
    }

    if (sink.size_mismatch)
    {
        (void)ota_manager_pipe_abort();
        snprintf(code, FW_UPDATE_CODE_LEN, "size_mismatch");
        snprintf(detail, FW_UPDATE_DETAIL_LEN,
                 "the server offers %lld bytes, the manifest says %lu",
                 (long long)info.content_length, (unsigned long)r->size);
        return ESP_ERR_INVALID_SIZE;
    }

    if (sink.pipe_err != ESP_OK)
    {
        /* the refusal text first: the abort resets the session (bench
           2026-10-19: a foreign image read as flash_failed) */
        code_from_session(code, detail);
        (void)ota_manager_pipe_abort();
        return sink.pipe_err;
    }

    if (err == ESP_ERR_INVALID_RESPONSE || (err == ESP_OK && info.status_code != 200))
    {
        (void)ota_manager_pipe_abort();
        snprintf(code, FW_UPDATE_CODE_LEN,
                 (info.status_code == 404) ? "image_not_found" : "image_fetch_failed");
        snprintf(detail, FW_UPDATE_DETAIL_LEN, "HTTP %d", info.status_code);
        *retry = info.status_code >= 500;
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (err != ESP_OK || !info.complete || sink.received != r->size)
    {
        (void)ota_manager_pipe_abort();
        snprintf(code, FW_UPDATE_CODE_LEN, "image_fetch_failed");
        snprintf(detail, FW_UPDATE_DETAIL_LEN, "%lu of %lu bytes (%s)",
                 (unsigned long)sink.received, (unsigned long)r->size,
                 (err != ESP_OK) ? esp_err_to_name(err) : "incomplete");
        *retry = true;
        return (err != ESP_OK) ? err : ESP_ERR_INVALID_SIZE;
    }

    if (progress != NULL)
    {
        progress(sink.received, r->size);
    }

    err = ota_manager_pipe_end(FWU_PIPE_END_WAIT_MS);

    if (err != ESP_OK)
    {
        code_from_session(code, detail);

        if (err == ESP_ERR_TIMEOUT)
        {
            snprintf(code, FW_UPDATE_CODE_LEN, "flash_failed");
            snprintf(detail, FW_UPDATE_DETAIL_LEN, "the writer did not finish");
        }

        return err;
    }

    return ESP_OK;
}

esp_err_t fwu_fetch_image(const fwu_release_t *r, const fwu_config_t *cfg,
                          fwu_progress_fn progress, volatile bool *abort,
                          uint8_t *attempt, char *code, char *detail)
{
    uint8_t sha[32];
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    esp_err_t err = ESP_FAIL;

    code[0] = '\0';
    detail[0] = '\0';

    if (next == NULL)
    {
        snprintf(code, FW_UPDATE_CODE_LEN, "no_slot");
        return ESP_ERR_NOT_FOUND;
    }

    if (r->size > next->size)
    {
        snprintf(code, FW_UPDATE_CODE_LEN, "image_too_large");
        snprintf(detail, FW_UPDATE_DETAIL_LEN, "%lu bytes for a %lu byte slot",
                 (unsigned long)r->size, (unsigned long)next->size);
        return ESP_ERR_INVALID_SIZE;
    }

    if (!hex_to_sha(r->sha256, sha))
    {
        snprintf(code, FW_UPDATE_CODE_LEN, "manifest_invalid");
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 0; i < FWU_IMAGE_ATTEMPTS; i++)
    {
        bool retry = false;

        *attempt = (uint8_t)(i + 1);
        err = image_attempt(r, cfg, progress, abort, sha, code, detail, &retry);

        if (err == ESP_OK)
        {
            return ESP_OK;
        }

        ESP_LOGW(TAG, "image %s attempt %d: %s (%s)", r->version_str, i + 1,
                 code, detail);

        if (!retry || i + 1 >= FWU_IMAGE_ATTEMPTS)
        {
            break;
        }

        /* the wait is abortable in one-second steps */
        for (uint32_t s = 0; s < RETRY_WAIT_S[i]; s++)
        {
            if (abort != NULL && *abort)
            {
                snprintf(code, FW_UPDATE_CODE_LEN, "aborted");
                return ESP_ERR_INVALID_STATE;
            }

            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    return err;
}
