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
 * @file api_http_system.c
 * @brief `/api/status` + `/api/info` (dev_status_manager/HTTP_API.md) and
 *        `/api/restart*` (restart_tracker/HTTP_API.md).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "sdkconfig.h"

#include "bridge_manager.h"
#include "event_manager.h"
#include "cmdline_manager.h"
#include "dev_status_manager.h"
#include "http_server_manager.h"
#include "log_manager.h"
#include "restart_tracker.h"
#include "settings_manager.h"

#include "api_http_private.h"

static const char *TAG = "api_http";

/* device-contract v2: the control-API level GET /api/info reports.
 * Bump when the /api surface changes incompatibly. */
#define API_INFO_API_LEVEL 6

/* ---- GET /api/status ----------------------------------------------------------- */

static esp_err_t status_handler(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON *bits = cJSON_AddObjectToObject(resp, "bits");

    for (int i = 0; i < 24; i++)
    {
        EventBits_t bit = (EventBits_t)1u << i;
        const char *name = dev_status_manager_bit_name(bit);

        if (strcmp(name, "unknown") != 0)
        {
            cJSON_AddBoolToObject(bits, name,
                                  dev_status_manager_is_set(bit));
        }
    }

    cJSON_AddBoolToObject(resp, "network_connected",
                          dev_status_manager_any_set(
                              DEV_STATUS_NETWORK_CONNECTED_MASK));

    char uptime[24] = "";

    dev_status_manager_format_uptime(uptime, sizeof(uptime));
    cJSON_AddStringToObject(resp, "uptime", uptime);
    cJSON_AddStringToObject(resp, "version",
                            dev_status_manager_app_version());
    cJSON_AddStringToObject(resp, "partition",
                            dev_status_manager_partition_label());

    {
        const char *device_id = dev_status_manager_device_id();
        char bluetooth_id[32] = { 0 };
        uint8_t wifi_mac[6] = { 0 };
        char wifi_mac_str[18] = { 0 };

        cJSON_AddStringToObject(resp, "device_id",
                                (device_id && device_id[0]) ? device_id : "");
        if (device_id && device_id[0])
        {
            snprintf(bluetooth_id, sizeof(bluetooth_id), "WiC_%s", device_id);
        }
        cJSON_AddStringToObject(resp, "bluetooth_id",
                                bluetooth_id[0] ? bluetooth_id : "N/A");
        if (esp_read_mac(wifi_mac, ESP_MAC_WIFI_STA) == ESP_OK)
        {
            snprintf(wifi_mac_str, sizeof(wifi_mac_str),
                     "%02X:%02X:%02X:%02X:%02X:%02X",
                     wifi_mac[0], wifi_mac[1], wifi_mac[2],
                     wifi_mac[3], wifi_mac[4], wifi_mac[5]);
        }
        cJSON_AddStringToObject(resp, "wifi_mac",
                                wifi_mac_str[0] ? wifi_mac_str : "N/A");
    }

    /* reboot counters come from restart_tracker (0s when unavailable) */
    static restart_tracker_state_t state; /* big struct: keep off the stack */
    uint32_t boot_count = 0;
    uint32_t unexpected = 0;

    if (restart_tracker_get_state(&state) == ESP_OK)
    {
        boot_count = state.boot_count;
        unexpected = state.unexpected_reset_count;
    }

    cJSON_AddNumberToObject(resp, "boot_count", boot_count);
    cJSON_AddNumberToObject(resp, "unexpected_resets", unexpected);

    /* memory health (Architecture §12b): internal is the scarce heap;
       largest_block vs free is THE fragmentation signal */
    dev_status_memory_t mem;

    if (dev_status_manager_memory(&mem) == ESP_OK)
    {
        cJSON *m = cJSON_AddObjectToObject(resp, "memory");
        const struct
        {
            const char *name;
            const dev_status_heap_t *h;
        } heaps[2] = { { "internal", &mem.internal },
                       { "psram", &mem.psram } };

        for (int i = 0; i < 2; i++)
        {
            cJSON *h = cJSON_AddObjectToObject(m, heaps[i].name);

            cJSON_AddNumberToObject(h, "total", heaps[i].h->total);
            cJSON_AddNumberToObject(h, "free", heaps[i].h->free);
            cJSON_AddNumberToObject(h, "min_free", heaps[i].h->min_free);
            cJSON_AddNumberToObject(h, "largest_block",
                                    heaps[i].h->largest_block);
        }
    }

    /* die temperature (best effort — omitted if the sensor errors) */
    float temp_c = 0;

    if (dev_status_manager_temperature(&temp_c) == ESP_OK)
    {
        cJSON_AddNumberToObject(resp, "temp_c",
                                (double)((int)(temp_c * 10)) / 10.0);
    }

    /* health surface (2026-07-19): log E/W counters, flash-op counters
       and the latched fault count — what the bench asserts against */
    cJSON *health = cJSON_AddObjectToObject(resp, "health");
    uint32_t errors = 0;
    uint32_t warnings = 0;

    log_manager_health(&errors, &warnings);
    cJSON_AddNumberToObject(health, "log_errors", errors);
    cJSON_AddNumberToObject(health, "log_warnings", warnings);

    dev_status_flash_t fl;

    if (dev_status_manager_flash(&fl) == ESP_OK)
    {
        cJSON *f = cJSON_AddObjectToObject(health, "flash");

        cJSON_AddNumberToObject(f, "writes", fl.write_count);
        cJSON_AddNumberToObject(f, "write_bytes", fl.write_bytes);
        cJSON_AddNumberToObject(f, "erases", fl.erase_count);
        cJSON_AddNumberToObject(f, "erase_bytes", fl.erase_bytes);
    }

    cJSON *caps = cJSON_AddObjectToObject(health, "caps");
    size_t used;
    size_t cap;
    int eu, ec, tu, tc;

    settings_manager_capacity(&used, &cap);
    cJSON *c = cJSON_AddObjectToObject(caps, "settings");

    cJSON_AddNumberToObject(c, "used", used);
    cJSON_AddNumberToObject(c, "cap", cap);
    cmdline_manager_capacity(&used, &cap);
    c = cJSON_AddObjectToObject(caps, "cmdline");
    cJSON_AddNumberToObject(c, "used", used);
    cJSON_AddNumberToObject(c, "cap", cap);
    bridge_manager_capacity(&eu, &ec, &tu, &tc);
    c = cJSON_AddObjectToObject(caps, "bridge_ep");
    cJSON_AddNumberToObject(c, "used", eu);
    cJSON_AddNumberToObject(c, "cap", ec);

    /* the rules engine registries (standard §12, 2026-09-17) */
    size_t su, sc, au, ac;

    event_manager_capacity(&su, &sc, &au, &ac);
    c = cJSON_AddObjectToObject(caps, "events_src");
    cJSON_AddNumberToObject(c, "used", su);
    cJSON_AddNumberToObject(c, "cap", sc);
    c = cJSON_AddObjectToObject(caps, "events_act");
    cJSON_AddNumberToObject(c, "used", au);
    cJSON_AddNumberToObject(c, "cap", ac);

    cJSON_AddNumberToObject(health, "faults",
                            dev_status_manager_faults(NULL, 0));

    return api_send_json(req, resp);
}

/* ---- GET /api/faults + POST /api/faults/clear (device DTCs) ----------------- */

static esp_err_t faults_handler(httpd_req_t *req)
{
    static dev_status_fault_t faults[DEV_STATUS_FAULT_MAX]; /* off-stack */
    int n = dev_status_manager_faults(faults, DEV_STATUS_FAULT_MAX);
    cJSON *resp = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(resp, "faults");

    for (int i = 0; i < n; i++)
    {
        cJSON *f = cJSON_CreateObject();

        cJSON_AddStringToObject(f, "code", faults[i].code);
        cJSON_AddStringToObject(f, "detail", faults[i].detail);
        cJSON_AddNumberToObject(f, "count", faults[i].count);
        cJSON_AddNumberToObject(f, "first_time", faults[i].first_time);
        cJSON_AddNumberToObject(f, "last_time", faults[i].last_time);
        cJSON_AddItemToArray(arr, f);
    }

    return api_send_json(req, resp);
}

static esp_err_t faults_clear_handler(httpd_req_t *req)
{
    esp_err_t err = dev_status_manager_faults_clear();
    cJSON *resp = cJSON_CreateObject();

    cJSON_AddBoolToObject(resp, "cleared", err == ESP_OK);
    return api_send_json(req, resp);
}

/* ---- GET /api/info (device identity — device-contract v2 ask #1) ----------- */

static esp_err_t info_handler(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();

    /* same keys and casing as the webhook push `status` section — the
       HA integration verifies identity BEFORE control commands with
       this, so keep it cheap and dependency-free */
    cJSON_AddStringToObject(resp, "device_type", CONFIG_WICAN_DEVICE_TYPE);
    cJSON_AddStringToObject(resp, "model", CONFIG_WICAN_MODEL_NAME);
    cJSON_AddStringToObject(resp, "hw_version", CONFIG_WICAN_HW_VERSION);
    cJSON_AddStringToObject(resp, "fw_version",
                            dev_status_manager_app_version());
    cJSON_AddStringToObject(resp, "device_id",
                            dev_status_manager_device_id());

    uint8_t mac[6] = { 0 };
    char mac_str[18];

    esp_read_mac(mac, ESP_MAC_WIFI_STA); /* matches the mDNS TXT `mac` */
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(resp, "mac", mac_str);

    cJSON_AddNumberToObject(resp, "api_level", API_INFO_API_LEVEL);
    return api_send_json(req, resp);
}

/* ---- GET /api/status/tasks (task monitor: see dev_status HTTP_API.md) ------ */

#define TASKS_MAX 48 /* > any real composition (~25 tasks observed) */

static esp_err_t tasks_handler(httpd_req_t *req)
{
    /* serialized by the httpd worker config; PSRAM keeps this off the
       internal heap (~2.3 KB) */
    static dev_status_task_t EXT_RAM_BSS_ATTR tasks[TASKS_MAX];
    size_t count = 0;
    uint64_t total_us = 0;
    esp_err_t err = dev_status_manager_task_stats(tasks, TASKS_MAX, &count,
                                                  &total_us);

    if (err == ESP_ERR_NOT_SUPPORTED)
    {
        return api_send_error(req, "501 Not Implemented",
                              "CONFIG_FREERTOS_USE_TRACE_FACILITY is off");
    }

    if (err != ESP_OK)
    {
        return api_send_error(req, "500 Internal Server Error", "tasks");
    }

    cJSON *resp = cJSON_CreateObject();

    /* raw cumulative counters — CPU%% is a CLIENT-side delta between two
       polls: task_delta / (total_delta * cores). total_us is per-core
       scheduler time (dev_status_manager.h) */
    cJSON_AddNumberToObject(resp, "cores", CONFIG_FREERTOS_NUMBER_OF_CORES);
    cJSON_AddNumberToObject(resp, "total_us", (double)total_us);

    cJSON *arr = cJSON_AddArrayToObject(resp, "tasks");

    for (size_t i = 0; i < count; i++)
    {
        cJSON *t = cJSON_CreateObject();
        char state[2] = { tasks[i].state, '\0' };

        cJSON_AddStringToObject(t, "name", tasks[i].name);
        cJSON_AddStringToObject(t, "state", state);
        cJSON_AddNumberToObject(t, "core", tasks[i].core);
        cJSON_AddNumberToObject(t, "prio", tasks[i].prio);
        cJSON_AddNumberToObject(t, "stack_hw", tasks[i].stack_hw);
        cJSON_AddNumberToObject(t, "runtime_us",
                                (double)tasks[i].runtime_us);
        cJSON_AddItemToArray(arr, t);
    }

    return api_send_json(req, resp);
}

/* ---- GET /api/restart/history --------------------------------------------------- */

static esp_err_t history_handler(httpd_req_t *req)
{
    static restart_tracker_state_t state; /* serialized by httpd worker cfg */

    if (restart_tracker_get_state(&state) != ESP_OK)
    {
        return api_send_error(req, "503 Service Unavailable",
                              "tracker state unavailable");
    }

    cJSON *resp = cJSON_CreateObject();

    cJSON_AddNumberToObject(resp, "boot_count", state.boot_count);
    cJSON_AddNumberToObject(resp, "unexpected_resets",
                            state.unexpected_reset_count);

    /* valid records (sequence > 0), newest first by sequence */
    int order[RESTART_TRACKER_HISTORY_LEN];
    int count = 0;

    for (int i = 0; i < RESTART_TRACKER_HISTORY_LEN; i++)
    {
        if (state.history[i].sequence == 0)
        {
            continue;
        }

        int pos = count;

        while (pos > 0 && state.history[order[pos - 1]].sequence <
                              state.history[i].sequence)
        {
            order[pos] = order[pos - 1];
            pos--;
        }

        order[pos] = i;
        count++;
    }

    cJSON *records = cJSON_AddArrayToObject(resp, "records");

    for (int n = 0; n < count; n++)
    {
        const restart_tracker_record_t *r = &state.history[order[n]];
        cJSON *rec = cJSON_CreateObject();

        cJSON_AddNumberToObject(rec, "seq", r->sequence);
        cJSON_AddStringToObject(rec, "reason",
            restart_tracker_reset_reason_to_str(r->actual_reset_reason));
        cJSON_AddBoolToObject(rec, "planned", r->was_planned != 0);
        cJSON_AddStringToObject(rec, "planned_reason",
            restart_tracker_planned_reason_to_str(
                (restart_tracker_planned_reason_t)r->planned_reason));
        cJSON_AddStringToObject(rec, "source",
            restart_tracker_source_to_str(
                (restart_tracker_source_t)r->source));
        cJSON_AddNumberToObject(rec, "flags", r->flags);
        cJSON_AddNumberToObject(rec, "boot_time", (double)r->boot_timestamp);
        cJSON_AddBoolToObject(rec, "time_valid", r->time_valid != 0);
        cJSON_AddNumberToObject(rec, "request_time",
                                (double)r->request_timestamp);
        cJSON_AddNumberToObject(rec, "request_uptime_ms",
                                (double)r->request_uptime_ms);
        cJSON_AddItemToArray(records, rec);
    }

    return api_send_json(req, resp);
}

/* ---- POST /api/restart ----------------------------------------------------------- */

static esp_err_t restart_handler(httpd_req_t *req)
{
    uint32_t flags = 0;

    if (req->content_len > 0)
    {
        char *body = api_read_body(req, 256);

        if (body != NULL)
        {
            cJSON *in = cJSON_Parse(body);
            const cJSON *f = cJSON_GetObjectItemCaseSensitive(in, "flags");

            if (cJSON_IsNumber(f))
            {
                flags = (uint32_t)f->valuedouble;
            }

            cJSON_Delete(in);
            free(body);
        }
    }

    ESP_LOGW(TAG, "reboot requested via /api/restart (flags=%lu)",
             (unsigned long)flags);

    cJSON *resp = cJSON_CreateObject();

    cJSON_AddBoolToObject(resp, "ok", true);

    esp_err_t err = api_send_json(req, resp);

    api_schedule_reboot(RESTART_TRACKER_PLANNED_REASON_USER_REQUEST,
                        RESTART_TRACKER_SOURCE_WEB_UI, flags);
    return err;
}

esp_err_t api_http_register_system(void)
{
    static const httpd_uri_t URIS[] =
    {
        { .uri = "/api/status/tasks", .method = HTTP_GET,
          .handler = tasks_handler }, /* exact before any future wildcard */
        { .uri = "/api/status", .method = HTTP_GET,
          .handler = status_handler },
        { .uri = "/api/info", .method = HTTP_GET,
          .handler = info_handler },
        { .uri = "/api/restart/history", .method = HTTP_GET,
          .handler = history_handler },
        { .uri = "/api/restart", .method = HTTP_POST,
          .handler = restart_handler },
        { .uri = "/api/faults/clear", .method = HTTP_POST,
          .handler = faults_clear_handler }, /* exact before /api/faults */
        { .uri = "/api/faults", .method = HTTP_GET,
          .handler = faults_handler },
    };

    return http_server_manager_register_handlers(
        URIS, sizeof(URIS) / sizeof(URIS[0]));
}
