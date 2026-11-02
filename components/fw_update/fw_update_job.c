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
 * @file fw_update_job.c
 * @brief The job runners on the component's task: the manifest check (this
 *        device's and, with a relay wired, the dongle's), the install of
 *        this device (the manifest again, the release re-validated, the
 *        image through ota_manager's pipe, the record, the restart) and
 *        the release notes. Split out of fw_update.c on 2026-10-19.
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

/* ---- the check ---------------------------------------------------------------------- */

void fwu_run_check(uint32_t job_id)
{
    fwu_fetch_result_t r;
    const fwu_config_t *cfg = fwu_settings_config();
    char etag[64];

    fwu_engine_lock();

    /* an ended install keeps its final state through a check: the UI shows
       the outcome until the next install (bench 2026-10-19: a scheduled
       check right behind a failed job wiped it from the snapshot) */
    fwu_job_t ended;
    bool keep = !FWU->job.active && FWU->job.state != FW_JOB_IDLE &&
                FWU->job.version[0] != '\0';

    if (keep)
    {
        ended = FWU->job;
    }

    fwu_job_begin(job_id, FW_UPDATE_TARGET_SELF, NULL, 0, FW_JOB_CHECKING);
    strlcpy(etag, FWU->etag, sizeof(etag));
    fwu_engine_unlock();

    esp_err_t err = fwu_fetch_manifest(cfg, CONFIG_WICAN_DEVICE_TYPE, etag, &r);

    fwu_engine_lock();
    FWU->check.checked = true;
    FWU->check.count++;
    FWU->check.mono_us = esp_timer_get_time();
    FWU->check.time = (int64_t)time(NULL);
    FWU->check.etag_hit = false;
    FWU->check.error[0] = '\0';

    if (FWU->check.time < 1600000000LL)
    {
        FWU->check.time = 0;
    }

    if (err == ESP_OK && r.etag_hit)
    {
        FWU->check.ok = FWU->view_valid;
        FWU->check.etag_hit = true;

        if (!FWU->view_valid)
        {
            /* a 304 against a view we lost (a parse failure): ask again whole */
            FWU->etag[0] = '\0';
            strlcpy(FWU->check.error, "etag_without_view", sizeof(FWU->check.error));
        }
    }
    else if (err == ESP_OK)
    {
        char why[64];
        fwu_parse_rc_t rc = fwu_manifest_parse(r.body, r.len, CONFIG_WICAN_DEVICE_TYPE,
                                               cfg->base_is_http, &FWU->view, why,
                                               sizeof(why));

        FWU->view_valid = rc == FWU_PARSE_OK;
        FWU->check.ok = FWU->view_valid;

        if (FWU->view_valid)
        {
            strlcpy(FWU->etag, r.etag, sizeof(FWU->etag));
            ESP_LOGI(TAG, "manifest: %u releases%s, newest %s",
                     (unsigned)FWU->view.n, FWU->view.dropped ? " (some dropped)" : "",
                     FWU->view.rel[0].version_str);
        }
        else
        {
            FWU->etag[0] = '\0';
            strlcpy(FWU->check.error,
                    (rc == FWU_PARSE_WRONG_DEVICE) ? "manifest_wrong_device"
                    : (rc == FWU_PARSE_EMPTY)      ? "manifest_empty"
                                                   : "manifest_invalid",
                    sizeof(FWU->check.error));
            ESP_LOGW(TAG, "manifest refused: %s (%s)", FWU->check.error, why);
        }
    }
    else
    {
        FWU->check.ok = false;
        strlcpy(FWU->check.error, r.code[0] ? r.code : "fetch_failed",
                sizeof(FWU->check.error));
    }

    free(r.body);
    fwu_reselect();

    if (FWU->check.ok && FWU->sel.available)
    {
        ESP_LOGI(TAG, "update available: %s (%s)%s",
                 FWU->view.rel[FWU->sel.target].version_str,
                 fwu_layout_str(FWU->sel.verdict), FWU->sel.step ? ", a step" : "");
    }

    fwu_engine_unlock();

    /* the dongle's manifest rides the same check (2026-10-19) */
    fwu_dongle_check(cfg);

    fwu_engine_lock();

    if (keep)
    {
        FWU->job = ended;
    }
    else
    {
        FWU->job.state = FW_JOB_IDLE;
        FWU->job.active = false;
    }

    /* any check satisfies the schedule: the next one after the interval, or
       half an hour after a failure (a check asked for by hand used to leave
       the scheduled one due, so it fired right behind the next job) */
    FWU->next_check_us = esp_timer_get_time() +
                         (FWU->check.ok ? fwu_jittered_interval_us()
                                        : 30LL * 60 * 1000000);
    fwu_engine_unlock();
    fwu_notify();
}

/* ---- the install ----------------------------------------------------------------- */

static void progress_cb(size_t received, size_t total)
{
    fwu_engine_lock();
    FWU->job.received = (uint32_t)received;
    FWU->job.total = (uint32_t)total;

    if (received >= total && total > 0)
    {
        FWU->job.state = FW_JOB_VERIFYING;
    }

    fwu_engine_unlock();
}

void fwu_renew_hold(void)
{
    int64_t now = esp_timer_get_time();

    if (now - FWU->hold_us >= FWU_HOLD_RENEW_US)
    {
        FWU->hold_us = now;
        (void)sleep_manager_hold(FWU_HOLD_MINUTES); /* refused when nothing counts */
    }
}

/** The install: the manifest again (a yank must beat a cached view), the
 *  release re-validated, the image through the pipe, the record, the restart. */
void fwu_run_install(const fwu_cmd_t *cmd)
{
    if (cmd->target == FW_UPDATE_TARGET_DONGLE)
    {
        fwu_dongle_install(cmd);
        return;
    }

    const fwu_config_t *cfg = fwu_settings_config();
    fwu_fetch_result_t r;
    char code[FW_UPDATE_CODE_LEN] = "";
    char detail[FW_UPDATE_DETAIL_LEN] = "";

    fwu_engine_lock();
    fwu_job_begin(cmd->job_id, FW_UPDATE_TARGET_SELF, cmd->version, cmd->flags,
              FW_JOB_CHECKING);
    FWU->abort = false;
    fwu_engine_unlock();
    FWU->hold_us = 0;
    fwu_renew_hold();

    esp_err_t err = fwu_fetch_manifest(cfg, CONFIG_WICAN_DEVICE_TYPE, NULL, &r);

    fwu_engine_lock();

    if (err != ESP_OK)
    {
        fwu_job_fail(FW_JOB_FAILED, "manifest_fetch_failed", r.code);
        fwu_engine_unlock();
        free(r.body);
        fwu_notify();
        return;
    }

    char why[64];
    fwu_parse_rc_t rc = fwu_manifest_parse(r.body, r.len, CONFIG_WICAN_DEVICE_TYPE,
                                           cfg->base_is_http, &FWU->view, why,
                                           sizeof(why));

    free(r.body);
    FWU->view_valid = rc == FWU_PARSE_OK;
    strlcpy(FWU->etag, FWU->view_valid ? r.etag : "", sizeof(FWU->etag));
    fwu_reselect();

    int idx = FWU->view_valid ? fwu_find_version(&FWU->view, cmd->version) : -1;

    if (idx < 0)
    {
        fwu_job_fail(FW_JOB_FAILED, "unknown_version",
                 FWU->view_valid ? "the release left the manifest" : why);
        fwu_engine_unlock();
        fwu_notify();
        return;
    }

    memcpy(&FWU->job_rel, &FWU->view.rel[idx], sizeof(FWU->job_rel));

    if (FWU->job_rel.yanked)
    {
        fwu_job_fail(FW_JOB_FAILED, "yanked", FWU->job_rel.yank_reason);
        fwu_engine_unlock();
        fwu_notify();
        return;
    }

    char md5[33] = "";
    const char *wipes = "";

    (void)partition_migrate_flashed_md5(md5);

    fwu_layout_t verdict = fwu_layout_verdict(&FWU->job_rel, md5, &wipes);

    if (verdict == FWU_LAYOUT_SERIAL || verdict == FWU_LAYOUT_UNKNOWN)
    {
        fwu_job_fail(FW_JOB_FAILED, (verdict == FWU_LAYOUT_SERIAL) ? "layout_serial"
                                                               : "layout_unknown",
                 "the release's partition layout cannot be reached by OTA");
        fwu_engine_unlock();
        fwu_notify();
        return;
    }

    if (verdict == FWU_LAYOUT_WIPE && !(cmd->flags & FW_UPDATE_F_CONFIRM_WIPE))
    {
        fwu_job_fail(FW_JOB_FAILED, "needs_confirm_wipe", wipes);
        fwu_engine_unlock();
        fwu_notify();
        return;
    }

    FWU->job.total = FWU->job_rel.size;
    fwu_engine_unlock();

    if (verdict == FWU_LAYOUT_WIPE)
    {
        /* the copies on the device are refreshed before anything formats.
           The job stays `checking` until the step is done, so a status
           that says `downloading` carries the step's result: on 2026-10-31
           the bench read `downloading` with backup.sd still false while
           the refresh (half a second) was running, because the state used
           to move first. */
        bool internal = false;
        bool sd = false;

        (void)config_backup_refresh_copies(15000, &internal, &sd);
        fwu_engine_lock();
        FWU->job.backup_sd = sd;
        fwu_engine_unlock();
    }

    fwu_engine_lock();
    FWU->job.state = FW_JOB_DOWNLOADING;
    fwu_engine_unlock();
    fwu_notify();

    uint8_t attempt = 0;

    err = fwu_fetch_image(&FWU->job_rel, cfg, progress_cb, &FWU->abort, &attempt,
                          code, detail);

    fwu_engine_lock();
    FWU->job.attempt = attempt;

    if (err != ESP_OK)
    {
        fwu_job_fail(strcmp(code, "aborted") == 0 ? FW_JOB_ABORTED : FW_JOB_FAILED,
                 code, detail);
        fwu_engine_unlock();
        fwu_notify();
        return;
    }

    /* the image is READY: the record first, then the restart */
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

    memset(&FWU->rec, 0, sizeof(FWU->rec));
    FWU->rec.job_id = FWU->job.id;
    FWU->rec.state = FW_JOB_REBOOTING;
    FWU->rec.target = (uint8_t)FWU->job.target;
    FWU->rec.slot_subtype = (next != NULL) ? next->subtype : 0xFF;
    FWU->rec.flags = (verdict != FWU_LAYOUT_NONE) ? FWU_FLAG_LAYOUT_CHANGED : 0;
    strlcpy(FWU->rec.from_version, dev_status_manager_app_version(),
            sizeof(FWU->rec.from_version));
    strlcpy(FWU->rec.to_version, FWU->job_rel.fw_version, sizeof(FWU->rec.to_version));
    FWU->rec_valid = fwu_run_internal(fwu_write_record_fn, NULL, 5000) == ESP_OK;
    FWU->job.state = FW_JOB_REBOOTING;
    FWU->job.received = FWU->job.total;
    fwu_engine_unlock();
    fwu_notify();

    ESP_LOGI(TAG, "job %lu: %s installed into %s, restarting",
             (unsigned long)FWU->job.id, FWU->job_rel.version_str,
             (next != NULL) ? next->label : "?");

    /* internal: the restart runs the shutdown handlers */
    if (xTaskCreate(fwu_restart_task, "fwu_restart", 3072,
                    (void *)(uintptr_t)FWU->job.id, 5, NULL) != pdPASS)
    {
        fwu_restart_task((void *)(uintptr_t)FWU->job.id);
    }
}


