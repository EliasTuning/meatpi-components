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
 * @file data_destinations.c
 * @brief Lifecycle, the applied-config / runtime-state caches (mutex-
 *        guarded, PSRAM), the poster task and the status/test surface.
 *
 * The poster (PSRAM stack — every payload source is RAM: the autopid
 * snapshot cache, the PSRAM config copy, dev_status getters; outbound
 * I/O goes through mqtt_manager / http_client_manager; stats are
 * cache-only) laps once a second (or at once when a test is requested):
 * for each enabled destination that is DUE it checks the link the type
 * needs (broker connected for MQTT, a network for HTTP/ABRP) — a down
 * link is a SKIPPED lap (counted, never a failure, never backed off:
 * the next lap after the link returns delivers), an attempt is booked
 * through the pure scheduler (success resets backoff; the 3rd
 * consecutive failure starts doubling the interval from 10 s up to
 * max(8 x period, 60 s)). Failures log ONCE per outage at W (the
 * transition), recoveries at I; attempts at D (§10).
 *
 * STACK RULE (found on the bench 2026-09-19: the first build overflowed
 * the MAIN task's stack in _start): the applied table is ~10 KB, so it is
 * never copied onto a caller's stack — readers borrow it under the lock
 * (dd_config_lock/peek/unlock) or copy ONE destination (~1.3 KB, poster
 * only, on its 32 KB stack).
 */
#include "data_destinations.h"

#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "dev_status_manager.h"
#include "log_manager.h"
#include "mqtt_manager.h"

#include "data_destinations_private.h"

static const char *TAG = "data_destinations";

/* 8192 words = 32 KB PSRAM (the ha_webhooks poster's figure): TLS
 * handshakes run on the caller's stack (>= 8 KB, http_client_manager
 * contract) under an already-deep cJSON build. */
#define DD_STACK_WORDS 8192
#define DD_TEST_WAIT_MS 20000

static dd_config_t s_cfg EXT_RAM_BSS_ATTR;
static dd_state_t  s_state[DD_MAX] EXT_RAM_BSS_ATTR;
static bool        s_configured;

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;

static TaskHandle_t  s_task;
static StaticTask_t  s_tcb;
static StackType_t   s_stack[DD_STACK_WORDS] EXT_RAM_BSS_ATTR;
static volatile bool s_run;

/* one-shot test job: index in, result out, signalled by the poster */
static SemaphoreHandle_t s_test_done;
static StaticSemaphore_t s_test_done_buf;
static volatile int      s_test_idx = -1;
static data_destinations_result_t s_test_result EXT_RAM_BSS_ATTR;

/* the poster's per-lap working copy of ONE destination (PSRAM, not stack) */
static dd_dest_t s_work EXT_RAM_BSS_ATTR;

/* logged-state per destination for the offline transitions */
static bool s_offline_logged[DD_MAX];

void dd_config_lock(void)
{
    if (s_lock != NULL)
    {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

void dd_config_unlock(void)
{
    if (s_lock != NULL)
    {
        xSemaphoreGive(s_lock);
    }
}

const dd_config_t *dd_config_peek(void)
{
    return &s_cfg;
}

void dd_format_utc(char out[DD_TS_LEN])
{
    time_t now = 0;
    struct tm utc;

    out[0] = '\0';
    time(&now);
    gmtime_r(&now, &utc);
    strftime(out, DD_TS_LEN, "%Y-%m-%dT%H:%M:%SZ", &utc);
}

/* ---- caches ---------------------------------------------------------------- */

void dd_config_store(const dd_config_t *cfg)
{
    dd_config_lock();
    s_cfg = *cfg;
    memset(s_state, 0, sizeof(s_state));
    dd_config_unlock();
    s_configured = true;
}

bool dd_config_is_configured(void)
{
    return s_configured;
}

size_t dd_config_count(void)
{
    dd_config_lock();

    size_t n = s_cfg.n;

    dd_config_unlock();
    return n;
}

int dd_config_find(const char *name)
{
    int idx = -1;

    dd_config_lock();

    for (size_t i = 0; name != NULL && i < s_cfg.n; i++)
    {
        if (strcmp(s_cfg.dest[i].name, name) == 0)
        {
            idx = (int)i;
        }
    }

    dd_config_unlock();
    return idx;
}

bool dd_state_get(size_t i, dd_state_t *out)
{
    if (out == NULL || i >= DD_MAX)
    {
        return false;
    }

    dd_config_lock();
    *out = s_state[i];
    dd_config_unlock();
    return true;
}

/* ---- the poster --------------------------------------------------------------- */

static bool link_up(dd_type_t type)
{
    if (type == DD_TYPE_MQTT)
    {
        return mqtt_manager_connected();
    }

    return dev_status_manager_any_set(DEV_STATUS_NETWORK_CONNECTED_MASK);
}

/** One destination's lap (d = the poster's PSRAM working copy): skip
 *  (link down), or deliver + book. */
static void run_one(size_t i, const dd_dest_t *d, bool forced,
                    data_destinations_result_t *res)
{
    int64_t now = esp_timer_get_time();
    dd_state_t st;

    (void)dd_state_get(i, &st);

    if (!forced && !dd_sched_due(&st, now))
    {
        return;
    }

    if (!link_up(d->type))
    {
        if (forced && res != NULL)
        {
            res->ok = false;
            snprintf(res->error, sizeof(res->error), "%s",
                     d->type == DD_TYPE_MQTT ? "mqtt: broker not connected"
                                             : "network down");
        }

        dd_sched_skip(&st, d->period_s, now);

        if (!s_offline_logged[i])
        {
            ESP_LOGW(TAG, "%s: %s - waiting", d->name,
                     d->type == DD_TYPE_MQTT ? "broker not connected"
                                             : "no network");
            s_offline_logged[i] = true;
        }

        dd_config_lock();
        s_state[i] = st;
        dd_config_unlock();
        return;
    }

    if (s_offline_logged[i])
    {
        ESP_LOGI(TAG, "%s: link back", d->name);
        s_offline_logged[i] = false;
    }

    data_destinations_result_t r = { 0 };
    bool ok = dd_post_one(d, &st, &r);

    now = esp_timer_get_time();
    st.last_status = r.status;

    if (ok)
    {
        dd_format_utc(st.last_ok_time);
        st.last_error[0] = '\0';

        if (st.ever_tried && !st.was_ok)
        {
            ESP_LOGI(TAG, "%s: delivering again (status %d)", d->name,
                     r.status);
        }

        ESP_LOGD(TAG, "%s: ok status %d in %u ms", d->name, r.status,
                 (unsigned)r.elapsed_ms);
    }
    else
    {
        snprintf(st.last_error, sizeof(st.last_error), "%s", r.error);
        dd_format_utc(st.last_error_time);

        if (!st.ever_tried || st.was_ok)
        {
            ESP_LOGW(TAG, "%s: delivery failed: %s", d->name, r.error);
        }
        else
        {
            ESP_LOGD(TAG, "%s: still failing: %s", d->name, r.error);
        }
    }

    dd_sched_after(&st, d->period_s, ok, now);

    if (!ok && st.consec_failures == DD_BACKOFF_AFTER)
    {
        ESP_LOGW(TAG, "%s: %u consecutive failures - backing off to %us",
                 d->name, (unsigned)st.consec_failures,
                 (unsigned)(st.backoff_ms / 1000));
    }

    dd_config_lock();
    s_state[i] = st;
    dd_config_unlock();

    if (res != NULL)
    {
        *res = r;
    }
}

/** Copy destination @p i into the poster's working buffer; false when
 *  out of range or disabled (unless @p even_disabled). */
static bool take_dest(size_t i, bool even_disabled)
{
    bool ok = false;

    dd_config_lock();

    if (i < s_cfg.n && (even_disabled || s_cfg.dest[i].enabled))
    {
        s_work = s_cfg.dest[i];
        ok = true;
    }

    dd_config_unlock();
    return ok;
}

static void poster_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(3000)); /* let the links come up */

    while (s_run)
    {
        /* a test request wakes the lap early */
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));

        int test_idx = s_test_idx;

        if (test_idx >= 0)
        {
            data_destinations_result_t r = { 0 };

            if (take_dest((size_t)test_idx, true))
            {
                run_one((size_t)test_idx, &s_work, true, &r);
            }
            else
            {
                r.ok = false;
                snprintf(r.error, sizeof(r.error), "unknown destination");
            }

            s_test_result = r;
            s_test_idx = -1;
            xSemaphoreGive(s_test_done);
        }

        dd_config_lock();

        bool enabled = s_cfg.enabled;
        size_t n = s_cfg.n;

        dd_config_unlock();

        if (!enabled)
        {
            continue;
        }

        for (size_t i = 0; i < n && s_run; i++)
        {
            if (take_dest(i, false))
            {
                run_one(i, &s_work, false, NULL);
            }
        }
    }

    ESP_LOGD(TAG, "poster stack hw %u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    s_task = NULL;
    vTaskDelete(NULL);
}

/* ---- public surface ---------------------------------------------------------- */

esp_err_t data_destinations_status(cJSON **out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out = NULL;

    if (!s_configured)
    {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *o = cJSON_CreateObject();

    if (o == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(o, "running", s_task != NULL);
    cJSON_AddBoolToObject(o, "network", dev_status_manager_any_set(
                                            DEV_STATUS_NETWORK_CONNECTED_MASK));
    cJSON_AddBoolToObject(o, "mqtt", mqtt_manager_connected());

    cJSON *arr = cJSON_CreateArray();
    int64_t now = esp_timer_get_time();

    /* borrow the table under the lock: no 10 KB copy on the httpd stack */
    dd_config_lock();
    cJSON_AddBoolToObject(o, "enabled", s_cfg.enabled);

    for (size_t i = 0; arr != NULL && i < s_cfg.n; i++)
    {
        const dd_dest_t *d = &s_cfg.dest[i];
        const dd_state_t *st = &s_state[i];
        cJSON *e = cJSON_CreateObject();

        if (e == NULL)
        {
            break;
        }

        cJSON_AddStringToObject(e, "name", d->name);
        cJSON_AddStringToObject(e, "type", dd_type_str(d->type));
        cJSON_AddBoolToObject(e, "enabled", d->enabled);
        cJSON_AddStringToObject(e, "url", d->url);
        cJSON_AddNumberToObject(e, "period_s", (double)d->period_s);
        cJSON_AddStringToObject(e, "auth", dd_auth_str(d->auth));
        cJSON_AddBoolToObject(e, "has_token", d->auth_token[0] != '\0');
        cJSON_AddBoolToObject(e, "has_api_key", d->api_key[0] != '\0');
        cJSON_AddStringToObject(e, "cert_set", d->cert_set);
        cJSON_AddNumberToObject(e, "success", (double)st->success);
        cJSON_AddNumberToObject(e, "fail", (double)st->fail);
        cJSON_AddNumberToObject(e, "skipped_offline",
                                (double)st->skipped_offline);
        cJSON_AddNumberToObject(e, "consecutive_failures",
                                (double)st->consec_failures);
        cJSON_AddNumberToObject(e, "backoff_s",
                                (double)(st->backoff_ms / 1000));

        int64_t in_us = st->next_due_us > now ? st->next_due_us - now : 0;

        cJSON_AddNumberToObject(e, "next_in_s",
                                (double)(in_us / 1000000));
        cJSON_AddNumberToObject(e, "last_status", st->last_status);
        cJSON_AddStringToObject(e, "last_error", st->last_error);
        cJSON_AddStringToObject(e, "last_error_time", st->last_error_time);
        cJSON_AddStringToObject(e, "last_ok_time", st->last_ok_time);
        cJSON_AddBoolToObject(e, "full_sent", st->settings_sent);
        cJSON_AddItemToArray(arr, e);
    }

    dd_config_unlock();

    if (arr != NULL)
    {
        cJSON_AddItemToObject(o, "destinations", arr);
    }

    *out = o;
    return ESP_OK;
}

esp_err_t data_destinations_test(const char *name,
                                 data_destinations_result_t *out)
{
    if (name == NULL || out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_configured || s_task == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    int idx = dd_config_find(name);

    if (idx < 0)
    {
        return ESP_ERR_NOT_FOUND;
    }

    if (s_test_idx >= 0)
    {
        return ESP_ERR_INVALID_STATE; /* one test at a time */
    }

    /* drain a stale completion, then post the job */
    (void)xSemaphoreTake(s_test_done, 0);
    s_test_idx = idx;
    xTaskNotifyGive(s_task);

    if (xSemaphoreTake(s_test_done, pdMS_TO_TICKS(DD_TEST_WAIT_MS)) != pdTRUE)
    {
        s_test_idx = -1;
        return ESP_ERR_TIMEOUT;
    }

    *out = s_test_result;
    return ESP_OK;
}

/* ---- lifecycle ---------------------------------------------------------------- */

esp_err_t data_destinations_init(void)
{
    static const log_descriptor_t LOG_DESC =
    { "data_destinations", ESP_LOG_INFO };

    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }

    if (s_test_done == NULL)
    {
        s_test_done = xSemaphoreCreateBinaryStatic(&s_test_done_buf);
    }

    log_manager_register(&LOG_DESC);
    return dd_settings_register();
}

esp_err_t data_destinations_start(void)
{
    if (!s_configured)
    {
        return ESP_ERR_INVALID_STATE; /* §4.3 step 5 */
    }

    if (s_task != NULL)
    {
        return ESP_OK;
    }

    size_t n;
    size_t on = 0;
    bool enabled;

    dd_config_lock();
    n = s_cfg.n;
    enabled = s_cfg.enabled;

    for (size_t i = 0; i < n; i++)
    {
        on += s_cfg.dest[i].enabled ? 1 : 0;
    }

    dd_config_unlock();

    ESP_LOGI(TAG, "up (enabled=%d, %u destinations, %u on)", enabled,
             (unsigned)n, (unsigned)on);

    s_run = true;
    s_task = xTaskCreateStatic(poster_task, "data_dest", DD_STACK_WORDS,
                               NULL, 4, s_stack, &s_tcb);
    return s_task != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t data_destinations_stop(void)
{
    s_run = false; /* the task exits on its next lap */

    if (s_task != NULL)
    {
        xTaskNotifyGive(s_task);
    }

    return ESP_OK;
}
