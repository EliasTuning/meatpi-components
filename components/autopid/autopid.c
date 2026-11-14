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
 * @file autopid.c
 * @brief Lifecycle, the config tables with their lock, and the public
 *        API. The poller task (scheduler -> runner -> expression eval ->
 *        cache) lives in autopid_poller.c; settings in autopid_settings.c
 *        (standard §4.1).
 *
 * Threading: s_lock guards the config tables + scheduler state. The
 * poller task copies what it needs per iteration and NEVER touches the
 * filesystem: config loads run in the caller's context (main at boot,
 * httpd on PUT), then re-arm the scheduler.
 */
#include "autopid.h"

#include <math.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "battery_monitor.h"
#include "sleep_manager.h"
#include "dev_status_manager.h"
#include "log_manager.h"
#include "obd_chip.h"

#include "expression_parser.h"

#include "autopid_private.h"
#include "autopid_poller.h"
#include "autopid_runner_private.h" /* ap_base_proto */

static const char *TAG = "autopid";

/* ---- state ------------------------------------------------------------------- */

static ap_config_t s_cfg EXT_RAM_BSS_ATTR;
static ap_sched_t  s_sched EXT_RAM_BSS_ATTR;

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;   /* internal: FreeRTOS object */

static bool s_started;

/* the tables were replaced (arm_scheduler): the chip runner remembers the
   row it sent last by its INDEX, which names another row now. Set and taken
   under s_lock; the poller takes it with the row it picks and forgets what
   it sent (bench 2026-11-10: a table of one row put in the place of a table
   of one row never got its own init: 0 answered polls until a restart) */
static bool s_tables_new;

/* enabled: seeded by on_apply (ap_core_set_enabled), cleared by
 * autopid_stop(), runtime-mutated, so it lives here; the boot-applied
 * knobs live in autopid_settings.c */
static bool s_enabled;

/* runtime */
static QueueHandle_t s_batt_q;
static int           s_batt_watch = -1;
static autopid_stats_t s_stats;

const ap_config_t *ap_core_config(void)
{
    return &s_cfg;
}

bool ap_core_enabled(void)
{
    return s_enabled;
}

autopid_stats_t *ap_core_stats(void)
{
    return &s_stats;
}

/* ---- settings hooks (autopid_settings.c on_apply context) ----------------------- */

void ap_core_set_enabled(bool enabled)
{
    s_enabled = enabled;
}

void ap_core_set_type_enabled(int type, bool enabled)
{
    s_sched.type_enabled[type] = enabled;
}

void ap_core_set_type_held(int type, bool held)
{
    /* the runner's verdict on the type's init (ap_runner_set_type_init): a
       plain flag write like the knob above, read by the scheduler */
    s_sched.type_held[type] = held;
}

/* ---- config (re)load: CALLER context, never the poller -------------------------- */

static void arm_scheduler(void)
{
    ap_sched_reset(&s_sched, &s_cfg, esp_timer_get_time());
    ap_runner_j1939_reset(); /* row indexes changed: publish afresh */
    s_tables_new = true;     /* ... and the chip rows send their inits again */
    s_stats.params_loaded = s_cfg.n_params;
    s_stats.pids_loaded = s_cfg.n_pids;
    s_stats.filters_loaded = s_cfg.n_filters;
    s_stats.groups_loaded = s_cfg.n_groups;
}

esp_err_t autopid_reload_config(void)
{
    static ap_config_t s_staging EXT_RAM_BSS_ATTR;

    esp_err_t err = autopid_config_load(&s_staging);

    /* missing file = legitimate empty tables */
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND)
    {
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    /* preserve the type_enabled knobs (settings-owned) and the holds (the
       runner's: a type init with a word the chip stores) */
    bool types[3];
    bool held[3];

    memcpy(types, s_sched.type_enabled, sizeof(types));
    memcpy(held, s_sched.type_held, sizeof(held));
    s_cfg = s_staging;
    arm_scheduler();
    memcpy(s_sched.type_enabled, types, sizeof(types));
    memcpy(s_sched.type_held, held, sizeof(held));
    ap_cache_clear();
    ap_events_reset(); /* slot indices changed with the tables */
    xSemaphoreGive(s_lock);

    ap_core_wake();

    return ESP_OK;
}

/* ---- public API ------------------------------------------------------------------- */

esp_err_t autopid_init(void)
{
    static const log_descriptor_t LOG_DESC = { "autopid", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);

    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }

    ap_cache_init();
    ap_dtc_init();
    ap_dtc_db_init();
    ap_dbc_init();
    ap_guard_init();
    ap_events_register(); /* Phase 3: sources/action/pull values */

    /* main-task context: internal stack, file IO is safe here */
    (void)autopid_config_load(&s_cfg);
    ap_vehicle_load();   /* /data/autopid/vehicles.json -> RAM copy */
    arm_scheduler();

    return ap_settings_register();
}

esp_err_t autopid_start(void)
{
    if (!ap_settings_is_configured())
    {
        return ESP_ERR_INVALID_STATE; /* §4.3 step 5 */
    }

    if (s_started)
    {
        return ESP_OK;
    }

    int pause_below_mv = ap_settings_pause_below_mv();
    float resume_v = 0.0f; /* 0 = the fixed threshold's + 0.3 V rule */

    if (pause_below_mv == 0 && ap_settings_pause_follow_sleep())
    {
        /* legacy disable_pid_requests parity: follow the sleep
         * threshold so a parked car's ECU is never polled awake; since
         * sleep_manager v3 (2026-10-01) polling resumes at the user's
         * wake voltage, so one pair of numbers rules both */
        sleep_manager_status_t sst;

        if (sleep_manager_status(&sst) == ESP_OK && sst.sleep_v > 1.0f)
        {
            pause_below_mv = (int)(sst.sleep_v * 1000.0f);
            resume_v = sst.wake_v;
            ESP_LOGI(TAG, "request pause follows sleep voltage "
                     "(%.2f V, resumes at %.2f V)", sst.sleep_v, sst.wake_v);
        }
    }

    if (pause_below_mv > 0)
    {
        static StaticQueue_t qbuf;                 /* internal: FreeRTOS */
        static uint8_t qstore[4 * sizeof(battery_monitor_event_t)]
            EXT_RAM_BSS_ATTR;

        s_batt_q = xQueueCreateStatic(4, sizeof(battery_monitor_event_t),
                                      qstore, &qbuf);

        battery_monitor_watch_cfg_t w =
        {
            .below_v = (float)pause_below_mv / 1000.0f,
            .above_v = resume_v > 0.0f ? resume_v
                                       : (float)pause_below_mv / 1000.0f + 0.3f,
            .hold_ms = 5000,
        };

        if (battery_monitor_watch(&w, s_batt_q, &s_batt_watch) != ESP_OK)
        {
            ESP_LOGW(TAG, "voltage-pause watch unavailable");
            s_batt_watch = -1;
        }
    }

    /* DTC databases + DBC files: cache loads need flash reads, main
     * task context (internal stack) is the sanctioned place (§2) */
    ap_dtc_db_load_all();
    ap_dbc_load_all();

    ap_poller_start(s_batt_q);
    s_started = true;
    ESP_LOGI(TAG, "started (%s; %u pids / %u params, %u groups)",
             s_enabled ? "enabled" : "disabled by settings",
             s_cfg.n_pids, s_cfg.n_params, s_cfg.n_groups);
    return ESP_OK;
}

esp_err_t autopid_stop(void)
{
    if (!s_started)
    {
        return ESP_OK;
    }

    s_enabled = false; /* the task idles; init-once, no teardown (§3) */
    s_started = false;
    dev_status_manager_clear(DEV_STATUS_BIT_AUTOPID_ENABLED);
    /* AUTOPID_IDLE is set by the poller once any in-flight request
     * finishes: sleep_manager waits on exactly that. */
    return ESP_OK;
}

/* ---- runtime group control (autopid_group.c) and the poller task
   (autopid_poller.c) borrow the core's lock and tables through these;
   ap_core_wake / ap_core_scan_pause live with the task ------------------ */

void ap_core_lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

void ap_core_unlock(void)
{
    xSemaphoreGive(s_lock);
}

bool ap_core_tables_new(void)
{
    bool fresh = s_tables_new;

    s_tables_new = false;
    return fresh;
}

ap_sched_t *ap_core_sched(void)
{
    return &s_sched;
}

/* One-shot chip jobs (std scan / test-a-PID / dtc scan / dtc clear)
 * never interleave: each would trash the other's protocol/header
 * state mid-flight (TASK_dtc.md §5). */
static volatile bool s_job_busy;

bool ap_core_job_acquire(void)
{
    bool got = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (!s_job_busy)
    {
        s_job_busy = true;
        got = true;
    }

    xSemaphoreGive(s_lock);
    return got;
}

void ap_core_job_release(void)
{
    s_job_busy = false;
}

uint16_t ap_core_sub_floor_count(void)
{
    uint16_t n = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    for (int i = 0; i < s_cfg.n_pids; i++)
    {
        if (!s_cfg.pids[i].enabled)
        {
            continue;
        }

        /* configured intent (inherit/override, no fail backoff);
           period 0 = deliberate max-rate mode, not a mistake */
        int g = s_cfg.pids[i].group;
        int64_t period_ms = s_cfg.pids[i].period_ms;

        if (period_ms == 0 && g >= 0 && g < s_cfg.n_groups)
        {
            period_ms = (s_sched.group_period_override[g] >= 0)
                            ? s_sched.group_period_override[g]
                            : (int64_t)s_cfg.groups[g].period_ms;
        }

        if (period_ms > 0 && period_ms < AP_PERIOD_FLOOR_MS)
        {
            n++;
        }
    }

    xSemaphoreGive(s_lock);
    return n;
}

esp_err_t autopid_snapshot(cJSON **out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = ap_cache_snapshot(&s_cfg);
    xSemaphoreGive(s_lock);

    return (*out != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t autopid_get_value(const char *param, double *out_value,
                            int64_t *out_ts_us)
{
    if (param == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    int slot = ap_cache_find(&s_cfg, param);

    xSemaphoreGive(s_lock);

    if (slot < 0)
    {
        /* not a polled parameter: try the injected values (GPS etc.) */
        return ap_ext_get(param, out_value, out_ts_us) ? ESP_OK
                                                       : ESP_ERR_NOT_FOUND;
    }

    return ap_cache_get((uint16_t)slot, out_value, out_ts_us)
               ? ESP_OK
               : ESP_ERR_INVALID_STATE;
}

esp_err_t autopid_publish_external(const char *name, const char *unit,
                                   double value)
{
    if (name == NULL || name[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    bool changed = false;

    if (ap_ext_put(name, unit, value, esp_timer_get_time(), &changed) < 0)
    {
        return ESP_ERR_NO_MEM; /* external table full */
    }

    /* same downstream as a polled sample: value sink (data_logger) +
       the autopid.param event (event_manager rules) */
    ap_events_external(name, unit != NULL ? unit : "", value, changed);
    return ESP_OK;
}

esp_err_t autopid_stats(autopid_stats_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out = s_stats;
    ap_poller_pauses(out);
    return ESP_OK;
}

bool autopid_ecu_online(void)
{
    /* "online" = the poller is up and an ECU answered a poll within the
     * last 30 s (generous vs the default cycle; a parked/off vehicle
     * goes offline one window after its last answer). */
    int64_t last_ok_us = ap_poller_last_ok_us();

    if (!s_stats.running || last_ok_us == 0)
    {
        return false;
    }

    return (esp_timer_get_time() - last_ok_us) < 30 * 1000000LL;
}

size_t autopid_protocol_text(char *out, size_t cap)
{
    /* what the prelude pins: '0' (the search) is no row of the table,
       so it comes out as "" */
    return ap_proto_text(ap_base_proto(), out, cap);
}
