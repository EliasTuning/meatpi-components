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
 * @file fw_update.c
 * @brief The engine: lifecycle, the one task (a 12 KB PSRAM stack: the TLS
 *        handshakes run on it, the flash never does), the command queue,
 *        the shared state, the schedule with jitter, the sleep hold, the
 *        verify milestone and the pre-restart confirm. The job runners
 *        live in fw_update_job.c, the public API in fw_update_api.c, the
 *        dongle relay in fw_update_dongle.c (2026-10-19).
 */
#include "fw_update_private.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "config_backup.h"
#include "dev_status_manager.h"
#include "log_manager.h"
#include "ota_manager.h"
#include "partition_migrate.h"
#include "restart_tracker.h"
#include "sleep_manager.h"

static const char *TAG = FWU_TAG;


static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;          /* internal: FreeRTOS object */
static StaticQueue_t s_cmds_buf;              /* internal: FreeRTOS object */
static uint8_t s_cmds_store[4 * sizeof(fwu_cmd_t)] EXT_RAM_BSS_ATTR;
static TaskHandle_t s_task;
static StaticTask_t s_tcb;                    /* internal: FreeRTOS object */
static StackType_t s_stack[FWU_TASK_STACK] EXT_RAM_BSS_ATTR;

/* the engine's state, one PSRAM object (about 75 KB, the manifest view
   most of it), shared with the job, api and dongle files (fwu_state()) */
static fwu_state_t s_state EXT_RAM_BSS_ATTR;

fwu_state_t *fwu_state(void)
{
    return &s_state;
}

/* ---- the lock and the lent view ---------------------------------------------------- */

void fwu_engine_lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

void fwu_engine_unlock(void)
{
    xSemaphoreGive(s_lock);
}

const fwu_manifest_t *fwu_engine_manifest(bool *valid)
{
    *valid = FWU->view_valid;
    return &FWU->view;
}

const fwu_selection_t *fwu_engine_selection(void)
{
    return &FWU->sel;
}

const fwu_job_t *fwu_engine_job(void)
{
    return &FWU->job;
}

const fwu_check_t *fwu_engine_check(void)
{
    return &FWU->check;
}

const fwu_record_t *fwu_engine_record(bool *valid)
{
    *valid = FWU->rec_valid;
    return &FWU->rec;
}

int32_t fwu_engine_next_check_s(void)
{
    if (FWU->next_check_us == 0 || !fwu_settings_config()->enabled)
    {
        return -1;
    }

    int64_t d = FWU->next_check_us - esp_timer_get_time();

    return (d > 0) ? (int32_t)(d / 1000000) : 0;
}

const char *fwu_engine_uplink(bool *is_cellular)
{
    *is_cellular = false;
    return (FWU->uplink != NULL) ? FWU->uplink(is_cellular) : "unknown";
}

/* ---- helpers ----------------------------------------------------------------------- */

bool fwu_network_up(void)
{
    return dev_status_manager_any_set(DEV_STATUS_NETWORK_CONNECTED_MASK);
}

/** The selection against the view, under the lock. */
void fwu_reselect(void)
{
    char md5[33] = "";

    (void)partition_migrate_flashed_md5(md5);

    if (FWU->view_valid)
    {
        fwu_select(&FWU->view, fwu_settings_config()->channel,
                   dev_status_manager_app_version(), md5, &FWU->sel);
    }
    else
    {
        fwu_select(NULL, FWU_CH_STABLE, "", md5, &FWU->sel);
    }
}

void fwu_job_begin(uint32_t id, fw_update_target_t target,
                      const char *version, uint32_t flags,
                      fw_update_job_state_t state)
{
    memset(&FWU->job, 0, sizeof(FWU->job));
    FWU->job.active = true;
    FWU->job.id = id;
    FWU->job.target = target;
    FWU->job.state = state;
    FWU->job.flags = flags;
    FWU->job.t_start_us = esp_timer_get_time();
    strlcpy(FWU->job.version, (version != NULL) ? version : "", sizeof(FWU->job.version));
}

void fwu_job_fail(fw_update_job_state_t state, const char *code,
                     const char *detail)
{
    FWU->job.state = state;
    FWU->job.active = false;
    strlcpy(FWU->job.error, code, sizeof(FWU->job.error));
    strlcpy(FWU->job.detail, (detail != NULL) ? detail : "", sizeof(FWU->job.detail));
    /* a job just re-fetched the manifest: push the scheduled check out so one
       does not fire on its heels and churn the job the UI is showing
       (2026-10-19; the keep logic would restore it, but better not to churn) */
    if (FWU->next_check_us != 0)
    {
        FWU->next_check_us = esp_timer_get_time() + fwu_jittered_interval_us();
    }
    ESP_LOGW(TAG, "job %lu %s: %s%s%s", (unsigned long)FWU->job.id,
             fwu_job_state_str(state), code, detail && detail[0] ? ", " : "",
             detail ? detail : "");
}

void fwu_notify(void)
{
    static fw_update_status_t st EXT_RAM_BSS_ATTR;

    if (FWU->event_cb != NULL && fwu_status_build(&st) == ESP_OK)
    {
        FWU->event_cb(&st);
    }
}

/* ---- flash work on an internal stack ------------------------------------------------ */

void fwu_write_record_fn(void *arg)
{
    (void)arg;
    (void)fwu_store_write(&FWU->rec);
}

/* the helper's answer to a confirm: the slots as the flash says them now.
   One helper runs at a time (fwu_run_internal's gate); the caller copies it
   into the state under the engine lock, which the helper never takes */
static fwu_slots_t s_slots_read EXT_RAM_BSS_ATTR;

void fwu_confirm_fn(void *arg)
{
    (void)arg;
    (void)ota_manager_confirm_running();
    fwu_slots_read_fn(&s_slots_read);
}

void fwu_erase_record_fn(void *arg)
{
    (void)arg;
    (void)fwu_store_erase();
}

/** The milestone: the record goes VERIFIED, the bootloader hears it. */
void fwu_verify_now(const char *why)
{
    const esp_partition_t *running = esp_ota_get_running_partition();

    fwu_engine_lock();

    bool pending = FWU->rec_valid && FWU->rec.state == FW_JOB_REBOOTING &&
                   running != NULL && running->subtype == FWU->rec.slot_subtype;

    if (pending)
    {
        FWU->rec.state = FW_JOB_VERIFIED;
        FWU->rec.by = FWU_BY_NONE;
        (void)fwu_run_internal(fwu_write_record_fn, NULL, 5000);
        ESP_LOGI(TAG, "update job %lu to %s verified (%s)",
                 (unsigned long)FWU->rec.job_id, FWU->rec.to_version, why);
    }

    fwu_engine_unlock();

    if (fwu_run_internal(fwu_confirm_fn, NULL, 5000) == ESP_OK)
    {
        /* the slots after the confirm: what the status shows from now on */
        fwu_engine_lock();
        FWU->slots = s_slots_read;
        fwu_engine_unlock();
    }

    if (pending)
    {
        fwu_notify();
    }
}

/** Before a planned restart that is not a rollback or a park retry: the
 *  bootloader must not fall back to the old image because a settings
 *  submit came two minutes after the update. */
static void pre_restart_hook(restart_tracker_planned_reason_t reason,
                             restart_tracker_source_t source, uint32_t flags)
{
    (void)source;
    (void)flags;

    if (reason == RESTART_TRACKER_PLANNED_REASON_OTA_ROLLBACK ||
        reason == RESTART_TRACKER_PLANNED_REASON_PARK_RETRY)
    {
        return;
    }

    (void)fwu_run_internal(fwu_confirm_fn, NULL, 3000);
}

void fwu_restart_task(void *arg)
{
    uint32_t job_id = (uint32_t)(uintptr_t)arg;

    vTaskDelay(pdMS_TO_TICKS(FWU_RESTART_DELAY_MS));
    restart_tracker_restart(RESTART_TRACKER_PLANNED_REASON_OTA_APPLY,
                            RESTART_TRACKER_SOURCE_OTA, job_id);
}

/* ---- the task ------------------------------------------------------------------------- */

int64_t fwu_jittered_interval_us(void)
{
    uint64_t base = (uint64_t)fwu_settings_config()->check_interval_h * 3600ULL *
                    1000000ULL;
    int64_t jitter = (int64_t)(base / 10U);          /* +-10 % */
    int64_t r = (int64_t)(esp_random() % (uint32_t)(2 * (jitter / 1000000) + 1));

    return (int64_t)base - jitter + r * 1000000;
}

static void schedule_tick(void)
{
    const fwu_config_t *cfg = fwu_settings_config();
    int64_t now = esp_timer_get_time();

    if (!cfg->enabled)
    {
        FWU->next_check_us = 0;
        return;
    }

    if (!fwu_network_up())
    {
        FWU->net_up_us = 0;
        return;
    }

    if (FWU->net_up_us == 0)
    {
        FWU->net_up_us = now;

        if (FWU->next_check_us == 0 || !FWU->check.checked)
        {
            /* the first boot of a new image asks sooner: the view is empty
               and the user is looking at the page (2026-10-19) */
            bool after_update = FWU->rec_valid &&
                                FWU->rec.state == FW_JOB_REBOOTING;

            FWU->next_check_us = now + (int64_t)(after_update ? FWU_AFTER_UPDATE_S
                                                              : FWU_FIRST_CHECK_S) *
                                           1000000;
        }
    }

    if (FWU->next_check_us != 0 && now >= FWU->next_check_us && !FWU->job.active)
    {
        fwu_engine_lock();

        uint32_t id = ++FWU->job_seq;

        fwu_engine_unlock();
        fwu_run_check(id);
        FWU->next_check_us = esp_timer_get_time() + fwu_jittered_interval_us();

        if (!FWU->check.ok)
        {
            /* a failed check asks again sooner: 30 min, then the interval */
            FWU->next_check_us = esp_timer_get_time() + 30LL * 60 * 1000000;
        }
    }
}

static void milestone_tick(void)
{
    if (FWU->rec_valid && FWU->rec.state == FW_JOB_REBOOTING &&
        restart_tracker_settle(false))
    {
        fwu_verify_now("the run settled");
    }
}

static void fwu_task(void *arg)
{
    (void)arg;

    for (;;)
    {
        fwu_cmd_t cmd;
        TickType_t wait = pdMS_TO_TICKS(FWU->job.active ? 1000 : 5000);

        if (xQueueReceive(FWU->cmds, &cmd, wait) == pdTRUE)
        {
            if (FWU->stopped)
            {
                continue;
            }

            switch (cmd.type)
            {
                case FWU_CMD_CHECK:   fwu_run_check(cmd.job_id); break;
                case FWU_CMD_INSTALL: fwu_run_install(&cmd); break;
                default:          break;
            }

            ESP_LOGD(TAG, "task stack headroom %u B",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }

        if (FWU->stopped)
        {
            continue;
        }

        milestone_tick();
        schedule_tick();

        if (FWU->job.active)
        {
            fwu_renew_hold();
        }
    }
}

/* ---- lifecycle -------------------------------------------------------------------------- */

esp_err_t fw_update_init(void)
{
    static const log_descriptor_t LOG_DESC = { FWU_TAG, ESP_LOG_INFO };

    if (FWU->inited)
    {
        return ESP_OK;
    }

    log_manager_register(&LOG_DESC);
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    FWU->cmds = xQueueCreateStatic(4, sizeof(fwu_cmd_t), s_cmds_store, &s_cmds_buf);
    FWU->rec_valid = fwu_store_read(&FWU->rec);
    FWU->job_seq = FWU->rec_valid ? FWU->rec.job_id : 0;
    /* the slots, read in line: init runs on the boot task's internal stack
       (the NVS read above relies on the same); every later read is the
       confirm helper's, never the status builder's (2026-10-21) */
    fwu_slots_read_fn(&FWU->slots);
    FWU->inited = true;
    fwu_dongle_init();
    return fwu_settings_register();
}

esp_err_t fw_update_start(void)
{
    if (!FWU->inited || !fwu_settings_is_configured())
    {
        return ESP_ERR_INVALID_STATE; /* §4.3 step 5 */
    }

    if (FWU->started)
    {
        FWU->stopped = false;
        return ESP_OK;
    }

    fwu_reselect();
    (void)restart_tracker_add_pre_restart_hook(pre_restart_hook);
    s_task = xTaskCreateStatic(fwu_task, "fw_update", FWU_TASK_STACK, NULL,
                               FWU_TASK_PRIO, s_stack, &s_tcb);

    if (s_task == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    FWU->started = true;
    ESP_LOGI(TAG, "up: channel %s, every %lu h%s, base %s%s",
             fwu_channel_str(fwu_settings_config()->channel),
             (unsigned long)fwu_settings_config()->check_interval_h,
             fwu_settings_config()->enabled ? "" : " (scheduled check off)",
             fwu_settings_config()->base_url,
             FWU->rec_valid && FWU->rec.state == FW_JOB_REBOOTING
                 ? ", an update awaits its verify milestone" : "");
    return ESP_OK;
}

esp_err_t fw_update_stop(void)
{
    FWU->stopped = true;
    FWU->abort = true;
    return ESP_OK;
}

