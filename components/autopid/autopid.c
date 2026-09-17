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
 * @brief Lifecycle and the poller task (scheduler -> runner ->
 *        expression eval -> cache). Settings live in autopid_settings.c
 *        (standard §4.1).
 *
 * Threading: s_lock guards the config tables + scheduler state. The
 * poller task copies what it needs per iteration and NEVER touches the
 * filesystem (its stack is PSRAM; LittleFS access from a PSRAM stack is
 * the §2-corollary panic) — config loads run in the caller's context
 * (main at boot, httpd on PUT), then re-arm the scheduler.
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

#include "obd_gate.h"
#include "autopid_private.h"
#include "autopid_transport.h"

static const char *TAG = "autopid";

#define AP_REQ_TIMEOUT   pdMS_TO_TICKS(1500)
#define AP_INIT_TIMEOUT  pdMS_TO_TICKS(2000)
#define AP_IDLE_WAIT     pdMS_TO_TICKS(500)

/* ---- state ------------------------------------------------------------------- */

static ap_config_t s_cfg EXT_RAM_BSS_ATTR;
static ap_sched_t  s_sched EXT_RAM_BSS_ATTR;
/* the due entry's parameter slice, copied under s_lock and consumed by
   the poller task alone — PSRAM, not a stack frame: at AP_PARAMS_PER
   256 the slice is ~60 KB, larger than the whole task stack */
static ap_param_t  s_params_copy[AP_PARAMS_PER] EXT_RAM_BSS_ATTR;

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;   /* internal: FreeRTOS object */

static TaskHandle_t s_task;
static StaticTask_t s_tcb;             /* internal: FreeRTOS object */
static StackType_t  s_stack[12288] EXT_RAM_BSS_ATTR; /* PSRAM: 8192 left
                                only 508 B headroom (System Monitor,
                                2026-07-08) over ap_resp_to_payload's
                                ~5.6 KB line table — the dtc job stack
                                DID overflow on the same table
                                (2026-07-22), so buy real margin; no fs
                                I/O on this task so PSRAM is safe */

static bool s_started;

/* enabled: seeded by on_apply (ap_core_set_enabled), cleared by
 * autopid_stop() — runtime-mutated, so it lives here; the boot-applied
 * knobs live in autopid_settings.c */
static bool s_enabled;

/* runtime */
static volatile bool s_paused_voltage;
static volatile bool s_paused_client;     /* an app is driving the chip */
static volatile bool s_scan_pause;        /* std scan owns the chip     */
static volatile bool s_paused_diag;       /* a diagnostic tool holds the bus (obd_gate) */
static QueueHandle_t s_batt_q;
static int           s_batt_watch = -1;
static autopid_stats_t s_stats;
static int64_t s_last_ok_us; /* last successful ECU poll (0 = never) */

const ap_config_t *ap_core_config(void)
{
    return &s_cfg;
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

/* ---- the poller task --------------------------------------------------------------- */

static void poller_task(void *arg)
{
    (void)arg;

    while (true)
    {
        /* voltage pause events (non-blocking drain) */
        if (s_batt_q != NULL)
        {
            battery_monitor_event_t ev;

            while (xQueueReceive(s_batt_q, &ev, 0) == pdTRUE)
            {
                bool below = (ev.type == BATTERY_MONITOR_EVENT_BELOW);

                if (below != s_paused_voltage)
                {
                    s_paused_voltage = below;
                    ESP_LOGI(TAG, "voltage pause %s (%.2f V)",
                             below ? "ON" : "OFF", ev.voltage);
                }
            }
        }

        /* yield to an external app (Car Scanner & co. over a bridge):
         * while it drives the chip our requests would interleave with
         * its conversations - it would read our lines as its answers and
         * our commands would STOP its requests (bench 2026-09-08). Legacy
         * parity: off the chip until the app has been silent
         * AP_CLIENT_YIELD_MS. */
        bool client = (ap_be()->client_idle_ms != NULL) &&
                      ap_sched_client_hold(ap_be()->client_idle_ms());

        if (client != s_paused_client)
        {
            s_paused_client = client;

            if (client)
            {
                ESP_LOGI(TAG, "paused: external OBD client active (resumes "
                         "%u s after its last command)",
                         (unsigned)(AP_CLIENT_YIELD_MS / 1000));
            }
            else
            {
                ESP_LOGI(TAG, "resumed: external OBD client idle");

                /* the app left the chip in ITS state (ATS0/ATH1/ATSH/
                   ATCRA...): restore the baseline the parser and the
                   PID inits assume before the first poll (bench
                   2026-09-08: every resumed poll failed on the app's
                   ATS0 otherwise). Only when we are going to poll. */
                if (s_enabled && !s_paused_voltage && !s_scan_pause)
                {
                    ap_runner_restore_baseline();
                }
            }
        }

        /* an ESP-side diagnostic tool (the UDS Tool, the J2534 PassThru
         * server) with its "exclusive" option on holds the bus for itself
         * (obd_gate's diagnostics hold): stay off the chip — polls AND DTC
         * scans — and acknowledge every loop so the tool can wait for us
         * to be off the bus before its first request. */
        bool diag = obd_gate_diag_held();

        if (diag != s_paused_diag)
        {
            s_paused_diag = diag;

            if (diag)
            {
                ESP_LOGI(TAG, "paused: a diagnostic tool holds the bus "
                              "(UDS Tool / J2534 exclusive)");
            }
            else
            {
                ESP_LOGI(TAG, "resumed: the diagnostic tool released the bus");

                /* the UDS chip transport re-addressed the chip (ATSH/
                   ATCRA/ATST): back to our baseline before polling */
                if (s_enabled && !s_paused_voltage && !s_scan_pause &&
                    !s_paused_client)
                {
                    ap_runner_restore_baseline();
                }
            }
        }

        obd_gate_diag_ack(diag);

        /* periodic DTC scan due-check — runs every iteration, incl. the
         * idle branch, so DTC works with polling disabled (dtc_enabled
         * without enabled; TASK_dtc.md §5). Voltage pause gates it: a
         * weak battery is no time for bus traffic; the client pause too. */
        if (!s_paused_voltage && !s_scan_pause && !s_paused_client &&
            !s_paused_diag)
        {
            ap_dtc_periodic_check();
        }

        if (!s_enabled || s_paused_voltage || s_scan_pause ||
            s_paused_client || s_paused_diag)
        {
            s_stats.running = false;
            dev_status_manager_set(DEV_STATUS_BIT_AUTOPID_IDLE);
            ulTaskNotifyTake(pdTRUE, AP_IDLE_WAIT);
            continue;
        }

        /* pick the next due entry under the lock; copy what we need */
        xSemaphoreTake(s_lock, portMAX_DELAY);

        int64_t now = esp_timer_get_time();
        int64_t due = 0;
        int i = ap_sched_next(&s_sched, &s_cfg, &due);
        ap_pid_t pid_copy;
        ap_filter_t flt_copy;
        ap_param_t *params_copy = s_params_copy;
        bool is_pid = false, is_filter = false;

        if (i >= 0 && due <= now && i < s_cfg.n_pids)
        {
            pid_copy = s_cfg.pids[i];
            memcpy(params_copy, &s_cfg.params[pid_copy.param_start],
                   sizeof(ap_param_t) * pid_copy.param_count);
            is_pid = true;
        }
        else if (i >= 0 && due <= now)
        {
            flt_copy = s_cfg.filters[i - s_cfg.n_pids];
            memcpy(params_copy, &s_cfg.params[flt_copy.param_start],
                   sizeof(ap_param_t) * flt_copy.param_count);
            is_filter = true;
        }

        xSemaphoreGive(s_lock);

        if (i < 0)
        {
            s_stats.running = false;
            dev_status_manager_set(DEV_STATUS_BIT_AUTOPID_IDLE);
            ulTaskNotifyTake(pdTRUE, AP_IDLE_WAIT);
            continue;
        }

        s_stats.running = true;

        if (due > now)
        {
            int64_t wait_us = due - now;
            TickType_t ticks = pdMS_TO_TICKS((wait_us / 1000) + 1);

            dev_status_manager_set(DEV_STATUS_BIT_AUTOPID_IDLE);
            ulTaskNotifyTake(pdTRUE,
                             (ticks > AP_IDLE_WAIT) ? AP_IDLE_WAIT : ticks);
            continue; /* re-evaluate: config/groups may have changed */
        }

        bool ok = true;

        if (is_pid || is_filter)
        {
            /* Busy window: sleep_manager waits for AUTOPID_IDLE before
             * cutting the chip/rail (§ sleep prepare) */
            dev_status_manager_clear(DEV_STATUS_BIT_AUTOPID_IDLE);

            ok = is_pid ? ap_runner_run(&pid_copy, i, params_copy)
                        : ap_runner_run_filter(&flt_copy, params_copy);

            if (ok)
            {
                s_stats.polls_ok++;
                s_last_ok_us = esp_timer_get_time();
            }
            else
            {
                s_stats.polls_failed++;
            }

            s_stats.last_poll_us = esp_timer_get_time();
        }

        char failed_name[AP_NAME_LEN + 12] = "";
        uint16_t streak = 0;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        ap_sched_ran(&s_sched, &s_cfg, i, esp_timer_get_time(), ok);

        if (!ok && (is_pid || is_filter))
        {
            streak = s_sched.slots[i].fail_streak;

            if (is_pid)
            {
                snprintf(failed_name, sizeof(failed_name), "%s",
                         pid_copy.name);
            }
            else
            {
                snprintf(failed_name, sizeof(failed_name), "filter %lX",
                         (unsigned long)flt_copy.frame_id);
            }
        }

        xSemaphoreGive(s_lock);

        if (failed_name[0] != '\0')
        {
            ap_events_pid_failed(failed_name, streak);
        }
    }
}

/* ---- config (re)load — CALLER context, never the poller -------------------------- */

static void arm_scheduler(void)
{
    ap_sched_reset(&s_sched, &s_cfg, esp_timer_get_time());
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

    /* preserve the type_enabled knobs (settings-owned) */
    bool types[3];

    memcpy(types, s_sched.type_enabled, sizeof(types));
    s_cfg = s_staging;
    arm_scheduler();
    memcpy(s_sched.type_enabled, types, sizeof(types));
    ap_cache_clear();
    ap_events_reset(); /* slot indices changed with the tables */
    xSemaphoreGive(s_lock);

    if (s_task != NULL)
    {
        xTaskNotifyGive(s_task);
    }

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
    ap_events_register(); /* Phase 3: sources/action/pull values */

    /* main-task context: internal stack, file IO is safe here */
    (void)autopid_config_load(&s_cfg);
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

    if (pause_below_mv == 0 && ap_settings_pause_follow_sleep())
    {
        /* legacy disable_pid_requests parity: follow the sleep
         * threshold so a parked car's ECU is never polled awake */
        sleep_manager_status_t sst;

        if (sleep_manager_status(&sst) == ESP_OK && sst.sleep_v > 1.0f)
        {
            pause_below_mv = (int)(sst.sleep_v * 1000.0f);
            ESP_LOGI(TAG, "request pause follows sleep voltage "
                     "(%.2f V)", sst.sleep_v);
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
            .above_v = (float)pause_below_mv / 1000.0f + 0.3f,
            .hold_ms = 5000,
        };

        if (battery_monitor_watch(&w, s_batt_q, &s_batt_watch) != ESP_OK)
        {
            ESP_LOGW(TAG, "voltage-pause watch unavailable");
            s_batt_watch = -1;
        }
    }

    /* DTC databases + DBC files: cache loads need flash reads — main
     * task context (internal stack) is the sanctioned place (§2) */
    ap_dtc_db_load_all();
    ap_dbc_load_all();

    s_task = xTaskCreateStatic(poller_task, "autopid",
                               sizeof(s_stack) / sizeof(s_stack[0]), NULL,
                               5, s_stack, &s_tcb);
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
     * finishes — sleep_manager waits on exactly that. */
    return ESP_OK;
}

/* ---- runtime group control lives in autopid_group.c: it borrows the
   core's lock, tables and poller handle through these ------------------- */

void ap_core_lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

void ap_core_unlock(void)
{
    xSemaphoreGive(s_lock);
}

ap_sched_t *ap_core_sched(void)
{
    return &s_sched;
}

void ap_core_wake(void)
{
    if (s_task != NULL)
    {
        xTaskNotifyGive(s_task);
    }
}

void ap_core_scan_pause(bool on)
{
    s_scan_pause = on;

    if (!on)
    {
        /* the scan changed protocol/header state under the chip */
        ap_runner_reset();
    }

    if (s_task != NULL)
    {
        xTaskNotifyGive(s_task);
    }
}

/* One-shot chip jobs (std scan / test-a-PID / dtc scan / dtc clear)
 * never interleave — each would trash the other's protocol/header
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
        /* not a polled parameter — try the injected values (GPS etc.) */
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
    out->paused_voltage = s_paused_voltage;
    out->paused_client = s_paused_client;
    out->paused_diag = s_paused_diag;
    return ESP_OK;
}

bool autopid_ecu_online(void)
{
    /* "online" = the poller is up and an ECU answered a poll within the
     * last 30 s (generous vs the default cycle; a parked/off vehicle
     * goes offline one window after its last answer). */
    if (!s_stats.running || s_last_ok_us == 0)
    {
        return false;
    }

    return (esp_timer_get_time() - s_last_ok_us) < 30 * 1000000LL;
}
