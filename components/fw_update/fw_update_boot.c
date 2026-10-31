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
 * @file fw_update_boot.c
 * @brief The flash side: the job record in NVS, the internal-stack helper,
 *        the slots read (otadata and the bootloader description, 2026-10-21)
 *        and the boot-path check that turns the crash-loop brake's park
 *        verdict into a rollback to the previous slot. Runs on the boot
 *        task (internal stack) before settings and before main_park_check():
 *        it may use NVS, esp_ota and the restart tracker, nothing else.
 */
#include "fw_update_private.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_bootloader_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "flash_jobs.h"
#include "ota_manager.h"
#include "restart_tracker.h"

static const char *TAG = FWU_TAG;

#define FWU_NVS_NS  "fw_update"
#define FWU_NVS_KEY "job"

/* ---- the record store ----------------------------------------------------------- */

bool fwu_store_read(fwu_record_t *out)
{
    nvs_handle_t h;
    size_t n = sizeof(*out);

    if (out == NULL || nvs_open(FWU_NVS_NS, NVS_READONLY, &h) != ESP_OK)
    {
        return false;
    }

    esp_err_t err = nvs_get_blob(h, FWU_NVS_KEY, out, &n);

    nvs_close(h);
    return err == ESP_OK && n == sizeof(*out) && fwu_record_valid(out);
}

esp_err_t fwu_store_write(fwu_record_t *rec)
{
    nvs_handle_t h;

    if (rec == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    rec->time = (int64_t)time(NULL);

    if (rec->time < 1600000000LL)
    {
        rec->time = 0; /* no clock yet */
    }

    fwu_record_seal(rec);

    esp_err_t err = nvs_open(FWU_NVS_NS, NVS_READWRITE, &h);

    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_set_blob(h, FWU_NVS_KEY, rec, sizeof(*rec));

    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }

    nvs_close(h);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "job record not stored: %s", esp_err_to_name(err));
    }

    return err;
}

esp_err_t fwu_store_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(FWU_NVS_NS, NVS_READWRITE, &h);

    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_erase_key(h, FWU_NVS_KEY);

    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        err = ESP_OK;
    }

    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }

    nvs_close(h);
    return err;
}

/* ---- the internal-stack helper ---------------------------------------------------- */

/* The payloads run on an internal stack whoever the caller is (the
 * fw_update task lives on a PSRAM stack; otadata and the NVS record are
 * flash): through the shared flash worker since 2026-11-01 (it was a
 * one-shot 4 KB internal task of this file's own, `fwu_flash`). The gate
 * keeps one payload at a time, with the callers' wait; the job itself runs
 * to completion (flash_jobs_run blocks until it returns). */
typedef struct
{
    void (*fn)(void *);
    void *arg;
} run_ctx_t;

static esp_err_t run_job(void *p)
{
    run_ctx_t *ctx = p;

    ctx->fn(ctx->arg);
    return ESP_OK;
}

esp_err_t fwu_run_internal(void (*fn)(void *), void *arg, uint32_t wait_ms)
{
    static SemaphoreHandle_t gate;     /* one helper at a time */
    static StaticSemaphore_t gate_buf; /* internal: FreeRTOS object */
    run_ctx_t ctx = { .fn = fn, .arg = arg };

    if (fn == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (gate == NULL)
    {
        gate = xSemaphoreCreateMutexStatic(&gate_buf);
    }

    if (xSemaphoreTake(gate, pdMS_TO_TICKS(wait_ms)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = flash_jobs_run(run_job, &ctx);

    xSemaphoreGive(gate);
    return err;
}

/* ---- the slots --------------------------------------------------------------------- */

void fwu_slots_read_fn(void *arg)
{
    fwu_slots_t *s = arg;
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t st;
    esp_bootloader_desc_t bl;

    if (s == NULL)
    {
        return;
    }

    memset(s, 0, sizeof(*s));
    strlcpy(s->running, (running != NULL) ? running->label : "", sizeof(s->running));
    strlcpy(s->previous, (next != NULL) ? next->label : "", sizeof(s->previous));

    /* the otadata read maps flash: this stack must be internal (the header) */
    if (running != NULL && esp_ota_get_state_partition(running, &st) == ESP_OK)
    {
        strlcpy(s->ota_state, ota_manager_img_state_str((int)st), sizeof(s->ota_state));
        s->pending_verify = st == ESP_OTA_IMG_PENDING_VERIFY || st == ESP_OTA_IMG_NEW;
    }
    else
    {
        strlcpy(s->ota_state, "unknown", sizeof(s->ota_state));
    }

    /* the bootloader's description is a flash read: the same rule */
    if (esp_ota_get_bootloader_description(NULL, &bl) == ESP_OK)
    {
        s->bootloader_version = bl.version;
        /* CONFIG_BOOTLOADER_PROJECT_VER 2 = built with the rollback option
           (2026-10-18); the field bootloaders say 1 */
        s->bootloader_rollback = bl.version >= 2;
    }

    s->valid = true;
}

/* ---- the boot check ---------------------------------------------------------------- */

static void finish(fwu_record_t *rec, fw_update_job_state_t state, uint8_t by)
{
    rec->state = (uint8_t)state;
    rec->by = by;
    (void)fwu_store_write(rec);
}

esp_err_t fw_update_boot_check(void)
{
    fwu_record_t rec;
    fwu_boot_in_t in;
    restart_tracker_record_t latest;
    restart_tracker_brake_t brake;
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();

    memset(&in, 0, sizeof(in));
    memset(&brake, 0, sizeof(brake));
    in.have_record = fwu_store_read(&rec);

    if (!in.have_record)
    {
        return ESP_OK; /* no update in flight: the usual boot */
    }

    in.rec_state = rec.state;
    in.rec_slot = rec.slot_subtype;
    in.rec_flags = rec.flags;
    in.running_slot = (running != NULL) ? running->subtype : 0xFF;
    in.last_invalid_known = invalid != NULL;
    in.last_invalid_slot = (invalid != NULL) ? invalid->subtype : 0;

    if (restart_tracker_get_latest_record(&latest) == ESP_OK)
    {
        in.latest_planned = latest.was_planned != 0;
        in.latest_reason = latest.planned_reason;
    }

    if (restart_tracker_get_brake(&brake) == ESP_OK)
    {
        in.brake_parks = brake.verdict == RESTART_TRACKER_BOOT_PARK ||
                         brake.verdict == RESTART_TRACKER_BOOT_PARK_BARE;
    }

    switch (fwu_boot_decide(&in))
    {
        case FWU_BOOT_WRITE_ROLLED_BACK_BL:
            ESP_LOGW(TAG, "update job %lu to %s: the bootloader fell back to "
                     "%s (the new image never confirmed itself)",
                     (unsigned long)rec.job_id, rec.to_version,
                     (running != NULL) ? running->label : "?");
            finish(&rec, FW_JOB_ROLLED_BACK, FWU_BY_BOOTLOADER);
            break;

        case FWU_BOOT_WRITE_BOOT_FAILED:
            ESP_LOGW(TAG, "update job %lu to %s: the new image did not boot, "
                     "running %s", (unsigned long)rec.job_id, rec.to_version,
                     (running != NULL) ? running->label : "?");
            finish(&rec, FW_JOB_BOOT_FAILED, FWU_BY_NONE);
            break;

        case FWU_BOOT_WRITE_VERIFIED:
            ESP_LOGI(TAG, "update job %lu to %s verified (the previous run "
                     "restarted on purpose)", (unsigned long)rec.job_id,
                     rec.to_version);
            finish(&rec, FW_JOB_VERIFIED, FWU_BY_NONE);
            (void)esp_ota_mark_app_valid_cancel_rollback();
            break;

        case FWU_BOOT_ROLLBACK_APP:
        {
            esp_err_t err = esp_ota_mark_app_invalid_rollback();

            if (err != ESP_OK)
            {
                ESP_LOGW(TAG, "update %s crashed %u times before it settled "
                         "and cannot be rolled back (%s): parking",
                         rec.to_version, (unsigned)brake.streak,
                         esp_err_to_name(err));
                break; /* main_park_check() takes it from here */
            }

            ESP_LOGW(TAG, "update %s crashed %u times before it settled: "
                     "rolling back to %s", rec.to_version,
                     (unsigned)brake.streak, rec.from_version);
            finish(&rec, FW_JOB_ROLLED_BACK, FWU_BY_APP);
            printf("WICAN ROLLBACK job=%lu from=%s to=%s\n",
                   (unsigned long)rec.job_id, rec.to_version, rec.from_version);
            vTaskDelay(pdMS_TO_TICKS(200)); /* let the lines out */
            restart_tracker_restart(RESTART_TRACKER_PLANNED_REASON_OTA_ROLLBACK,
                                    RESTART_TRACKER_SOURCE_BOOT, rec.job_id);
            break; /* not reached */
        }

        case FWU_BOOT_KEEP_PENDING:
            ESP_LOGI(TAG, "update job %lu to %s runs, not yet verified "
                     "(the milestone is %u s up, or a planned restart)",
                     (unsigned long)rec.job_id, rec.to_version,
                     (unsigned)RESTART_TRACKER_SETTLE_S);
            break;

        default:
            break;
    }

    return ESP_OK;
}
