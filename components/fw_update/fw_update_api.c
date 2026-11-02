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
 * @file fw_update_api.c
 * @brief The public API: the check and install requests with their
 *        synchronous refusals, abort, the snapshot, the
 *        milestone by hand, the record erase and the providers the
 *        composition root wires. Split out of fw_update.c on 2026-10-19.
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


/* ---- the API --------------------------------------------------------------------------- */

esp_err_t fw_update_check_now(uint32_t *job_id)
{
    if (!FWU->started)
    {
        return ESP_ERR_INVALID_STATE;
    }

    fwu_engine_lock();

    if (FWU->job.active)
    {
        bool checking = FWU->job.state == FW_JOB_CHECKING &&
                        FWU->job.version[0] == '\0';

        if (job_id != NULL)
        {
            *job_id = FWU->job.id;
        }

        fwu_engine_unlock();
        return checking ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    fwu_cmd_t cmd = { .type = FWU_CMD_CHECK, .job_id = ++FWU->job_seq };

    if (job_id != NULL)
    {
        *job_id = cmd.job_id;
    }

    fwu_engine_unlock();
    return (xQueueSend(FWU->cmds, &cmd, 0) == pdTRUE) ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t refuse(char *why, const char *code, esp_err_t err)
{
    if (why != NULL)
    {
        strlcpy(why, code, FW_UPDATE_CODE_LEN);
    }

    return err;
}

/** The dongle target (2026-10-19): the shared busy and network checks,
 *  the relay's own refusals, then the command. */
static esp_err_t install_dongle(const char *version, uint32_t flags,
                                uint32_t *job_id, char *why, bool *already)
{
    char chosen[FW_UPDATE_VER_LEN] = "";

    if (!FWU->started || FWU->stopped)
    {
        return refuse(why, "busy", ESP_ERR_INVALID_STATE);
    }

    fwu_engine_lock();

    if (FWU->job.active)
    {
        bool same = FWU->job.target == FW_UPDATE_TARGET_DONGLE &&
                    FWU->job.version[0] != '\0' &&
                    (version == NULL || version[0] == '\0' ||
                     strcmp(FWU->job.version, version) == 0);

        if (same)
        {
            if (job_id != NULL)
            {
                *job_id = FWU->job.id;
            }

            fwu_engine_unlock();
            return ESP_OK; /* idempotent: that job */
        }

        fwu_engine_unlock();
        return refuse(why, "busy", ESP_ERR_INVALID_STATE);
    }

    if (!fwu_network_up())
    {
        fwu_engine_unlock();
        return refuse(why, "no_network", ESP_ERR_INVALID_STATE);
    }

    esp_err_t err = fwu_dongle_validate_install(version, flags, why, already,
                                                chosen, sizeof(chosen));

    if (err != ESP_OK || (already != NULL && *already))
    {
        fwu_engine_unlock();
        return err;
    }

    fwu_cmd_t cmd = { .type = FWU_CMD_INSTALL, .target = FW_UPDATE_TARGET_DONGLE,
                      .flags = flags, .job_id = ++FWU->job_seq };

    strlcpy(cmd.version, chosen, sizeof(cmd.version));
    fwu_job_begin(cmd.job_id, FW_UPDATE_TARGET_DONGLE, chosen, flags,
                  FW_JOB_CHECKING);

    if (job_id != NULL)
    {
        *job_id = cmd.job_id;
    }

    fwu_engine_unlock();

    if (xQueueSend(FWU->cmds, &cmd, 0) != pdTRUE)
    {
        fwu_engine_lock();
        fwu_job_fail(FW_JOB_FAILED, "busy", "the command queue is full");
        fwu_engine_unlock();
        return refuse(why, "busy", ESP_ERR_INVALID_STATE);
    }

    return ESP_OK;
}

esp_err_t fw_update_install(fw_update_target_t target, const char *version,
                            uint32_t flags, uint32_t *job_id, char *why,
                            bool *already)
{
    if (why != NULL)
    {
        why[0] = '\0';
    }

    if (already != NULL)
    {
        *already = false;
    }

    if (target == FW_UPDATE_TARGET_DONGLE)
    {
        return install_dongle(version, flags, job_id, why, already);
    }

    if (target != FW_UPDATE_TARGET_SELF)
    {
        return refuse(why, "bad_request", ESP_ERR_INVALID_ARG);
    }

    if (!FWU->started || FWU->stopped)
    {
        return refuse(why, "busy", ESP_ERR_INVALID_STATE);
    }

    fwu_engine_lock();

    if (FWU->job.active)
    {
        /* an install job carries its version; a check does not */
        bool same = FWU->job.version[0] != '\0' &&
                    (version == NULL || strcmp(FWU->job.version, version) == 0);

        if (same)
        {
            if (job_id != NULL)
            {
                *job_id = FWU->job.id;
            }

            fwu_engine_unlock();
            return ESP_OK; /* idempotent: that job */
        }

        fwu_engine_unlock();
        return refuse(why, "busy", ESP_ERR_INVALID_STATE);
    }

    if (!FWU->view_valid)
    {
        fwu_engine_unlock();
        return refuse(why, "no_manifest", ESP_ERR_INVALID_STATE);
    }

    if (!fwu_network_up())
    {
        fwu_engine_unlock();
        return refuse(why, "no_network", ESP_ERR_INVALID_STATE);
    }

    int idx;

    if (version == NULL || version[0] == '\0')
    {
        if (!FWU->sel.available)
        {
            fwu_engine_unlock();

            if (strcmp(FWU->sel.reason, "up_to_date") == 0)
            {
                if (already != NULL)
                {
                    *already = true;
                }

                return ESP_OK;
            }

            bool missing = strcmp(FWU->sel.reason, "no_release") == 0 ||
                           strcmp(FWU->sel.reason, "unknown_version") == 0;

            return refuse(why, FWU->sel.reason,
                          missing ? ESP_ERR_NOT_FOUND : ESP_ERR_NOT_ALLOWED);
        }

        idx = FWU->sel.target;
    }
    else
    {
        idx = fwu_find_version(&FWU->view, version);

        if (idx < 0)
        {
            fwu_engine_unlock();
            return refuse(why, "unknown_version", ESP_ERR_NOT_FOUND);
        }

        if (!fwu_channel_visible(fwu_settings_config()->channel,
                                 FWU->view.rel[idx].channel))
        {
            fwu_engine_unlock();
            return refuse(why, "not_visible", ESP_ERR_NOT_FOUND);
        }

        if (FWU->view.rel[idx].yanked)
        {
            fwu_engine_unlock();
            return refuse(why, "yanked", ESP_ERR_NOT_FOUND);
        }

        if (strcmp(FWU->view.rel[idx].fw_version,
                   dev_status_manager_app_version()) == 0)
        {
            fwu_engine_unlock();

            if (already != NULL)
            {
                *already = true;
            }

            return ESP_OK;
        }
    }

    const fwu_release_t *rel = &FWU->view.rel[idx];
    char md5[33] = "";
    const char *wipes = "";

    (void)partition_migrate_flashed_md5(md5);

    fwu_layout_t verdict = fwu_layout_verdict(rel, md5, &wipes);

    if (verdict == FWU_LAYOUT_SERIAL || verdict == FWU_LAYOUT_UNKNOWN)
    {
        fwu_engine_unlock();
        return refuse(why, (verdict == FWU_LAYOUT_SERIAL) ? "layout_serial"
                                                          : "layout_unknown",
                      ESP_ERR_NOT_ALLOWED);
    }

    if (verdict == FWU_LAYOUT_WIPE && !(flags & FW_UPDATE_F_CONFIRM_WIPE))
    {
        fwu_engine_unlock();
        return refuse(why, "needs_confirm_wipe", ESP_ERR_NOT_ALLOWED);
    }

    bool cellular = false;

    (void)fwu_engine_uplink(&cellular);

    /* never over the dongle's LTE (Ali, 2026-10-19): the image waits for WiFi */
    if (cellular)
    {
        fwu_engine_unlock();
        return refuse(why, "cellular_blocked", ESP_ERR_NOT_ALLOWED);
    }

    dev_status_memory_t mem;

    if (dev_status_manager_memory(&mem) == ESP_OK &&
        (mem.internal.free < FWU_RAM_FREE_MIN ||
         mem.internal.largest_block < FWU_RAM_LARGEST_MIN))
    {
        fwu_engine_unlock();
        return refuse(why, "insufficient_ram", ESP_ERR_NOT_ALLOWED);
    }

    fwu_cmd_t cmd = { .type = FWU_CMD_INSTALL, .target = (uint8_t)target, .flags = flags,
                  .job_id = ++FWU->job_seq };

    strlcpy(cmd.version, rel->version_str, sizeof(cmd.version));
    /* the job is visible at once: a second POST sees it */
    fwu_job_begin(cmd.job_id, target, rel->version_str, flags, FW_JOB_CHECKING);
    FWU->job.total = rel->size;

    if (job_id != NULL)
    {
        *job_id = cmd.job_id;
    }

    fwu_engine_unlock();

    if (xQueueSend(FWU->cmds, &cmd, 0) != pdTRUE)
    {
        fwu_engine_lock();
        fwu_job_fail(FW_JOB_FAILED, "busy", "the command queue is full");
        fwu_engine_unlock();
        return refuse(why, "busy", ESP_ERR_INVALID_STATE);
    }

    return ESP_OK;
}

esp_err_t fw_update_abort(void)
{
    fwu_engine_lock();

    if (!FWU->job.active)
    {
        fwu_engine_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    if (FWU->job.state == FW_JOB_REBOOTING || FWU->job.state == FW_JOB_PUSHING)
    {
        fwu_engine_unlock();
        return ESP_ERR_INVALID_STATE; /* too late */
    }

    FWU->abort = true;
    fwu_engine_unlock();
    return ESP_OK;
}

esp_err_t fw_update_status(fw_update_status_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!FWU->inited)
    {
        return ESP_ERR_INVALID_STATE;
    }

    return fwu_status_build(out);
}

esp_err_t fw_update_confirm(void)
{
    if (!FWU->inited)
    {
        return ESP_ERR_INVALID_STATE;
    }

    fwu_verify_now("confirmed by hand");
    return ESP_OK;
}

esp_err_t fw_update_forget(void)
{
    if (!FWU->inited)
    {
        return ESP_ERR_INVALID_STATE;
    }

    fwu_engine_lock();
    FWU->rec_valid = false;
    memset(&FWU->rec, 0, sizeof(FWU->rec));
    fwu_engine_unlock();
    return fwu_run_internal(fwu_erase_record_fn, NULL, 5000);
}

esp_err_t fw_update_set_event_cb(fw_update_event_cb_t cb)
{
    FWU->event_cb = cb;
    return ESP_OK;
}

esp_err_t fw_update_set_uplink_provider(fw_update_uplink_fn fn)
{
    FWU->uplink = fn;
    return ESP_OK;
}

esp_err_t fw_update_set_dongle_ops(const fw_update_dongle_ops_t *ops)
{
    FWU->dongle = ops;
    return ESP_OK;
}
