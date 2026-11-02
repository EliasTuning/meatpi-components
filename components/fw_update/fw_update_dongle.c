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
 * @file fw_update_dongle.c
 * @brief The WiCAN PRO updates the ESPNetLink dongle (2026-10-19,
 *        internal/TASK_fw_update.md section 6): the dongle's own manifest
 *        (`espnetlink/firmware/manifest.json`) is read with the WiCAN's
 *        channel, the offered release is shown beside this device's, and
 *        on request (fw_update_dongle_job.c) the image is downloaded into PSRAM, verified (size,
 *        sha256, an ESP32-S3 application image whose project name is the
 *        dongle's, the manifest's version), pushed into the dongle's own
 *        upload route over the USB link or its WiFi, and the dongle's
 *        restart and new version are waited for. Staging in RAM keeps the
 *        dongle's single httpd worker out of every network stall: nothing
 *        reaches it before the whole image passed its checks here.
 *
 * The relay knows no dongle component: the composition root wires the
 * endpoint, the hold, the restart notice and the identify from
 * espnetlink_link (fw_update_dongle_ops_t).
 */
#include "fw_update_dongle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"

#include "dev_status_manager.h"
#include "http_client_manager.h"

static const char *TAG = FWU_TAG;

/* the relay's state: one PSRAM object shared with the job file */
static fwd_state_t s_d EXT_RAM_BSS_ATTR;

fwd_state_t *fwd_state(void)
{
    return &s_d;
}

/* ---- helpers ------------------------------------------------------------------------- */

bool fwd_endpoint_now(fw_update_dongle_endpoint_t *ep)
{
    const fw_update_dongle_ops_t *ops = FWU->dongle;

    memset(ep, 0, sizeof(*ep));
    return ops != NULL && ops->endpoint != NULL && ops->endpoint(ep) == ESP_OK;
}

bool fwd_version_equal(const char *a, const char *b)
{
    return a != NULL && b != NULL && a[0] != '\0' && strcmp(a, b) == 0;
}

/** The dongle has an upload route: api 7 and up, confirmed by a probe. */
static bool dongle_pushable(const fw_update_dongle_endpoint_t *ep)
{
    if (ep->api_level > 0 && ep->api_level < FWD_MIN_API_LEVEL)
    {
        return false;
    }

    if (FWD->ota_probed && fwd_version_equal(FWD->ota_probed_fw, ep->fw_version))
    {
        return FWD->ota_capable;
    }

    return true; /* unknown yet: the install probes before it pushes */
}

/** The selection against the dongle's view. The dongle reports no layout
 *  id (api 9), so the layout verdict of the manifest is not applied: a
 *  release is installable when its channel offers it. Under the lock. */
void fwd_select(void)
{
    if (!FWD->view_valid)
    {
        memset(&FWD->sel, 0, sizeof(FWD->sel));
        FWD->sel.target = -1;
        FWD->sel.latest = -1;
        FWD->sel.me = -1;
        return;
    }

    fwu_select(&FWD->view, fwu_settings_config()->channel,
               FWD->ep_valid ? FWD->ep.fw_version : "", "", &FWD->sel);

    if (FWD->sel.target >= 0 && FWD->sel.verdict == FWU_LAYOUT_UNKNOWN)
    {
        FWD->sel.verdict = FWU_LAYOUT_NONE;
        FWD->sel.installable = FWD->sel.available;

        if (strcmp(FWD->sel.reason, "layout_unknown") == 0)
        {
            FWD->sel.reason[0] = '\0';
        }
    }
}

/** The one word the row shows. Under the lock. */
static const char *verdict_word(const fw_update_dongle_endpoint_t *ep)
{
    if (!ep->present)
    {
        return "not_present";
    }

    if (!ep->reachable)
    {
        return "unreachable";
    }

    if (!dongle_pushable(ep))
    {
        return "needs_usb_flash";
    }

    if (!FWD->view_valid)
    {
        return "no_manifest";
    }

    if (FWD->sel.available && FWD->sel.installable)
    {
        return "update_available";
    }

    if (FWD->sel.available)
    {
        return "layout_mismatch";
    }

    if (strcmp(FWD->sel.reason, "no_path") == 0 ||
        strcmp(FWD->sel.reason, "unknown_version") == 0)
    {
        return FWD->sel.reason;
    }

    return "up_to_date";
}

/** GET http://<host>/api/ota/status: 1 idle/ready/failed, 2 receiving,
 *  0 no route (404), -1 no answer. */
int fwd_probe_ota(const char *host)
{
    char url[80];
    http_client_response_t resp;

    snprintf(url, sizeof(url), "http://%s/api/ota/status", host);

    http_client_request_t req =
    {
        .url          = url,
        .method       = HTTP_CLIENT_GET,
        .timeout_ms   = FWD_PROBE_TIMEOUT_MS,
        .max_response = 512,
    };

    memset(&resp, 0, sizeof(resp));

    if (http_client_manager_request(&req, &resp) != ESP_OK)
    {
        http_client_manager_free(&resp);
        return -1;
    }

    int rc;

    if (resp.status_code == 404)
    {
        rc = 0;
    }
    else if (resp.status_code == 200 && resp.data != NULL &&
             strstr(resp.data, "\"receiving\"") != NULL)
    {
        rc = 2;
    }
    else
    {
        rc = (resp.status_code == 200) ? 1 : -1;
    }

    http_client_manager_free(&resp);
    return rc;
}

void fwd_note_probe(const fw_update_dongle_endpoint_t *ep, int rc)
{
    if (rc < 0)
    {
        return;
    }

    FWD->ota_probed = true;
    FWD->ota_capable = rc != 0;
    strlcpy(FWD->ota_probed_fw, ep->fw_version, sizeof(FWD->ota_probed_fw));
}

/* ---- the check ---------------------------------------------------------------------- */

void fwu_dongle_init(void)
{
    FWD->sel.target = -1;
    FWD->sel.latest = -1;
    FWD->sel.me = -1;
}

void fwu_dongle_set_stage_cap(uint32_t bytes)
{
    FWD->stage_cap = bytes;
}

void fwu_dongle_check(const fwu_config_t *cfg)
{
    fw_update_dongle_endpoint_t ep;
    fwu_fetch_result_t r;
    char etag[64];

    if (FWU->dongle == NULL)
    {
        return;
    }

    bool have = fwd_endpoint_now(&ep);

    fwu_engine_lock();
    FWD->ep = ep;
    FWD->ep_valid = have;
    strlcpy(etag, FWD->etag, sizeof(etag));
    fwu_engine_unlock();

    if (!have || !ep.present)
    {
        return; /* no dongle: its manifest is not fetched */
    }

    esp_err_t err = fwu_fetch_manifest(cfg, FWU_DONGLE_DEVICE_TYPE, etag, &r);
    int probe = -1;

    if (ep.reachable && (!FWD->ota_probed ||
                         !fwd_version_equal(FWD->ota_probed_fw, ep.fw_version)))
    {
        probe = fwd_probe_ota(ep.host);
    }

    fwu_engine_lock();
    FWD->check.checked = true;
    FWD->check.count++;
    FWD->check.mono_us = esp_timer_get_time();
    FWD->check.time = (int64_t)time(NULL);
    FWD->check.etag_hit = false;
    FWD->check.error[0] = '\0';

    if (FWD->check.time < 1600000000LL)
    {
        FWD->check.time = 0;
    }

    if (err == ESP_OK && r.etag_hit)
    {
        FWD->check.ok = FWD->view_valid;
        FWD->check.etag_hit = true;

        if (!FWD->view_valid)
        {
            FWD->etag[0] = '\0';
            strlcpy(FWD->check.error, "etag_without_view", sizeof(FWD->check.error));
        }
    }
    else if (err == ESP_OK)
    {
        char why[64];
        fwu_parse_rc_t rc = fwu_manifest_parse(r.body, r.len, FWU_DONGLE_DEVICE_TYPE,
                                               cfg->base_is_http, &FWD->view, why,
                                               sizeof(why));

        FWD->view_valid = rc == FWU_PARSE_OK;
        FWD->check.ok = FWD->view_valid;

        if (FWD->view_valid)
        {
            strlcpy(FWD->etag, r.etag, sizeof(FWD->etag));
            ESP_LOGI(TAG, "dongle manifest: %u releases, newest %s",
                     (unsigned)FWD->view.n, FWD->view.rel[0].version_str);
        }
        else
        {
            FWD->etag[0] = '\0';
            strlcpy(FWD->check.error,
                    (rc == FWU_PARSE_WRONG_DEVICE) ? "manifest_wrong_device"
                    : (rc == FWU_PARSE_EMPTY)      ? "manifest_empty"
                                                   : "manifest_invalid",
                    sizeof(FWD->check.error));
            ESP_LOGW(TAG, "dongle manifest refused: %s (%s)", FWD->check.error, why);
        }
    }
    else
    {
        FWD->check.ok = false;
        strlcpy(FWD->check.error, r.code[0] ? r.code : "fetch_failed",
                sizeof(FWD->check.error));
    }

    free(r.body);
    fwd_note_probe(&ep, probe);
    fwd_select();

    if (FWD->check.ok && FWD->sel.available)
    {
        ESP_LOGI(TAG, "dongle update available: %s (running %s)",
                 FWD->view.rel[FWD->sel.target].version_str,
                 ep.fw_version[0] ? ep.fw_version : "?");
    }

    fwu_engine_unlock();
}

bool fwd_stage_fits(size_t size, char *detail)
{
    size_t total_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    if (detail != NULL)
    {
        detail[0] = '\0';
    }

    if (FWD->stage_cap != 0 && size > FWD->stage_cap)
    {
        if (detail != NULL)
        {
            snprintf(detail, FW_UPDATE_DETAIL_LEN, "a %lu byte image over the %lu byte cap",
                     (unsigned long)size, (unsigned long)FWD->stage_cap);
        }

        return false;
    }

    if (size > FWD_STAGE_MAX || total_free < size + FWD_STAGE_RESERVE)
    {
        if (detail != NULL)
        {
            snprintf(detail, FW_UPDATE_DETAIL_LEN,
                     "%lu bytes free in PSRAM (largest %lu) for a %lu byte image plus the reserve",
                     (unsigned long)total_free, (unsigned long)largest, (unsigned long)size);
        }

        return false;
    }

    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (p == NULL)
    {
        if (detail != NULL)
        {
            snprintf(detail, FW_UPDATE_DETAIL_LEN,
                     "no contiguous %lu bytes in PSRAM (%lu free, largest %lu)",
                     (unsigned long)size, (unsigned long)total_free, (unsigned long)largest);
        }

        return false;
    }

    heap_caps_free(p);
    return true;
}

/* ---- the synchronous refusals ---------------------------------------------------------- */

static esp_err_t refuse(char *why, const char *code, esp_err_t err)
{
    if (why != NULL)
    {
        strlcpy(why, code, FW_UPDATE_CODE_LEN);
    }

    return err;
}

esp_err_t fwu_dongle_validate_install(const char *version, uint32_t flags,
                                      char *why, bool *already,
                                      char *chosen, size_t chosen_cap)
{
    if (FWU->dongle == NULL)
    {
        return refuse(why, "dongle_absent", ESP_ERR_NOT_ALLOWED);
    }

    if (!FWD->ep_valid || !FWD->ep.present)
    {
        return refuse(why, "no_dongle", ESP_ERR_NOT_ALLOWED);
    }

    if (!FWD->view_valid)
    {
        return refuse(why, "no_manifest", ESP_ERR_INVALID_STATE);
    }

    if (!dongle_pushable(&FWD->ep))
    {
        return refuse(why, "needs_usb_flash", ESP_ERR_NOT_ALLOWED);
    }

    int idx;

    if (version == NULL || version[0] == '\0')
    {
        if (!FWD->sel.available)
        {
            if (strcmp(FWD->sel.reason, "up_to_date") == 0)
            {
                if (already != NULL)
                {
                    *already = true;
                }

                return ESP_OK;
            }

            bool missing = strcmp(FWD->sel.reason, "no_release") == 0 ||
                           strcmp(FWD->sel.reason, "unknown_version") == 0;

            return refuse(why, FWD->sel.reason,
                          missing ? ESP_ERR_NOT_FOUND : ESP_ERR_NOT_ALLOWED);
        }

        idx = FWD->sel.target;
    }
    else
    {
        idx = fwu_find_version(&FWD->view, version);

        if (idx < 0)
        {
            return refuse(why, "unknown_version", ESP_ERR_NOT_FOUND);
        }

        if (!fwu_channel_visible(fwu_settings_config()->channel,
                                 FWD->view.rel[idx].channel))
        {
            return refuse(why, "not_visible", ESP_ERR_NOT_FOUND);
        }

        if (FWD->view.rel[idx].yanked)
        {
            return refuse(why, "yanked", ESP_ERR_NOT_FOUND);
        }

        if (fwd_version_equal(FWD->view.rel[idx].fw_version, FWD->ep.fw_version))
        {
            if (already != NULL)
            {
                *already = true;
            }

            return ESP_OK;
        }
    }

    const fwu_release_t *rel = &FWD->view.rel[idx];
    bool cellular = false;

    (void)fwu_engine_uplink(&cellular);

    /* never over the dongle's LTE (Ali, 2026-10-19): the image waits for WiFi */
    if (cellular)
    {
        return refuse(why, "cellular_blocked", ESP_ERR_NOT_ALLOWED);
    }

    dev_status_memory_t mem;

    if (dev_status_manager_memory(&mem) == ESP_OK &&
        (mem.internal.free < FWU_RAM_FREE_MIN ||
         mem.internal.largest_block < FWU_RAM_LARGEST_MIN))
    {
        return refuse(why, "insufficient_ram", ESP_ERR_NOT_ALLOWED);
    }

    char detail[FW_UPDATE_DETAIL_LEN];

    if (!fwd_stage_fits(rel->size, detail))
    {
        ESP_LOGW(TAG, "dongle install %s refused: %s", rel->version_str, detail);
        return refuse(why, "no_memory", ESP_ERR_NO_MEM);
    }

    strlcpy(chosen, rel->version_str, chosen_cap);
    return ESP_OK;
}

/* ---- the snapshot ---------------------------------------------------------------- */

void fwu_dongle_status_fill(fw_update_status_t *o)
{
    fw_update_dongle_endpoint_t ep;

    o->dongle_wired = FWU->dongle != NULL;

    if (!o->dongle_wired)
    {
        strlcpy(o->dongle_verdict, "not_built", sizeof(o->dongle_verdict));
        return;
    }

    if (!fwd_endpoint_now(&ep))
    {
        ep = FWD->ep;
    }

    o->dongle_present = ep.present;
    o->dongle_reachable = ep.reachable;
    strlcpy(o->dongle_transport, ep.via, sizeof(o->dongle_transport));
    strlcpy(o->dongle_device_id, ep.device_id, sizeof(o->dongle_device_id));
    strlcpy(o->dongle_installed, ep.fw_version, sizeof(o->dongle_installed));
    o->dongle_api_level = ep.api_level;
    o->dongle_checked = FWD->check.checked;
    strlcpy(o->dongle_check_error, FWD->check.error, sizeof(o->dongle_check_error));
    strlcpy(o->dongle_verdict, verdict_word(&ep), sizeof(o->dongle_verdict));

    if (FWD->view_valid)
    {
        o->dongle_release_known = FWD->sel.installed_known;
        o->dongle_dev_build = FWD->sel.dev_build;
        o->dongle_available = FWD->sel.available;
        o->dongle_installable = FWD->sel.available && FWD->sel.installable &&
                                strcmp(o->dongle_verdict, "update_available") == 0;
        strlcpy(o->dongle_reason, FWD->sel.reason, sizeof(o->dongle_reason));

        if (FWD->sel.latest >= 0)
        {
            const fwu_release_t *l = &FWD->view.rel[FWD->sel.latest];

            o->dongle_have_latest = true;
            strlcpy(o->dongle_latest_version, l->version_str,
                    sizeof(o->dongle_latest_version));
            strlcpy(o->dongle_latest_date, l->date, sizeof(o->dongle_latest_date));
            strlcpy(o->dongle_latest_summary, l->summary,
                    sizeof(o->dongle_latest_summary));
            o->dongle_latest_size = l->size;

            if (l->notes[0] == '\0' ||
                !fwu_compose_url(fwu_settings_config()->base_url, l->notes,
                                 o->dongle_latest_notes_url,
                                 sizeof(o->dongle_latest_notes_url)))
            {
                o->dongle_latest_notes_url[0] = '\0';
            }
        }

        if (FWD->sel.target >= 0)
        {
            strlcpy(o->dongle_target_version, FWD->view.rel[FWD->sel.target].version_str,
                    sizeof(o->dongle_target_version));
        }
    }
    else
    {
        strlcpy(o->dongle_reason, "no_manifest", sizeof(o->dongle_reason));
    }

    bool cellular = false;

    (void)fwu_engine_uplink(&cellular);
    o->dongle_cellular_needed = cellular;

    if (FWD->last.valid)
    {
        o->dongle_have_last = true;
        o->dongle_last_id = FWD->last.id;
        strlcpy(o->dongle_last_from, FWD->last.from, sizeof(o->dongle_last_from));
        strlcpy(o->dongle_last_to, FWD->last.to, sizeof(o->dongle_last_to));
        o->dongle_last_state = FWD->last.state;
        strlcpy(o->dongle_last_error, FWD->last.error, sizeof(o->dongle_last_error));
        strlcpy(o->dongle_last_detail, FWD->last.detail, sizeof(o->dongle_last_detail));
        o->dongle_last_time = FWD->last.time;
        o->dongle_last_push_s = FWD->last.push_s;
    }
}
