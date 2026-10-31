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
 * @file espnetlink_link_pair.c
 * @brief The pairing store: the dongle's AP becomes a wifi_manager STA
 *        candidate (first free fallback slot, or the slot already holding
 *        that SSID, trusted) and our own ssid/device_id/enabled are set.
 *        Every write is change-guarded (§11) so the boot-time re-read of
 *        an unchanged key costs zero flash writes and no reboot.
 *
 * settings_manager_set() used to walk LittleFS on the CALLER's stack (flash
 * reads with the cache disabled): a PSRAM-stack caller tripped
 * esp_task_stack_is_sane_cache_disabled (bench 2026-08-24, first pairing
 * attempt), so the store ran on a short-lived internal-RAM worker task of
 * its own. Since 2026-10-31 the Settings Manager runs its storage on the
 * shared flash worker whoever the caller is, and since 2026-11-01 the
 * store runs on the caller's stack (link task, HTTP or CLI handler), one
 * store at a time behind a mutex.
 */
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "settings_manager.h"

#include "espnetlink_link.h"
#include "espnetlink_link_private.h"

static const char *TAG = "espnetlink";

#define PAIR_MAX_FALLBACKS 5

static const char *item_str(const cJSON *o, const char *k)
{
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);

    return (cJSON_IsString(i) && i->valuestring != NULL) ? i->valuestring
                                                         : "";
}

/* set only when different; returns true when the object was touched */
static bool set_str(cJSON *o, const char *k, const char *v)
{
    if (strcmp(item_str(o, k), v) == 0 &&
        cJSON_GetObjectItemCaseSensitive(o, k) != NULL)
    {
        return false;
    }

    cJSON *n = cJSON_CreateString(v);

    if (cJSON_GetObjectItemCaseSensitive(o, k) != NULL)
    {
        cJSON_ReplaceItemInObjectCaseSensitive(o, k, n);
    }
    else
    {
        cJSON_AddItemToObject(o, k, n);
    }
    return true;
}

static bool set_bool(cJSON *o, const char *k, bool v)
{
    const cJSON *cur = cJSON_GetObjectItemCaseSensitive(o, k);

    if (cJSON_IsBool(cur) && cJSON_IsTrue(cur) == v)
    {
        return false;
    }

    cJSON *n = cJSON_CreateBool(v);

    if (cur != NULL)
    {
        cJSON_ReplaceItemInObjectCaseSensitive(o, k, n);
    }
    else
    {
        cJSON_AddItemToObject(o, k, n);
    }
    return true;
}

static esp_err_t persist(const char *name, cJSON *o, bool *changed)
{
    char errbuf[96] = "";
    bool did = false;
    esp_err_t err = settings_manager_set(name, o, errbuf, sizeof(errbuf),
                                         &did);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "pair: %s set: %s %s", name, esp_err_to_name(err),
                 errbuf);
        espnl_status_set_last_error(errbuf[0] != '\0'
                                        ? errbuf
                                        : esp_err_to_name(err));
        return err;
    }
    espnl_status_set_last_error("");
    if (did && changed != NULL)
    {
        *changed = true;
    }
    return ESP_OK;
}

/* The dongle is reached over STA: a WiFi mode without one would make
 * the pairing dead on arrival. ap -> apsta, off -> sta; sta/apsta kept. */
static bool ensure_sta_mode(cJSON *wm)
{
    const char *mode = item_str(wm, "mode");

    if (strcmp(mode, "ap") == 0)
    {
        ESP_LOGW(TAG, "pair: wifi mode ap -> apsta (STA needed for the "
                 "dongle uplink)");
        return set_str(wm, "mode", "apsta");
    }
    if (strcmp(mode, "off") == 0)
    {
        ESP_LOGW(TAG, "pair: wifi mode off -> sta (STA needed for the "
                 "dongle uplink)");
        return set_str(wm, "mode", "sta");
    }
    return false;
}

typedef struct
{
    const char *ssid;
    const char *password;
    const char *device_id;
    int         slot;
    bool        changed;
    esp_err_t   err;
} store_job_t;

static esp_err_t store_impl(store_job_t *job)
{
    const char *ssid = job->ssid;
    const char *password = job->password;
    const char *device_id = job->device_id;
    int *slot_out = &job->slot;
    bool *changed = &job->changed;

    *changed = false;

    cJSON *wm = NULL;
    esp_err_t err = settings_manager_get("wifi_manager", &wm);

    if (err != ESP_OK)
    {
        return err;
    }

    int slot = -1;
    char key[32];

    /* already the primary? nothing to write on the wifi side */
    if (strcmp(item_str(wm, "sta_ssid"), ssid) == 0)
    {
        slot = 0;
    }
    else
    {
        int first_free = -1;

        for (int i = 1; i <= PAIR_MAX_FALLBACKS; i++)
        {
            snprintf(key, sizeof(key), "fallback%d_ssid", i);
            const char *cur = item_str(wm, key);

            if (strcmp(cur, ssid) == 0)
            {
                slot = i;
                break;
            }
            if (cur[0] == '\0' && first_free < 0)
            {
                first_free = i;
            }
        }
        if (slot < 0)
        {
            slot = first_free;
        }
    }

    if (slot < 0)
    {
        cJSON_Delete(wm);
        ESP_LOGE(TAG, "pair: no free wifi_manager fallback slot");
        return ESP_ERR_NO_MEM;
    }

    bool touched = ensure_sta_mode(wm);

    if (slot > 0)
    {
        snprintf(key, sizeof(key), "fallback%d_ssid", slot);
        touched |= set_str(wm, key, ssid);
        snprintf(key, sizeof(key), "fallback%d_password", slot);
        touched |= set_str(wm, key, password);
        snprintf(key, sizeof(key), "fallback%d_trusted", slot);
        touched |= set_bool(wm, key, true);
    }

    if (touched)
    {
        err = persist("wifi_manager", wm, changed);
    }
    cJSON_Delete(wm);
    if (err != ESP_OK)
    {
        return err;
    }

    cJSON *me = NULL;

    err = settings_manager_get("espnetlink", &me);
    if (err != ESP_OK)
    {
        return err;
    }

    touched = set_bool(me, "enabled", true);
    touched |= set_str(me, "ssid", ssid);
    if (device_id != NULL && device_id[0] != '\0')
    {
        touched |= set_str(me, "device_id", device_id);
    }
    if (touched)
    {
        err = persist("espnetlink", me, changed);
    }
    cJSON_Delete(me);
    if (err != ESP_OK)
    {
        return err;
    }

    *slot_out = slot;
    return ESP_OK;
}

/* ---- one store at a time ------------------------------------------------ */

/* Until 2026-11-01 the store ran on a one-shot 6 KB internal task of its
 * own (`espnl_store`: the §2 corollary, LittleFS with the cache off). The
 * Settings Manager runs its storage on an internal stack itself now
 * (flash_jobs, 2026-10-31), so the store runs on the caller's own stack
 * (PSRAM or not) and only its flash part is marshalled; the mutex keeps
 * one store at a time, as before. */
static SemaphoreHandle_t s_store_lock;   /* one store at a time */
static StaticSemaphore_t s_store_lock_buf;

void espnl_pair_init(void)
{
    if (s_store_lock == NULL)
    {
        s_store_lock = xSemaphoreCreateMutexStatic(&s_store_lock_buf);
    }
}

esp_err_t espnl_pair_store(const char *ssid, const char *password,
                           const char *device_id, int *slot_out,
                           bool *changed)
{
    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > 32 ||
        password == NULL || strlen(password) > 64 ||
        (device_id != NULL && strlen(device_id) > 12))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_store_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE; /* espnetlink_link_init() not run */
    }

    store_job_t job =
    {
        .ssid = ssid, .password = password, .device_id = device_id,
        .slot = -1, .changed = false, .err = ESP_FAIL,
    };

    xSemaphoreTake(s_store_lock, portMAX_DELAY);
    job.err = store_impl(&job);
    xSemaphoreGive(s_store_lock);

    if (slot_out != NULL)
    {
        *slot_out = job.slot;
    }
    if (changed != NULL)
    {
        *changed = job.changed;
    }
    return job.err;
}

esp_err_t espnetlink_link_pair(const char *ssid, const char *password,
                               int *slot_out)
{
    int slot = -1;
    bool changed = false;
    esp_err_t err = espnl_pair_store(ssid, password, "", &slot, &changed);

    if (err != ESP_OK)
    {
        return err;
    }

    ESP_LOGI(TAG, "paired with '%s' (wifi slot %d)%s", ssid, slot,
             changed ? ": reboot to apply" : " (unchanged)");
    if (slot_out != NULL)
    {
        *slot_out = slot;
    }
    return ESP_OK;
}
