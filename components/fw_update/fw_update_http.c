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
 * @file fw_update_http.c
 * @brief The five routes (fw_update/HTTP_API.md): GET /api/fw_update,
 *        POST /api/fw_update/check, /install, /abort (the release notes are the
 *        page's own fetch of the snapshot's notes_url since 2026-11-02).
 *        Own-routes pattern (standard §6); errors carry a snake_case `code`
 *        beside the house `error` text.
 */
#include "fw_update_private.h"

#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "cJSON.h"
#include "http_server_manager.h"

#define FWU_BODY_MAX 256

static esp_err_t send_json(httpd_req_t *req, cJSON *o, const char *status)
{
    char *js = cJSON_PrintUnformatted(o);

    cJSON_Delete(o);

    if (js == NULL)
    {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "serialize");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, status);
    httpd_resp_send(req, js, strlen(js));
    free(js);
    return ESP_OK;
}

/** The human sentence for a code. */
static const char *code_text(const char *code)
{
    static const struct
    {
        const char *code;
        const char *text;
    } TEXTS[] =
    {
        { "busy", "another update job is running" },
        { "busy_upload", "a firmware upload is in progress" },
        { "no_network", "the device is not connected to a network" },
        { "no_manifest", "no release list yet: check for updates first" },
        { "unknown_version", "no such release in the manifest" },
        { "not_visible", "that release is not on the selected channel" },
        { "yanked", "that release was withdrawn" },
        { "no_release", "the manifest offers nothing for this channel" },
        { "no_path", "the update needs a stepping stone this channel does not carry" },
        { "up_to_date", "the running firmware is the newest on this channel" },
        { "needs_confirm_wipe", "the new partition layout erases data: confirm the wipe" },
        { "layout_serial", "the release needs a USB flash: its partition layout cannot be reached by an update" },
        { "layout_unknown", "the manifest does not know this device's partition layout" },
        { "cellular_blocked", "the device is online through the mobile connection: firmware downloads need a WiFi network" },
        { "insufficient_ram", "not enough free internal RAM for a download now" },
        { "dongle_absent", "the ESPNetLink update relay is not built into this firmware" },
        { "no_dongle", "no ESPNetLink dongle is reachable" },
        { "needs_usb_flash", "this dongle firmware cannot be updated from here: flash it once over USB from a computer" },
        { "dongle_busy", "the dongle is receiving another image" },
        { "dongle_rejected", "the dongle refused the image" },
        { "push_failed", "the image could not be sent to the dongle" },
        { "timeout_reboot", "the dongle did not come back after the update" },
        { "version_mismatch_after", "the dongle came back with another version" },
        { "no_memory", "not enough free memory to stage the dongle image" },
        { "wrong_image", "the download is not a firmware image for this target" },
        { "bad_request", "bad request" },
        { "no_job", "no update job is running" },
        { "too_late", "the restart is scheduled: the job cannot be aborted" },
    };

    for (size_t i = 0; i < sizeof(TEXTS) / sizeof(TEXTS[0]); i++)
    {
        if (strcmp(TEXTS[i].code, code) == 0)
        {
            return TEXTS[i].text;
        }
    }

    return code;
}

static esp_err_t send_error(httpd_req_t *req, const char *status,
                            const char *code, const char *wipes)
{
    cJSON *o = cJSON_CreateObject();

    if (o == NULL)
    {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mem");
    }

    cJSON_AddStringToObject(o, "error", code_text(code));
    cJSON_AddStringToObject(o, "code", code);

    if (wipes != NULL)
    {
        cJSON *arr = cJSON_AddArrayToObject(o, "wipes");
        char tmp[FW_UPDATE_WIPES_LEN];
        char *save = NULL;

        strlcpy(tmp, wipes, sizeof(tmp));

        for (char *t = strtok_r(tmp, ",", &save); t != NULL;
             t = strtok_r(NULL, ",", &save))
        {
            cJSON_AddItemToArray(arr, cJSON_CreateString(t));
        }
    }

    return send_json(req, o, status);
}

const char *fwu_http_status_for(esp_err_t err, const char *code)
{
    (void)code;

    switch (err)
    {
        case ESP_ERR_INVALID_ARG:   return "400 Bad Request";
        case ESP_ERR_NOT_FOUND:     return "404 Not Found";
        case ESP_ERR_INVALID_STATE:
        case ESP_ERR_NOT_ALLOWED:
        case ESP_ERR_NO_MEM:        return "409 Conflict";
        default:                    return "500 Internal Server Error";
    }
}

/* ---- GET /api/fw_update ---------------------------------------------------------- */

static cJSON *target_self(const fw_update_status_t *s)
{
    cJSON *t = cJSON_CreateObject();

    cJSON_AddStringToObject(t, "installed", s->installed);
    cJSON_AddStringToObject(t, "build", s->build);
    cJSON_AddBoolToObject(t, "release_known", s->release_known);
    cJSON_AddBoolToObject(t, "dev_build", s->dev_build);
    cJSON_AddBoolToObject(t, "installed_yanked", s->installed_yanked);

    if (s->installed_yanked)
    {
        cJSON_AddStringToObject(t, "yank_reason", s->yank_reason);
    }

    if (s->have_latest)
    {
        cJSON *l = cJSON_AddObjectToObject(t, "latest");

        cJSON_AddStringToObject(l, "version", s->latest_version);
        cJSON_AddStringToObject(l, "date", s->latest_date);
        cJSON_AddStringToObject(l, "summary", s->latest_summary);
        cJSON_AddStringToObject(l, "notes_url", s->latest_notes_url);
        cJSON_AddNumberToObject(l, "size", s->latest_size);
    }
    else
    {
        cJSON_AddNullToObject(t, "latest");
    }

    cJSON_AddBoolToObject(t, "available", s->available);
    cJSON_AddBoolToObject(t, "installable", s->installable);
    cJSON_AddStringToObject(t, "reason", s->reason);

    if (s->target_version[0] != '\0')
    {
        cJSON *g = cJSON_AddObjectToObject(t, "target");
        cJSON *w = cJSON_CreateArray();
        char tmp[FW_UPDATE_WIPES_LEN];
        char *save = NULL;

        cJSON_AddStringToObject(g, "version", s->target_version);
        cJSON_AddBoolToObject(g, "step", s->step);
        cJSON_AddStringToObject(g, "verdict", s->verdict);
        strlcpy(tmp, s->wipes, sizeof(tmp));

        for (char *x = strtok_r(tmp, ",", &save); x != NULL;
             x = strtok_r(NULL, ",", &save))
        {
            cJSON_AddItemToArray(w, cJSON_CreateString(x));
        }

        cJSON_AddItemToObject(g, "wipes", w);
        cJSON_AddStringToObject(g, "layout_md5", s->target_layout_md5);
    }
    else
    {
        cJSON_AddNullToObject(t, "target");
    }

    cJSON *lay = cJSON_AddObjectToObject(t, "layout");

    cJSON_AddStringToObject(lay, "flashed_md5", s->flashed_md5);
    cJSON_AddStringToObject(lay, "build_md5", s->build_md5);
    return t;
}

/** targets.dongle (2026-10-19): the relay's view of the ESPNetLink. */
static cJSON *target_dongle(const fw_update_status_t *s)
{
    cJSON *t = cJSON_CreateObject();

    cJSON_AddBoolToObject(t, "present", s->dongle_present);
    cJSON_AddStringToObject(t, "verdict", s->dongle_verdict);

    if (!s->dongle_wired)
    {
        return t;
    }

    cJSON_AddBoolToObject(t, "reachable", s->dongle_reachable);
    cJSON_AddStringToObject(t, "transport", s->dongle_transport);

    cJSON *i = cJSON_AddObjectToObject(t, "installed");

    cJSON_AddStringToObject(i, "fw_version", s->dongle_installed);
    cJSON_AddStringToObject(i, "device_id", s->dongle_device_id);
    cJSON_AddNumberToObject(i, "api_level", s->dongle_api_level);
    cJSON_AddBoolToObject(i, "release_known", s->dongle_release_known);
    cJSON_AddBoolToObject(i, "dev_build", s->dongle_dev_build);

    if (s->dongle_checked)
    {
        cJSON *k = cJSON_AddObjectToObject(t, "last_check");

        cJSON_AddBoolToObject(k, "ok", s->dongle_check_error[0] == '\0');
        cJSON_AddStringToObject(k, "error", s->dongle_check_error);
    }
    else
    {
        cJSON_AddNullToObject(t, "last_check");
    }

    if (s->dongle_have_latest)
    {
        cJSON *l = cJSON_AddObjectToObject(t, "latest");

        cJSON_AddStringToObject(l, "version", s->dongle_latest_version);
        cJSON_AddStringToObject(l, "date", s->dongle_latest_date);
        cJSON_AddStringToObject(l, "summary", s->dongle_latest_summary);
        cJSON_AddStringToObject(l, "notes_url", s->dongle_latest_notes_url);
        cJSON_AddNumberToObject(l, "size", s->dongle_latest_size);
    }
    else
    {
        cJSON_AddNullToObject(t, "latest");
    }

    cJSON_AddBoolToObject(t, "available", s->dongle_available);
    cJSON_AddBoolToObject(t, "installable", s->dongle_installable);
    cJSON_AddStringToObject(t, "reason", s->dongle_reason);
    cJSON_AddStringToObject(t, "target_version", s->dongle_target_version);
    cJSON_AddBoolToObject(t, "cellular_needed", s->dongle_cellular_needed);

    if (s->dongle_have_last)
    {
        cJSON *l = cJSON_AddObjectToObject(t, "last_result");

        cJSON_AddNumberToObject(l, "id", s->dongle_last_id);
        cJSON_AddStringToObject(l, "from", s->dongle_last_from);
        cJSON_AddStringToObject(l, "to", s->dongle_last_to);
        cJSON_AddStringToObject(l, "state", fwu_job_state_str(s->dongle_last_state));
        cJSON_AddStringToObject(l, "error", s->dongle_last_error);
        cJSON_AddStringToObject(l, "detail", s->dongle_last_detail);
        cJSON_AddNumberToObject(l, "time", (double)s->dongle_last_time);
        cJSON_AddNumberToObject(l, "push_s", s->dongle_last_push_s);
    }
    else
    {
        cJSON_AddNullToObject(t, "last_result");
    }

    return t;
}

static cJSON *status_json(const fw_update_status_t *s)
{
    cJSON *o = cJSON_CreateObject();

    if (o == NULL)
    {
        return NULL;
    }

    cJSON_AddBoolToObject(o, "enabled", s->enabled);
    cJSON_AddStringToObject(o, "channel", s->channel);

    cJSON *c = cJSON_AddObjectToObject(o, "cellular");

    cJSON_AddStringToObject(c, "policy", s->cellular_policy);
    cJSON_AddStringToObject(c, "uplink", s->uplink);
    cJSON_AddBoolToObject(c, "is_cellular", s->is_cellular);

    if (s->checked)
    {
        cJSON *k = cJSON_AddObjectToObject(o, "last_check");

        cJSON_AddNumberToObject(k, "time", (double)s->check_time);
        cJSON_AddBoolToObject(k, "ok", s->check_ok);
        cJSON_AddStringToObject(k, "error", s->check_error);
        cJSON_AddBoolToObject(k, "etag_hit", s->etag_hit);
        cJSON_AddNumberToObject(k, "age_s", s->check_age_s);
        cJSON_AddNumberToObject(k, "next_in_s", s->next_in_s);
        cJSON_AddNumberToObject(k, "count", s->checks);
    }
    else
    {
        cJSON_AddNullToObject(o, "last_check");
        cJSON_AddNumberToObject(o, "next_in_s", s->next_in_s);
    }

    cJSON *targets = cJSON_AddObjectToObject(o, "targets");

    cJSON_AddItemToObject(targets, "self", target_self(s));

    cJSON_AddItemToObject(targets, "dongle", target_dongle(s));

    if (s->job_active || s->job_state != FW_JOB_IDLE)
    {
        cJSON *j = cJSON_AddObjectToObject(o, "job");

        cJSON_AddNumberToObject(j, "id", s->job_id);
        cJSON_AddBoolToObject(j, "active", s->job_active);
        cJSON_AddStringToObject(j, "target",
                                (s->job_target == FW_UPDATE_TARGET_DONGLE) ? "dongle"
                                                                            : "self");
        cJSON_AddStringToObject(j, "version", s->job_version);
        cJSON_AddStringToObject(j, "state", fwu_job_state_str(s->job_state));
        cJSON_AddNumberToObject(j, "received", s->job_received);
        cJSON_AddNumberToObject(j, "total", s->job_total);
        cJSON_AddNumberToObject(j, "pct",
                                (s->job_total > 0)
                                    ? (double)(100ULL * s->job_received / s->job_total)
                                    : 0);
        cJSON_AddNumberToObject(j, "attempt", s->job_attempt);
        cJSON_AddStringToObject(j, "error", s->job_error);
        cJSON_AddStringToObject(j, "detail", s->job_detail);
        cJSON_AddNumberToObject(j, "elapsed_s", s->job_elapsed_s);
        cJSON_AddStringToObject(j, "transport", s->job_transport);

        cJSON *b = cJSON_AddObjectToObject(j, "backup");

        cJSON_AddBoolToObject(b, "sd", s->job_backup_sd);
    }
    else
    {
        cJSON_AddNullToObject(o, "job");
    }

    if (s->have_last)
    {
        cJSON *l = cJSON_AddObjectToObject(o, "last_result");

        cJSON_AddNumberToObject(l, "id", s->last_id);
        cJSON_AddStringToObject(l, "target",
                                (s->last_target == FW_UPDATE_TARGET_DONGLE) ? "dongle"
                                                                             : "self");
        cJSON_AddStringToObject(l, "from", s->last_from);
        cJSON_AddStringToObject(l, "to", s->last_to);
        cJSON_AddStringToObject(l, "state", fwu_job_state_str(s->last_state));
        cJSON_AddStringToObject(l, "by", s->last_by);
        cJSON_AddStringToObject(l, "error", s->last_error);
        cJSON_AddNumberToObject(l, "time", (double)s->last_time);
    }
    else
    {
        cJSON_AddNullToObject(o, "last_result");
    }

    cJSON *r = cJSON_AddObjectToObject(o, "rollback");

    cJSON_AddBoolToObject(r, "pending_verify", s->pending_verify);
    cJSON_AddStringToObject(r, "running", s->running);
    cJSON_AddStringToObject(r, "previous", s->previous);
    cJSON_AddStringToObject(r, "ota_state", s->ota_state);
    cJSON_AddBoolToObject(r, "bootloader_rollback", s->bootloader_rollback);
    cJSON_AddNumberToObject(r, "bootloader_version", s->bootloader_version);
    return o;
}

static esp_err_t get_handler(httpd_req_t *req)
{
    static fw_update_status_t st EXT_RAM_BSS_ATTR; /* 1 KB: never a stack frame */
    static SemaphoreHandle_t lock;
    static StaticSemaphore_t lock_buf;

    if (lock == NULL)
    {
        lock = xSemaphoreCreateMutexStatic(&lock_buf);
    }

    xSemaphoreTake(lock, portMAX_DELAY);

    esp_err_t err = fw_update_status(&st);
    cJSON *o = (err == ESP_OK) ? status_json(&st) : NULL;

    xSemaphoreGive(lock);

    if (o == NULL)
    {
        return send_error(req, "503 Service Unavailable", "busy", NULL);
    }

    return send_json(req, o, "200 OK");
}

/* ---- the three POSTs --------------------------------------------------------------- */

static esp_err_t check_handler(httpd_req_t *req)
{
    uint32_t id = 0;
    esp_err_t err = fw_update_check_now(&id);

    if (err == ESP_ERR_INVALID_STATE)
    {
        return send_error(req, "409 Conflict", "busy", NULL);
    }

    if (err != ESP_OK)
    {
        return send_error(req, "500 Internal Server Error", esp_err_to_name(err), NULL);
    }

    cJSON *o = cJSON_CreateObject();

    cJSON_AddNumberToObject(o, "job_id", id);
    cJSON_AddStringToObject(o, "state", "checking");
    return send_json(req, o, "202 Accepted");
}

static cJSON *read_body(httpd_req_t *req)
{
    char buf[FWU_BODY_MAX + 1];
    size_t got = 0;

    if (req->content_len == 0)
    {
        return cJSON_CreateObject();
    }

    if (req->content_len > FWU_BODY_MAX)
    {
        return NULL;
    }

    while (got < req->content_len)
    {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);

        if (r <= 0)
        {
            return NULL;
        }

        got += (size_t)r;
    }

    buf[got] = '\0';
    return cJSON_Parse(buf);
}

static esp_err_t install_handler(httpd_req_t *req)
{
    cJSON *body = read_body(req);

    if (!cJSON_IsObject(body))
    {
        cJSON_Delete(body);
        return send_error(req, "400 Bad Request", "bad_request", NULL);
    }

    const cJSON *t = cJSON_GetObjectItemCaseSensitive(body, "target");
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(body, "version");
    fw_update_target_t target = FW_UPDATE_TARGET_SELF;
    uint32_t flags = 0;
    char version[FW_UPDATE_VER_LEN] = "";

    if (cJSON_IsString(t) && strcmp(t->valuestring, "dongle") == 0)
    {
        target = FW_UPDATE_TARGET_DONGLE;
    }
    else if (cJSON_IsString(t) && strcmp(t->valuestring, "self") != 0)
    {
        cJSON_Delete(body);
        return send_error(req, "400 Bad Request", "bad_request", NULL);
    }

    if (cJSON_IsString(v))
    {
        strlcpy(version, v->valuestring, sizeof(version));
    }

    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body, "confirm_wipe")))
    {
        flags |= FW_UPDATE_F_CONFIRM_WIPE;
    }

    /* the bench's knob on the dongle staging buffer (0 = none) */
    const cJSON *cap = cJSON_GetObjectItemCaseSensitive(body, "max_stage_bytes");

    fwu_dongle_set_stage_cap(cJSON_IsNumber(cap) ? (uint32_t)cap->valuedouble : 0);
    cJSON_Delete(body);

    uint32_t id = 0;
    char why[FW_UPDATE_CODE_LEN] = "";
    bool already = false;
    static fw_update_status_t st EXT_RAM_BSS_ATTR;
    esp_err_t err = fw_update_install(target, version[0] ? version : NULL, flags,
                                      &id, why, &already);

    if (err != ESP_OK)
    {
        const char *wipes = NULL;

        if (strcmp(why, "needs_confirm_wipe") == 0 &&
            fw_update_status(&st) == ESP_OK)
        {
            wipes = st.wipes;
        }

        return send_error(req, fwu_http_status_for(err, why),
                          why[0] ? why : esp_err_to_name(err), wipes);
    }

    cJSON *o = cJSON_CreateObject();

    (void)fw_update_status(&st);

    if (already)
    {
        cJSON_AddBoolToObject(o, "already", true);
        cJSON_AddStringToObject(o, "installed",
                                (target == FW_UPDATE_TARGET_DONGLE) ? st.dongle_installed
                                                                    : st.installed);
        return send_json(req, o, "200 OK");
    }

    cJSON_AddNumberToObject(o, "job_id", id);
    cJSON_AddStringToObject(o, "state", fwu_job_state_str(st.job_state));
    cJSON_AddStringToObject(o, "target",
                            (target == FW_UPDATE_TARGET_DONGLE) ? "dongle" : "self");
    cJSON_AddStringToObject(o, "version", st.job_version);
    cJSON_AddStringToObject(o, "from",
                            (target == FW_UPDATE_TARGET_DONGLE) ? st.dongle_installed
                                                                : st.installed);
    return send_json(req, o, "202 Accepted");
}

static esp_err_t abort_handler(httpd_req_t *req)
{
    static fw_update_status_t st EXT_RAM_BSS_ATTR;
    esp_err_t err = fw_update_abort();

    if (err == ESP_ERR_NOT_FOUND)
    {
        return send_error(req, "409 Conflict", "no_job", NULL);
    }

    if (err == ESP_ERR_INVALID_STATE)
    {
        return send_error(req, "409 Conflict", "too_late", NULL);
    }

    (void)fw_update_status(&st);

    cJSON *o = cJSON_CreateObject();

    cJSON_AddBoolToObject(o, "aborted", true);
    cJSON_AddNumberToObject(o, "job_id", st.job_id);
    return send_json(req, o, "200 OK");
}

esp_err_t fw_update_register_http(void)
{
    static const httpd_uri_t URIS[] =
    {
        { .uri = "/api/fw_update", .method = HTTP_GET, .handler = get_handler },
        { .uri = "/api/fw_update/check", .method = HTTP_POST,
          .handler = check_handler },
        { .uri = "/api/fw_update/install", .method = HTTP_POST,
          .handler = install_handler },
        { .uri = "/api/fw_update/abort", .method = HTTP_POST,
          .handler = abort_handler },
    };

    return http_server_manager_register_handlers(URIS,
                                                 sizeof(URIS) / sizeof(URIS[0]));
}
