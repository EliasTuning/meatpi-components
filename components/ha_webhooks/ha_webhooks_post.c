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
 * @file ha_webhooks_post.c
 * @brief The poster task (PSRAM stack): every `interval_s`, while enabled
 *        and the network is up, takes the body from hw_payload_build()
 *        (ha_webhooks_payload.c), gzips it on request and POSTs it to the
 *        HA webhook URL(s) with failover. HTTP 403 from HA (identity
 *        rejection) pauses posting after HW_REJECT_LIMIT consecutive
 *        cycles until the next registration.
 *
 * PSRAM stack is safe because the payload is built from RAM only and the
 * outbound HTTP goes through http_client_manager (its own task). Stats
 * updates are cache-only (no flash): §2 corollary.
 *
 * The poster does NOT depend on autopid being enabled (2026-09-08): a
 * status-only push is a valid contract push and the HA integration's
 * fixed sensors (battery, wifi mode, uptime ...) need no vehicle data.
 */
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "miniz.h" /* the ROM deflate compressor (tdefl_* are ROM symbols) */

#include "dev_status_manager.h"
#include "http_client_manager.h"
#include "wifi_manager.h" /* "active on" (2026-11-08) */

#include "ha_webhooks_private.h"

static const char *TAG = "ha_webhooks";

/* 8192 words = 32 KB (PSRAM, cheap): 24 KB overflowed once gzip landed,
 * the ROM tdefl deflate keeps its Huffman/blocking locals on the CALLER's
 * stack (several KB under an already-deep poster frame). Bench-verify
 * stack_hw via /api/status/tasks after any change here. */
#define HW_STACK_WORDS 8192
#define HW_HTTP_TIMEOUT_MS 6000

/* consecutive HTTP-403 cycles (HA identity rejection) before pushes
 * pause: the contract's "stop; needs user attention" device action.
 * Re-registration (POST /api/webhook) or reboot resumes. */
#define HW_REJECT_LIMIT 3

static TaskHandle_t     s_task;
static StaticTask_t     s_tcb;
static StackType_t      s_stack[HW_STACK_WORDS] EXT_RAM_BSS_ATTR;
static volatile bool    s_run;

/* consecutive identity-rejected (403) cycles; >= HW_REJECT_LIMIT pauses
   the poster until the next registration (resync) or reboot */
static volatile uint32_t s_reject_streak;

void hw_poster_resync(void)
{
    hw_payload_resync();  /* next build = full snapshot for the new HA */
    s_reject_streak = 0;  /* a (re-)registration ends the rejected pause */
}

/* ---- gzip (ask #9 item 4: LTE data saver, JSON compresses 5-10x) ---------- */

/**
 * gzip @p in via the ROM miniz compressor (zero flash cost; the ~166 KB
 * compressor state is a lazy one-time PSRAM alloc, reused: the poster
 * is the only caller). Returns a PSRAM buffer (caller frees) or NULL:
 * the caller then falls back to the uncompressed body.
 */
static uint8_t *hw_gzip(const char *in, size_t in_len, size_t *out_len)
{
    static tdefl_compressor *s_comp;

    if (s_comp == NULL)
    {
        s_comp = heap_caps_malloc(sizeof(tdefl_compressor),
                                  MALLOC_CAP_SPIRAM);
    }

    if (s_comp == NULL || in_len == 0)
    {
        return NULL;
    }

    /* 10 B gzip header + raw deflate (worst case ~in_len + 5 B / 32 KB
       block + slack) + 8 B trailer (CRC32 + ISIZE) */
    size_t deflate_cap = in_len + in_len / 16 + 64;
    uint8_t *out = heap_caps_malloc(10 + deflate_cap + 8,
                                    MALLOC_CAP_SPIRAM);

    if (out == NULL)
    {
        return NULL;
    }

    static const uint8_t GZ_HDR[10] =
    { 0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff }; /* deflate, no name, OS=unknown */

    memcpy(out, GZ_HDR, sizeof(GZ_HDR));

    /* no TDEFL_WRITE_ZLIB_HEADER -> raw deflate (what gzip framing wants) */
    if (tdefl_init(s_comp, NULL, NULL, TDEFL_DEFAULT_MAX_PROBES)
            != TDEFL_STATUS_OKAY)
    {
        free(out);
        return NULL;
    }

    size_t in_sz = in_len;
    size_t out_sz = deflate_cap;
    tdefl_status st = tdefl_compress(s_comp, in, &in_sz, out + 10,
                                     &out_sz, TDEFL_FINISH);

    if (st != TDEFL_STATUS_DONE || in_sz != in_len)
    {
        free(out);
        return NULL;
    }

    uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT,
                                      (const unsigned char *)in, in_len);
    uint32_t isize = (uint32_t)in_len;
    uint8_t *tr = out + 10 + out_sz;

    for (int i = 0; i < 4; i++)
    {
        tr[i] = (uint8_t)(crc >> (8 * i));
        tr[4 + i] = (uint8_t)(isize >> (8 * i));
    }

    *out_len = 10 + out_sz + 8;
    return out;
}

/* ---- POST with failover --------------------------------------------------- */

static bool url_is_raw_ip_https(const char *url)
{
    if (strncasecmp(url, "https://", 8) != 0)
    {
        return false;
    }

    const char *h = url + 8;

    for (; *h && *h != '/' && *h != ':'; h++)
    {
        if ((*h < '0' || *h > '9') && *h != '.')
        {
            return false;
        }
    }

    return true;
}

/** Try each configured URL in order; first 2xx wins. Fills @p err;
 *  sets @p rejected on an HA identity rejection (HTTP 403). */
static bool post_failover(const hw_config_t *c, const void *body,
                          size_t body_len, bool gzipped,
                          char err[HW_ERR_LEN], char okstatus[HW_STATUS_LEN],
                          bool *rejected)
{
    static const char *const GZIP_HEADERS[] = { "Content-Encoding: gzip" };
    const char *urls[2] = { c->url, c->url2 };

    err[0] = '\0';
    *rejected = false;

    for (int i = 0; i < 2; i++)
    {
        if (urls[i][0] == '\0')
        {
            continue;
        }

        http_client_request_t req = {
            .url = urls[i],
            .method = HTTP_CLIENT_POST,
            .body = body,
            .body_len = body_len,
            .content_type = "application/json",
            .extra_headers = gzipped ? GZIP_HEADERS : NULL,
            .extra_header_count = gzipped ? 1 : 0,
            .cert_set = c->cert_set[0] != '\0' ? c->cert_set : NULL,
            .skip_common_name = url_is_raw_ip_https(urls[i]),
            .timeout_ms = HW_HTTP_TIMEOUT_MS,
        };
        http_client_response_t resp = { 0 };
        esp_err_t r = http_client_manager_request(&req, &resp);
        bool ok = (r == ESP_OK && resp.status_code >= 200 &&
                   resp.status_code < 300);

        if (ok)
        {
            snprintf(okstatus, HW_STATUS_LEN, "ok");
            http_client_manager_free(&resp);
            return true;
        }

        if (r == ESP_OK && resp.status_code == 403)
        {
            /* the push contract: 403 = our device_id does not match the
               configured device, the second URL is the SAME HA, so
               don't failover under a rejected identity */
            snprintf(err, HW_ERR_LEN, "http=403 identity rejected");
            *rejected = true;
            http_client_manager_free(&resp);
            return false;
        }

        if (r != ESP_OK)
        {
            snprintf(err, HW_ERR_LEN, "esp_err=%s", esp_err_to_name(r));
        }
        else
        {
            snprintf(err, HW_ERR_LEN, "http=%d", resp.status_code);
        }

        http_client_manager_free(&resp);
    }

    return false;
}

/* ---- the task ------------------------------------------------------------- */

static void poster_task(void *arg)
{
    (void)arg;

    uint32_t last_attempt = 0;

    vTaskDelay(pdMS_TO_TICKS(5000)); /* let the system settle */

    while (s_run)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));

        hw_config_t c;

        if (!hw_config_get(&c) || !c.enabled || c.url[0] == '\0')
        {
            continue;
        }

        if (!dev_status_manager_any_set(DEV_STATUS_NETWORK_CONNECTED_MASK))
        {
            dev_status_manager_wait_any(DEV_STATUS_NETWORK_CONNECTED_MASK,
                                        pdMS_TO_TICKS(3000));
            continue;
        }

        /* "active on these networks" (2026-11-08): the pushes wait while
           the station is not on one of the named WiFi networks (or on no
           WiFi at all); one I line per transition, nothing counted as a
           failure, the interval not consumed */
        static bool s_net_paused;

        if (!wifi_manager_sta_on_network(c.networks))
        {
            if (!s_net_paused)
            {
                ESP_LOGI(TAG, "not on a selected network: pushes wait");
                s_net_paused = true;
            }

            continue;
        }

        if (s_net_paused)
        {
            ESP_LOGI(TAG, "on a selected network again: pushes resume");
            s_net_paused = false;
        }

        /* NO autopid gate here: status (+config) goes out whenever the
           link is enabled and the network is up; autopid_data is simply
           omitted while autopid is off or has no values (contract: a
           status-only push is valid). The gate that used to sit here
           silenced every FRESH device - autopid ships disabled, HA's
           registration returned 201 and not one push ever left the
           device (bench 2026-09-08, ha_webhook_gate_bench.py). */

        if (s_reject_streak >= HW_REJECT_LIMIT)
        {
            continue; /* identity rejected: paused until re-registration */
        }

        uint32_t now = xTaskGetTickCount() / configTICK_RATE_HZ;

        if ((now - last_attempt) < c.interval_s)
        {
            continue;
        }

        /* schedule on ATTEMPT, not success: a down endpoint must not be
           retried every second (legacy lesson) */
        last_attempt = now;

        char *body = hw_payload_build(c.data_mode_full);

        if (body == NULL)
        {
            continue;
        }

        /* gzip (optional, either data mode): fall back to plain on any
           compression failure, delivery beats savings */
        const void *send_buf = body;
        size_t send_len = strlen(body);
        uint8_t *gz = NULL;
        bool gzipped = false;

        if (c.gzip)
        {
            size_t gz_len = 0;

            gz = hw_gzip(body, send_len, &gz_len);

            if (gz != NULL)
            {
                send_buf = gz;
                send_len = gz_len;
                gzipped = true;
            }
            else
            {
                ESP_LOGW(TAG, "gzip failed; posting uncompressed");
            }
        }

        char err[HW_ERR_LEN] = "";
        char okstatus[HW_STATUS_LEN] = "";
        bool rejected = false;
        bool ok = post_failover(&c, send_buf, send_len, gzipped, err,
                                okstatus, &rejected);

        free(gz);
        free(body);

        /* update stats (cache only: no flash) */
        hw_stats_t st;

        hw_stats_get(&st);

        if (ok)
        {
            s_reject_streak = 0;
            st.success_count++;
            st.retries = 0;
            strlcpy(st.status, "ok", sizeof(st.status));
            hw_format_utc(st.last_post);
            st.last_error[0] = '\0';
            st.last_error_time[0] = '\0';
        }
        else
        {
            s_reject_streak = rejected ? s_reject_streak + 1 : 0;
            st.fail_count++;
            st.retries++;

            if (s_reject_streak >= HW_REJECT_LIMIT)
            {
                strlcpy(st.status, "rejected", sizeof(st.status));
                ESP_LOGE(TAG,
                         "HA rejected our device identity (HTTP 403) %u "
                         "times, pausing pushes until re-registration "
                         "(was this device replaced?)",
                         (unsigned)s_reject_streak);
            }
            else
            {
                strlcpy(st.status, "failed", sizeof(st.status));
            }

            strlcpy(st.last_error, err, sizeof(st.last_error));
            hw_format_utc(st.last_error_time);
            ESP_LOGW(TAG, "webhook POST failed: %s", err);
        }

        hw_stats_set(&st);
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t hw_poster_start(void)
{
    if (s_task != NULL)
    {
        return ESP_OK;
    }

    s_run = true;
    s_task = xTaskCreateStatic(poster_task, "ha_webhook",
                               HW_STACK_WORDS, NULL, 4, s_stack, &s_tcb);
    return s_task != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

void hw_poster_stop(void)
{
    s_run = false; /* the task exits on its next lap */
}
