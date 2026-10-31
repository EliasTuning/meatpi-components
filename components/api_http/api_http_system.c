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
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "sdkconfig.h"

#include "ble_manager.h"
#include "bridge_manager.h"
#include "event_manager.h"
#include "cmdline_manager.h"
#include "dev_status_manager.h"
#include "flash_jobs.h"
#include "http_server_manager.h"
#include "log_manager.h"
#include "partition_migrate.h"
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

    /* the USB connector summary (2026-10-13): what is on the connector and
       its one-line state, for the header pill and the ESPNetLink bar;
       absent while nothing is enumerated */
    dev_status_usb_t usb;

    if (dev_status_manager_usb(&usb))
    {
        cJSON *u = cJSON_AddObjectToObject(resp, "usb");

        if (u != NULL)
        {
            cJSON_AddStringToObject(u, "kind",
                                    dev_status_manager_usb_kind_str(usb.kind));
            if (usb.kind == DEV_STATUS_USB_ESPNETLINK)
            {
                cJSON_AddStringToObject(u, "device_id", usb.device_id);
                cJSON_AddStringToObject(u, "sim", usb.sim);
                cJSON_AddBoolToObject(u, "lte", usb.lte);
                cJSON_AddStringToObject(u, "operator", usb.operator_name);
                cJSON_AddBoolToObject(u, "gps_fix", usb.fix);
                cJSON_AddNumberToObject(u, "satellites", usb.satellites);
                cJSON_AddNumberToObject(u, "no_fix_s", usb.no_fix_s);
                cJSON_AddStringToObject(u, "hint",
                                        usb.no_fix_hint ? "no_fix" : "");
            }
            else if (usb.kind == DEV_STATUS_USB_GPS)
            {
                cJSON_AddBoolToObject(u, "fix", usb.fix);
                cJSON_AddNumberToObject(u, "satellites", usb.satellites);
                cJSON_AddStringToObject(u, "product", usb.product);
            }
            else if (usb.kind == DEV_STATUS_USB_ETH)
            {
                cJSON_AddBoolToObject(u, "link", usb.link);
                cJSON_AddStringToObject(u, "ip", usb.ip);
                cJSON_AddStringToObject(u, "product", usb.product);
            }
            else
            {
                cJSON_AddStringToObject(u, "product", usb.product);
            }
        }
    }

    /* the SD slot summary (2026-10-25): what card is in and whether WiCAN
       can read it, for the notice bar and the pages that mention the card;
       absent while the slot is empty */
    dev_status_sd_t sd;

    if (dev_status_manager_sd(&sd))
    {
        cJSON *s = cJSON_AddObjectToObject(resp, "sd");

        if (s != NULL)
        {
            char serial[12];

            snprintf(serial, sizeof(serial), "%08lx", (unsigned long)sd.serial);
            cJSON_AddStringToObject(s, "state", sd.state);
            cJSON_AddStringToObject(s, "reason", sd.reason);
            cJSON_AddStringToObject(s, "name", sd.name);
            cJSON_AddNumberToObject(s, "capacity_bytes", (double)sd.capacity_bytes);
            cJSON_AddStringToObject(s, "serial", serial);
        }
    }

    /* the current vehicle summary (2026-10-26): which car the live tables
       belong to and whether its setup finished, for the notice bar and the
       dashboard; absent while the store has no current car */
    dev_status_vehicle_t veh;

    if (dev_status_manager_vehicle(&veh))
    {
        cJSON *v = cJSON_AddObjectToObject(resp, "vehicle");

        if (v != NULL)
        {
            cJSON_AddStringToObject(v, "name", veh.name);
            cJSON_AddStringToObject(v, "key", veh.key);
            cJSON_AddStringToObject(v, "vin", veh.vin);
            cJSON_AddBoolToObject(v, "pending", veh.pending);
            cJSON_AddNumberToObject(v, "cars", veh.cars);
        }
    }

    /* memory health (Architecture §12b): internal is the scarce heap;
       largest_block vs free is THE fragmentation signal. This route is
       polled (the web UI: every 3 s), so the PSRAM heap is not walked for
       its largest block: that is 3 to 4 ms with interrupts off, and a busy
       CAN bus paid for every poll in frames (2026-10-03). `?deep=1` asks
       for it. */
    dev_status_memory_t mem;
    char query[24];
    char deep[4] = "";
    bool want_deep =
        httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "deep", deep, sizeof(deep)) == ESP_OK &&
        deep[0] == '1';
    esp_err_t mem_err = want_deep ? dev_status_manager_memory_deep(&mem)
                                  : dev_status_manager_memory(&mem);

    if (mem_err == ESP_OK)
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

            if (heaps[i].h->largest_block != 0) /* 0 = not measured */
            {
                cJSON_AddNumberToObject(h, "largest_block",
                                        heaps[i].h->largest_block);
            }
        }
    }

    /* die temperature (best effort: omitted if the sensor errors) */
    float temp_c = 0;

    if (dev_status_manager_temperature(&temp_c) == ESP_OK)
    {
        cJSON_AddNumberToObject(resp, "temp_c",
                                (double)((int)(temp_c * 10)) / 10.0);
    }

    /* health surface (2026-07-19): log E/W counters, flash-op counters
       and the latched fault count, what the bench asserts against */
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

    /* the flash worker (2026-11-01): how often PSRAM-stack callers needed
       it, whether one is alive at this reading (the heap probe says so per
       row), the lowest watermark a worker reported, and the refusals (no
       internal RAM for a worker: main latches a fault on them) */
    flash_jobs_stats_t fj;

    flash_jobs_stats(&fj);

    cJSON *fjo = cJSON_AddObjectToObject(health, "flash_jobs");

    cJSON_AddNumberToObject(fjo, "inline", fj.inline_runs);
    cJSON_AddNumberToObject(fjo, "marshalled", fj.marshalled);
    cJSON_AddNumberToObject(fjo, "spawns", fj.spawns);
    cJSON_AddNumberToObject(fjo, "exits", fj.exits);
    cJSON_AddNumberToObject(fjo, "refusals", fj.refusals);
    cJSON_AddNumberToObject(fjo, "stack_hw_min", fj.stack_hw_min);
    cJSON_AddBoolToObject(fjo, "worker_alive", fj.worker_alive);

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

    /* BLE stream channels (ble_http, ble_j2534; standard §12, 2026-09-21) */
    ble_manager_channel_capacity(&used, &cap);
    c = cJSON_AddObjectToObject(caps, "ble_ch");
    cJSON_AddNumberToObject(c, "used", used);
    cJSON_AddNumberToObject(c, "cap", cap);

    /* the HTTP route table (it overflowed twice before it could be seen:
       2026-07-08 and 2026-09-07; standard §12, 2026-10-03) */
    http_server_manager_capacity(&used, &cap);
    c = cJSON_AddObjectToObject(caps, "http_routes");
    cJSON_AddNumberToObject(c, "used", used);
    cJSON_AddNumberToObject(c, "cap", cap);

    cJSON_AddNumberToObject(health, "faults",
                            dev_status_manager_faults(NULL, 0));

    return api_send_json(req, resp);
}

/* ---- GET /api/faults + POST /api/faults/clear (device DTCs) ----------------- */

static esp_err_t faults_handler(httpd_req_t *req)
{
    static dev_status_fault_t faults[DEV_STATUS_FAULT_MAX] EXT_RAM_BSS_ATTR; /* off-stack, PSRAM (1.3 KB) */
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

/* ---- GET /api/info (device identity: device-contract v2 ask #1) ----------- */

static esp_err_t info_handler(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();

    /* same keys and casing as the webhook push `status` section: the
       HA integration verifies identity BEFORE control commands with
       this, so keep it cheap and dependency-free */
    cJSON_AddStringToObject(resp, "device_type", CONFIG_WICAN_DEVICE_TYPE);
    cJSON_AddStringToObject(resp, "model", CONFIG_WICAN_MODEL_NAME);
    cJSON_AddStringToObject(resp, "hw_version", CONFIG_WICAN_HW_VERSION);
    cJSON_AddStringToObject(resp, "fw_version",
                            dev_status_manager_app_version());
    cJSON_AddStringToObject(resp, "device_id",
                            dev_status_manager_device_id());
    /* 2026-10-18: the build (git describe) beside the release number, the
       running slot and the partition layout's id, so the HA integration
       and a fleet tool reason about an update without guessing */
    cJSON_AddStringToObject(resp, "fw_build", dev_status_manager_app_build());
    cJSON_AddStringToObject(resp, "partition",
                            dev_status_manager_partition_label());

    char layout_md5[33] = "";

    (void)partition_migrate_flashed_md5(layout_md5);
    cJSON_AddStringToObject(resp, "layout_md5", layout_md5);

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

#define TASKS_MAX 64 /* > any real composition: 49 tasks on 2026-10-31 (the
                        bench DUT with the dongle, the logger and the
                        webhook poster), and the list is cut silently
                        at the cap, which hid fs_writer from the stack
                        audit; PSRAM, so headroom is cheap */

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

    /* raw cumulative counters: CPU%% is a CLIENT-side delta between two
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
        /* 2026-10-31: where the stack lives, for the stack audit (the HTTP
           server's and the filesystem writer's are in PSRAM now) */
        cJSON_AddBoolToObject(t, "stack_ext", tasks[i].stack_ext);
        cJSON_AddNumberToObject(t, "runtime_us",
                                (double)tasks[i].runtime_us);
        cJSON_AddItemToArray(arr, t);
    }

    return api_send_json(req, resp);
}

/* ---- GET /api/restart/history --------------------------------------------------- */

/** An address as the console prints it: "0x4201a2b3" (what addr2line takes). */
static void add_hex(cJSON *obj, const char *name, uint32_t value)
{
    char text[12];

    snprintf(text, sizeof(text), "0x%08" PRIx32, value);
    cJSON_AddStringToObject(obj, name, text);
}

static cJSON *hex_array(const uint32_t *values, int count)
{
    cJSON *arr = cJSON_CreateArray();

    for (int i = 0; i < count; i++)
    {
        char text[12];

        snprintf(text, sizeof(text), "0x%08" PRIx32, values[i]);
        cJSON_AddItemToArray(arr, cJSON_CreateString(text));
    }

    return arr;
}

/** A crash note as JSON (restart_tracker/HTTP_API.md): the one shape for a
 *  history record's `crash` and the stored report's. */
static cJSON *crash_obj(const restart_tracker_crash_t *crash)
{
    static char summary[224] EXT_RAM_BSS_ATTR; /* httpd worker: one at a time */
    cJSON *obj = cJSON_CreateObject();

    restart_tracker_crash_summary(crash, summary, sizeof(summary));
    cJSON_AddStringToObject(obj, "summary", summary);
    cJSON_AddStringToObject(obj, "kind",
                            restart_tracker_crash_kind_to_str(crash->kind));
    cJSON_AddStringToObject(obj, "reason", crash->reason);
    cJSON_AddNumberToObject(obj, "cause", crash->cause);
    add_hex(obj, "pc", crash->pc);
    add_hex(obj, "excvaddr", crash->excvaddr);
    cJSON_AddNumberToObject(obj, "core", crash->core);
    cJSON_AddStringToObject(obj, "task", crash->task);
    cJSON_AddBoolToObject(obj, "in_isr", crash->in_isr);
    cJSON_AddNumberToObject(obj, "uptime_s", crash->uptime_s);
    cJSON_AddStringToObject(obj, "text", crash->text);
    cJSON_AddItemToObject(obj, "backtrace",
                          hex_array(crash->bt, crash->bt_len));
    cJSON_AddBoolToObject(obj, "backtrace_more", crash->bt_more);
    cJSON_AddBoolToObject(obj, "backtrace_corrupt", crash->bt_corrupt);
    cJSON_AddItemToObject(obj, "other_core",
                          hex_array(crash->bt2, crash->bt2_len));
    cJSON_AddBoolToObject(obj, "nested", crash->nested);
    cJSON_AddStringToObject(obj, "elf_sha", crash->elf_sha);
    cJSON_AddBoolToObject(obj, "same_image",
                          crash->elf_sha[0] != '\0' &&
                          strcmp(crash->elf_sha,
                                 esp_app_get_elf_sha256_str()) == 0);
    cJSON_AddBoolToObject(obj, "complete", crash->complete);
    return obj;
}

/** The crash note filed under a boot, or NULL when that boot followed no
 *  recorded crash. */
static cJSON *crash_json(uint32_t sequence)
{
    static restart_tracker_crash_t crash EXT_RAM_BSS_ATTR; /* as above */

    if (restart_tracker_get_crash(sequence, &crash) != ESP_OK)
    {
        return NULL;
    }

    return crash_obj(&crash);
}

/** The crash report kept in NVS (it outlives the power, the notes above do
 *  not), or NULL when none is stored. */
static cJSON *report_json(void)
{
    static restart_tracker_report_t report EXT_RAM_BSS_ATTR; /* as above */

    if (restart_tracker_get_report(&report) != ESP_OK)
    {
        return NULL;
    }

    cJSON *obj = cJSON_CreateObject();

    cJSON_AddNumberToObject(obj, "stored_time", (double)report.stored_unix);
    cJSON_AddBoolToObject(obj, "time_valid", report.stored_unix > 0);
    cJSON_AddStringToObject(obj, "firmware", report.firmware);
    cJSON_AddNumberToObject(obj, "streak", report.streak);
    cJSON_AddBoolToObject(obj, "parked", report.parked);
    cJSON_AddItemToObject(obj, "crash", crash_obj(&report.crash));
    return obj;
}

/** The crash-loop brake: this boot's verdict and the count as it stands. */
static cJSON *brake_json(void)
{
    restart_tracker_brake_t brake;

    if (restart_tracker_get_brake(&brake) != ESP_OK)
    {
        return NULL;
    }

    cJSON *obj = cJSON_CreateObject();

    cJSON_AddStringToObject(obj, "verdict",
                            restart_tracker_boot_mode_to_str(brake.verdict));
    cJSON_AddNumberToObject(obj, "streak", brake.streak);
    cJSON_AddNumberToObject(obj, "limit", RESTART_TRACKER_BRAKE_STREAK);
    cJSON_AddNumberToObject(obj, "parks", brake.parks);
    cJSON_AddBoolToObject(obj, "settled", brake.settled);
    cJSON_AddNumberToObject(obj, "settle_s", RESTART_TRACKER_SETTLE_S);
    cJSON_AddNumberToObject(obj, "report_budget", brake.report_budget);
    return obj;
}

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
    /* the running image, as a crash note names the image that crashed */
    cJSON_AddStringToObject(resp, "elf_sha", esp_app_get_elf_sha256_str());

    cJSON *extra = brake_json();

    if (extra != NULL)
    {
        cJSON_AddItemToObject(resp, "brake", extra);
    }

    extra = report_json();

    if (extra != NULL)
    {
        cJSON_AddItemToObject(resp, "report", extra);
    }

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
        cJSON_AddStringToObject(rec, "mode",
                                restart_tracker_boot_mode_to_str(r->boot_mode));
        cJSON_AddBoolToObject(rec, "settled", r->settled != 0);

        cJSON *crash = crash_json(r->sequence);

        if (crash != NULL)
        {
            cJSON_AddItemToObject(rec, "crash", crash);
        }

        cJSON_AddItemToArray(records, rec);
    }

    return api_send_json(req, resp);
}

/* ---- GET / DELETE /api/restart/report -------------------------------------------- */

/** The stored crash report as the text a user sends on: the same lines the
 *  console's `restart_tracker --report` and safe mode's page give. */
static esp_err_t report_handler(httpd_req_t *req)
{
    /* PSRAM; serialized by httpd worker cfg */
    static restart_tracker_report_t report EXT_RAM_BSS_ATTR;
    static char text[RESTART_TRACKER_REPORT_TEXT_MAX] EXT_RAM_BSS_ATTR;

    if (restart_tracker_get_report(&report) != ESP_OK)
    {
        return api_send_error(req, "404 Not Found",
                              "no crash report is stored");
    }

    int len = restart_tracker_report_text(&report, text, sizeof(text));

    if (len >= (int)sizeof(text))
    {
        len = (int)sizeof(text) - 1;
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, text, (len > 0) ? len : 0);
}

static esp_err_t report_clear_handler(httpd_req_t *req)
{
    /* one NVS erase, on the worker's internal stack (standard 2) */
    esp_err_t err = restart_tracker_clear_report();
    cJSON *resp = cJSON_CreateObject();

    cJSON_AddBoolToObject(resp, "cleared", err == ESP_OK);
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
        { .uri = "/api/restart/report", .method = HTTP_GET,
          .handler = report_handler },
        { .uri = "/api/restart/report", .method = HTTP_DELETE,
          .handler = report_clear_handler },
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
