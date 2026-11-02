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
 * @file fw_update_status.c
 * @brief The snapshot GET /api/fw_update, the console and the HA push read:
 *        the settings as applied, the uplink, the last check, this device
 *        against the manifest, the job, the last recorded job and the
 *        rollback state of the slots. RAM only: the HA poster calls this
 *        from a PSRAM stack, and a flash read there aborts the firmware
 *        (the field crash of 2026-10-21), so the slots come from the record
 *        the boot task and the confirm helper read (fwu_slots_read_fn).
 */
#include "fw_update_private.h"

#include <string.h>
#include <time.h>

#include "esp_ota_ops.h"
#include "esp_timer.h"

#include "dev_status_manager.h"
#include "partition_migrate.h"

static void fill_self(fw_update_status_t *o, const fwu_manifest_t *m,
                      bool view_valid, const fwu_selection_t *sel)
{
    strlcpy(o->installed, dev_status_manager_app_version(), sizeof(o->installed));
    strlcpy(o->build, dev_status_manager_app_build(), sizeof(o->build));
    (void)partition_migrate_flashed_md5(o->flashed_md5);
    (void)partition_migrate_build_md5(o->build_md5);

    if (!view_valid)
    {
        strlcpy(o->reason, "no_manifest", sizeof(o->reason));
        return;
    }

    o->release_known = sel->installed_known;
    o->dev_build = sel->dev_build;
    o->installed_yanked = sel->installed_yanked;

    if (sel->me >= 0 && m->rel[sel->me].yanked)
    {
        strlcpy(o->yank_reason, m->rel[sel->me].yank_reason,
                sizeof(o->yank_reason));
    }

    if (sel->latest >= 0)
    {
        const fwu_release_t *l = &m->rel[sel->latest];

        o->have_latest = true;
        strlcpy(o->latest_version, l->version_str, sizeof(o->latest_version));
        strlcpy(o->latest_date, l->date, sizeof(o->latest_date));
        strlcpy(o->latest_summary, l->summary, sizeof(o->latest_summary));
        o->latest_size = l->size;

        if (l->notes[0] == '\0' ||
            !fwu_compose_url(fwu_settings_config()->base_url, l->notes,
                             o->latest_notes_url, sizeof(o->latest_notes_url)))
        {
            o->latest_notes_url[0] = '\0';
        }
    }

    o->available = sel->available;
    o->installable = sel->installable;
    strlcpy(o->reason, sel->reason, sizeof(o->reason));

    if (sel->target >= 0)
    {
        const fwu_release_t *t = &m->rel[sel->target];

        strlcpy(o->target_version, t->version_str, sizeof(o->target_version));
        strlcpy(o->target_layout_md5, t->layout_md5, sizeof(o->target_layout_md5));
        o->step = sel->step;
        strlcpy(o->verdict, fwu_layout_str(sel->verdict), sizeof(o->verdict));
        strlcpy(o->wipes, (sel->wipes != NULL) ? sel->wipes : "", sizeof(o->wipes));
    }
}

/** The slots as the boot task (init) or the confirm helper last read them
 *  on an internal stack: a copy, no flash (under the engine lock). */
static void fill_rollback(fw_update_status_t *o)
{
    const fwu_slots_t *s = &FWU->slots;

    strlcpy(o->running, s->running, sizeof(o->running));
    strlcpy(o->previous, s->previous, sizeof(o->previous));
    strlcpy(o->ota_state, s->valid ? s->ota_state : "unknown", sizeof(o->ota_state));
    o->pending_verify = s->pending_verify;
    o->bootloader_version = s->bootloader_version;
    o->bootloader_rollback = s->bootloader_rollback;
}

esp_err_t fwu_status_build(fw_update_status_t *out)
{
    const fwu_config_t *cfg = fwu_settings_config();
    bool view_valid = false;
    bool rec_valid = false;
    bool cellular = false;
    int64_t now_us = esp_timer_get_time();

    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->enabled = cfg->enabled;
    strlcpy(out->channel, fwu_channel_str(cfg->channel), sizeof(out->channel));
    strlcpy(out->cellular_policy, "wifi_only", sizeof(out->cellular_policy));

    fwu_engine_lock();

    strlcpy(out->uplink, fwu_engine_uplink(&cellular), sizeof(out->uplink));
    out->is_cellular = cellular;

    const fwu_check_t *chk = fwu_engine_check();

    out->checked = chk->checked;
    out->check_time = chk->time;
    out->check_ok = chk->ok;
    out->etag_hit = chk->etag_hit;
    out->checks = chk->count;
    strlcpy(out->check_error, chk->error, sizeof(out->check_error));
    out->check_age_s = chk->checked ? (uint32_t)((now_us - chk->mono_us) / 1000000)
                                    : 0;
    out->next_in_s = fwu_engine_next_check_s();

    fill_self(out, fwu_engine_manifest(&view_valid), view_valid,
              fwu_engine_selection());

    const fwu_job_t *job = fwu_engine_job();

    out->job_active = job->active;
    out->job_id = job->id;
    out->job_target = job->target;
    strlcpy(out->job_version, job->version, sizeof(out->job_version));
    out->job_state = job->state;
    out->job_received = job->received;
    out->job_total = job->total;
    out->job_attempt = job->attempt;
    strlcpy(out->job_error, job->error, sizeof(out->job_error));
    strlcpy(out->job_detail, job->detail, sizeof(out->job_detail));
    out->job_elapsed_s = (job->t_start_us != 0)
                             ? (uint32_t)((now_us - job->t_start_us) / 1000000) : 0;
    out->job_backup_sd = job->backup_sd;
    strlcpy(out->job_transport, job->transport, sizeof(out->job_transport));

    const fwu_record_t *rec = fwu_engine_record(&rec_valid);

    if (rec_valid)
    {
        const esp_partition_t *running = esp_ota_get_running_partition();

        out->have_last = true;
        out->last_id = rec->job_id;
        out->last_target = (fw_update_target_t)rec->target;
        strlcpy(out->last_from, rec->from_version, sizeof(out->last_from));
        strlcpy(out->last_to, rec->to_version, sizeof(out->last_to));
        out->last_state = (fw_update_job_state_t)rec->state;
        strlcpy(out->last_by, fwu_by_str(rec->by), sizeof(out->last_by));
        strlcpy(out->last_error, rec->error, sizeof(out->last_error));
        out->last_time = rec->time;

        /* REBOOTING with the new slot running = the verify is pending */
        if (rec->state == FW_JOB_REBOOTING && running != NULL &&
            running->subtype == rec->slot_subtype)
        {
            out->last_state = FW_JOB_PENDING_VERIFY;
        }
    }

    fwu_dongle_status_fill(out);
    fill_rollback(out);
    fwu_engine_unlock();
    return ESP_OK;
}
