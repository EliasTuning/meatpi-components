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
 * @file autopid_vehicle.c
 * @brief The vehicle store (TASK_quick_setup.md, second pass): owner of
 *        /data/autopid/vehicles.json (the index, a RAM copy under one
 *        lock), the import of the first-pass vehicle.json, the queries
 *        and edits behind /api/autopid/vehicles and the one-shot
 *        internal-stack worker `apid_veh` that does the poller's file
 *        work. The file-level switch, the first-contact decision
 *        (autopid_vehicle_seen) and the detection job's update live in
 *        autopid_vehicle_switch.c.
 *
 * Writes are event-driven only (a detection, a switch, the user's edits,
 * a once-a-day last_seen touch), never periodic (standard §11), and every
 * one happens on an INTERNAL stack (§2): the httpd task and the scan task
 * call in directly, the poller (PSRAM stack) hands its work to the worker.
 */
#include "autopid_private.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "autopid.h"   /* autopid_vehicle_summary_t */
#include "filesystem.h"

static const char *TAG = "autopid";

static ap_veh_index_t  s_idx EXT_RAM_BSS_ATTR;        /* the RAM copy        */
static char s_json[AP_VEH_INDEX_JSON_MAX] EXT_RAM_BSS_ATTR; /* persist scratch */
static char s_proto_cache[AP_VEH_PROTO_LEN];          /* current's protocol  */
static volatile uint8_t s_dialect_cache;              /* current's dialect   */
static volatile bool    s_j1939_cache;                /* current's network   */
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;                  /* internal: FreeRTOS  */

/* the one-shot worker: a PSRAM-stack caller marks the work and spawns it;
   it runs until nothing is pending, so bursts coalesce into one write */
static volatile bool s_dirty;
static volatile bool s_switch_pending;
static volatile bool s_writer_busy;
static char          s_switch_prev[AP_VEH_KEY_LEN];   /* under the lock      */
static StaticTask_t  s_writer_tcb;                    /* internal: FreeRTOS  */
/* PSRAM since 2026-11-01 (it was 6 KB of internal .bss, allocated for the
   firmware's life): every flash access of the writer goes through the
   filesystem component, which runs the flash part of a PSRAM-stack
   caller's operation on the shared flash worker (2026-10-31) */
static StackType_t   s_writer_stack[6144] EXT_RAM_BSS_ATTR;

/* ---- shared store internals ------------------------------------------------------ */

void ap_veh_lock(void)
{
    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
}

void ap_veh_unlock(void)
{
    xSemaphoreGive(s_lock);
}

ap_veh_index_t *ap_veh_index(void)
{
    return &s_idx;
}

int64_t ap_veh_epoch_now(void)
{
    time_t now = time(NULL);

    return (now > 1577836800) ? (int64_t)now : 0; /* 2020 floor */
}

void ap_veh_car_path(const char *key, char *out, size_t cap)
{
    snprintf(out, cap, AP_VEH_DIR "/%s.json", key);
}

void ap_veh_refresh_protocol_cache(void)
{
    const char *p = "";
    uint8_t dialect = AP_DIALECT_OBD2;
    bool j1939 = false;

    if (s_idx.current >= 0 && s_idx.current < s_idx.n)
    {
        p = s_idx.v[s_idx.current].protocol;
        dialect = s_idx.v[s_idx.current].dialect;
        j1939 = s_idx.v[s_idx.current].j1939;
    }

    snprintf(s_proto_cache, sizeof(s_proto_cache), "%s", p);
    s_dialect_cache = dialect;
    s_j1939_cache = j1939;
}

const char *autopid_vehicle_protocol(void)
{
    return s_proto_cache;
}

ap_dialect_t autopid_vehicle_dialect(void)
{
    return (ap_dialect_t)s_dialect_cache;
}

bool autopid_vehicle_j1939(void)
{
    return s_j1939_cache;
}

/** Serialize the RAM copy and replace the index file (internal-stack
 *  caller). The lock covers the scratch buffer and the write. */
esp_err_t ap_veh_persist(void)
{
    ap_veh_lock();
    s_dirty = false;

    int n = ap_vidx_to_json(&s_idx, s_json, sizeof(s_json));
    esp_err_t err = (n < 0) ? ESP_ERR_INVALID_SIZE
                            : filesystem_write(AP_VEH_INDEX_PATH, s_json,
                                               (size_t)n);

    ap_veh_unlock();

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "vehicles.json write failed (%s)",
                 esp_err_to_name(err));
    }

    return err;
}

static void writer_task(void *arg)
{
    (void)arg;

    do
    {
        if (s_switch_pending)
        {
            char prev[AP_VEH_KEY_LEN];

            ap_veh_lock();
            memcpy(prev, s_switch_prev, sizeof(prev));
            s_switch_pending = false;
            ap_veh_unlock();

            ap_veh_switch_files(prev, true);
        }

        if (s_dirty)
        {
            (void)ap_veh_persist();
        }
    } while (s_dirty || s_switch_pending);

    /* ephemeral tasks escape System Monitor: surface the watermark for
       the stack-audit bench (same net as the dtc job / std scan tasks) */
    ESP_LOGI(TAG, "vehicle writer stack_hw=%u B",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    s_writer_busy = false;
    vTaskDelete(NULL);
}

/** Run the worker (any task). A running worker loops on the flags. */
static void worker_kick(void)
{
    if (s_writer_busy)
    {
        return;
    }

    s_writer_busy = true;

    if (xTaskCreateStatic(writer_task, "apid_veh",
                          sizeof(s_writer_stack) / sizeof(s_writer_stack[0]),
                          NULL, 4, s_writer_stack, &s_writer_tcb) == NULL)
    {
        s_writer_busy = false;
        ESP_LOGW(TAG, "vehicle worker task unavailable; change kept in RAM");
    }
}

void ap_veh_persist_async(void)
{
    s_dirty = true;
    worker_kick();
}

void ap_veh_queue_switch(const char *prev_key)
{
    ap_veh_lock();
    snprintf(s_switch_prev, sizeof(s_switch_prev), "%s",
             (prev_key != NULL) ? prev_key : "");
    s_switch_pending = true;
    ap_veh_unlock();
    worker_kick();
}

/* ---- load + the first-pass import (autopid_init, main task) ------------------ */

/** The first-pass vehicle.json becomes the first (current) entry once;
 *  its tables are whatever config.json holds today. */
static void import_legacy(void)
{
    char body[AP_VEH_JSON_MAX + 1];
    size_t got = 0;

    if (filesystem_read(AP_VEH_LEGACY_PATH, body, sizeof(body) - 1, &got)
            != ESP_OK)
    {
        return;
    }

    body[got] = '\0';

    ap_vehicle_doc_t doc;
    ap_veh_entry_t e;

    memset(&e, 0, sizeof(e));

    if (ap_veh_doc_from_json(body, &doc) && !ap_veh_doc_empty(&doc))
    {
        ap_vidx_key_for(doc.vin, doc.fingerprint, e.key);
        snprintf(e.vin, sizeof(e.vin), "%s", doc.vin);
        snprintf(e.fingerprint, sizeof(e.fingerprint), "%s",
                 doc.fingerprint);
        snprintf(e.protocol, sizeof(e.protocol), "%s", doc.protocol);
        ap_vidx_default_name(doc.vin, doc.fingerprint, e.name);
        e.first_seen = doc.detected_ts;
        e.last_seen = doc.detected_ts;
        e.scan_ts = doc.detected_ts;
        e.pending_profile = false;      /* the user set this car up      */

        ap_veh_lock();

        int i = ap_vidx_add(&s_idx, &e, NULL);

        if (i >= 0)
        {
            s_idx.current = (int8_t)i;
        }

        ap_veh_refresh_protocol_cache();
        ap_veh_unlock();

        if (i >= 0)
        {
            char path[64];

            ap_veh_car_path(e.key, path, sizeof(path));
            (void)ap_veh_copy_tables(autopid_config_path(), path);
            (void)ap_veh_persist();
            ESP_LOGI(TAG, "vehicle store: imported %s (%s) from vehicle.json",
                     e.name, e.key);
        }
    }

    (void)filesystem_delete(AP_VEH_LEGACY_PATH);
}

void ap_vehicle_load(void)
{
    size_t got = 0;

    ap_veh_lock();
    ap_vidx_init(&s_idx);
    ap_veh_refresh_protocol_cache();
    ap_veh_unlock();

    esp_err_t err = filesystem_read(AP_VEH_INDEX_PATH, s_json,
                                    sizeof(s_json) - 1, &got);

    if (err == ESP_ERR_NOT_FOUND)
    {
        import_legacy();        /* fresh device, or the first-pass file  */
        return;
    }

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "vehicles.json unreadable (%s); starting empty",
                 esp_err_to_name(err));
        return;
    }

    s_json[got] = '\0';

    /* parse straight into the RAM copy (a failed parse leaves it empty):
       no 2.7 KB index on the main task's stack */
    ap_veh_lock();

    bool ok = ap_vidx_from_json(s_json, &s_idx);

    ap_veh_refresh_protocol_cache();

    if (!ok)
    {
        ap_veh_unlock();
        ESP_LOGW(TAG, "vehicles.json malformed; starting empty");
        return;
    }

    if (s_idx.current >= 0)
    {
        const ap_veh_entry_t *c = &s_idx.v[s_idx.current];

        ESP_LOGI(TAG, "vehicle store: %u cars, current %s (%s), protocol %s "
                      "(%s)", (unsigned)s_idx.n, c->name, c->key,
                 c->protocol[0] ? c->protocol : "(none)",
                 ap_dialect_name((ap_dialect_t)c->dialect));
    }
    else
    {
        ESP_LOGI(TAG, "vehicle store: %u cars, no current car",
                 (unsigned)s_idx.n);
    }

    ap_veh_unlock();
}

void ap_vehicle_apply_type_init(void)
{
    char init[AP_INIT_LEN];

    ap_veh_lock();

    const char *src = ap_core_specific_init_default();

    if (s_idx.current >= 0 && s_idx.current < s_idx.n &&
        s_idx.v[s_idx.current].specific_init[0] != '\0')
    {
        src = s_idx.v[s_idx.current].specific_init;
    }

    snprintf(init, sizeof(init), "%s", src);
    ap_veh_unlock();

    ap_runner_set_type_init(AP_PID_SPECIFIC, init);
}

/* ---- queries --------------------------------------------------------------------------- */

cJSON *autopid_vehicles_json(void)
{
    cJSON *o = cJSON_CreateObject();

    if (o == NULL)
    {
        return NULL;
    }

    ap_veh_lock();
    cJSON_AddNumberToObject(o, "version", 1);
    cJSON_AddStringToObject(o, "current",
                            (s_idx.current >= 0 && s_idx.current < s_idx.n)
                                ? s_idx.v[s_idx.current].key : "");
    cJSON_AddNumberToObject(o, "max", AP_VEH_MAX);

    cJSON *arr = cJSON_AddArrayToObject(o, "vehicles");

    for (int i = 0; arr != NULL && i < s_idx.n; i++)
    {
        cJSON *e = ap_vidx_entry_json(&s_idx.v[i], true, i == s_idx.current);

        if (e != NULL)
        {
            cJSON_AddItemToArray(arr, e);
        }
    }

    ap_veh_unlock();
    return o;
}

bool autopid_vehicle_summary(autopid_vehicle_summary_t *out)
{
    if (out == NULL)
    {
        return false;
    }

    memset(out, 0, sizeof(*out));
    ap_veh_lock();

    bool have = (s_idx.current >= 0 && s_idx.current < s_idx.n);

    if (have)
    {
        const ap_veh_entry_t *c = &s_idx.v[s_idx.current];

        snprintf(out->name, sizeof(out->name), "%s", c->name);
        snprintf(out->key, sizeof(out->key), "%s", c->key);
        snprintf(out->vin, sizeof(out->vin), "%s", c->vin);
        out->pending = c->pending_profile;
        out->cars = (uint8_t)s_idx.n;
    }

    ap_veh_unlock();
    return have;
}

cJSON *autopid_vehicle_entry_json(const char *key)
{
    cJSON *o = NULL;

    ap_veh_lock();

    int i = ap_vidx_find_key(&s_idx, key);

    if (i >= 0)
    {
        o = ap_vidx_entry_json(&s_idx.v[i], true, i == s_idx.current);
    }

    ap_veh_unlock();
    return o;
}

/* ---- edits (internal-stack callers: the httpd task) ---------------------------------- */

static bool set_str(char *dst, size_t cap, const char *src)
{
    if (src == NULL || strncmp(dst, src, cap - 1) == 0)
    {
        return false;
    }

    snprintf(dst, cap, "%s", src);
    return true;
}

esp_err_t autopid_vehicle_update(const char *key, const char *name,
                                 const char *profile,
                                 const char *specific_init)
{
    ap_veh_lock();

    int i = ap_vidx_find_key(&s_idx, key);

    if (i < 0)
    {
        ap_veh_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    ap_veh_entry_t *e = &s_idx.v[i];
    bool changed = set_str(e->name, sizeof(e->name), name);
    bool init_changed = false;

    if (profile != NULL)
    {
        /* the profile step is done either way: a name, or "" = keep the
           car without a profile (standard PIDs only), which also drops
           the init that belonged to the previous profile */
        changed |= set_str(e->profile, sizeof(e->profile), profile);

        if (e->pending_profile)
        {
            e->pending_profile = false;
            changed = true;
        }

        if (profile[0] == '\0' && e->specific_init[0] != '\0')
        {
            e->specific_init[0] = '\0';
            changed = true;
            init_changed = true;
        }
    }

    if (set_str(e->specific_init, sizeof(e->specific_init), specific_init))
    {
        changed = true;
        init_changed = true;
    }

    bool is_current = (i == s_idx.current);

    ap_veh_unlock();

    if (is_current && init_changed)
    {
        ap_vehicle_apply_type_init();   /* live: replays on the next poll */
        ap_runner_reset();
    }

    if (!changed)
    {
        return ESP_OK;                  /* nothing to write (§11)         */
    }

    ESP_LOGI(TAG, "vehicle %s updated", key);
    return ap_veh_persist();
}

esp_err_t autopid_vehicle_activate(const char *key)
{
    char prev[AP_VEH_KEY_LEN] = "";

    ap_veh_lock();

    int i = ap_vidx_find_key(&s_idx, key);

    if (i < 0)
    {
        ap_veh_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    if (i == s_idx.current)
    {
        ap_veh_unlock();
        return ESP_OK;                  /* already the active car         */
    }

    if (s_idx.current >= 0 && s_idx.current < s_idx.n)
    {
        memcpy(prev, s_idx.v[s_idx.current].key, sizeof(prev));
    }

    s_idx.current = (int8_t)i;
    ap_veh_refresh_protocol_cache();
    ap_veh_unlock();

    ap_veh_switch_files(prev, true);
    return ESP_OK;
}

esp_err_t autopid_vehicle_delete(const char *key)
{
    char path[64];

    ap_veh_lock();

    int i = ap_vidx_find_key(&s_idx, key);

    if (i < 0)
    {
        ap_veh_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    bool was_current = (i == s_idx.current);

    ap_veh_car_path(s_idx.v[i].key, path, sizeof(path));
    ap_vidx_remove(&s_idx, i);
    ap_veh_refresh_protocol_cache();
    ap_veh_unlock();

    esp_err_t err = filesystem_delete(path);

    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND)
    {
        ESP_LOGW(TAG, "%s delete failed (%s)", path, esp_err_to_name(err));
    }

    if (was_current)
    {
        /* config.json stays as it is; only the SPECIFIC init goes back
           to the settings default */
        ap_vehicle_apply_type_init();
        ap_runner_reset();
    }

    ESP_LOGI(TAG, "vehicle %s forgotten%s", key,
             was_current ? " (was current; tables kept)" : "");
    return ap_veh_persist();
}

void ap_vehicle_config_saved(const char *json, size_t len)
{
    char path[64] = "";

    ap_veh_lock();

    if (s_idx.current >= 0 && s_idx.current < s_idx.n)
    {
        ap_veh_car_path(s_idx.v[s_idx.current].key, path, sizeof(path));
    }

    ap_veh_unlock();

    if (path[0] == '\0')
    {
        return;                         /* no current car: nothing to mirror */
    }

    esp_err_t err = ap_veh_write_guarded(path, json, len);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "%s write failed (%s)", path, esp_err_to_name(err));
    }
}
