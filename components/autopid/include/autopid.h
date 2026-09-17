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
 * @file autopid.h
 * @brief AutoPID — scheduled OBD PID polling into a live parameter cache.
 *
 * The v6 rewrite of the legacy autopid (TASK_autopid.md): the PID is the
 * scheduling unit (one request updates ALL its parameters), no transports
 * inside (values flow out through the cache/API and, in Phase 3, the
 * event_manager), configuration = small settings knobs + the PID/filter
 * tables at /data/autopid/config.json (too big for settings arrays;
 * edited via GET/PUT /api/autopid/config, applied LIVE on save).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Register settings ("autopid") + log descriptors; load the config file
 *  tables into the static pools. No chip traffic. */
esp_err_t autopid_init(void);

/** Start the poller task (idle when disabled/no enabled PIDs). Refuses
 *  ESP_ERR_INVALID_STATE when unconfigured (§4.3 step 5). */
esp_err_t autopid_start(void);
esp_err_t autopid_stop(void);

/**
 * Runtime group control — EPHEMERAL (the log-level pattern): settings/
 * config decide boot state; event rules re-assert context after reboot
 * (driving/charging, TASK_event_manager §5b). @p period_override_ms < 0
 * keeps the group's configured period; 0 = high-fidelity max rate.
 */
esp_err_t autopid_group_set(const char *group, bool enabled,
                            int32_t period_override_ms);

/** Drop the runtime override: enabled back to the group's configured
 *  default, period back to the configured one. The `undo` of the
 *  `autopid.group` rule action (a "while" rule ending). */
esp_err_t autopid_group_restore(const char *group);

/**
 * The live value snapshot as a cJSON object — the LEGACY autopid_data
 * shape ({"ParamName": value, ...}) so existing dashboards keep working;
 * additional packagings are additional formatters over the same cache.
 * Caller frees.
 */
esp_err_t autopid_snapshot(cJSON **out);

/**
 * Duplicate the last-loaded config JSON (`/data/autopid/config.json`) from
 * a PSRAM cache — NO flash I/O, so a PSRAM-stack consumer (the ha_webhooks
 * poster) can include the config section safely (§2 corollary). Caller
 * frees @p out. ESP_ERR_INVALID_STATE before any config has loaded.
 */
esp_err_t autopid_config_json_dup(char **out);

/** One parameter's latest value (polled OR externally published, e.g.
 *  GPS). ESP_ERR_NOT_FOUND for unknown names; ESP_ERR_INVALID_STATE when
 *  a polled parameter has no valid sample yet. */
esp_err_t autopid_get_value(const char *param, double *out_value,
                            int64_t *out_ts_us);

/**
 * Publish an EXTERNAL named value into the live cache — a sample that is
 * not a polled OBD parameter (GPS from the ESPNetLink dongle, wired in
 * main from usb_acm_cli's GPS sink). It joins `autopid_snapshot()`
 * (→ the HA autopid_data push + `${autopid.data}`), fires the value sink
 * (→ data_logger), emits `autopid.param` on change (→ event rules), and
 * is readable via `autopid_get_value()` (→ dashboard). So a published
 * value reaches everywhere polled parameters do, with no per-consumer
 * work. Up to 12 distinct names; ESP_ERR_NO_MEM when that table is full.
 */
esp_err_t autopid_publish_external(const char *name, const char *unit,
                                   double value);

/**
 * Value sink: called for EVERY accepted sample (post-plausibility-
 * clamp), BEFORE the on-change/min_event_interval event gating —
 * `changed` compares against the last EMITTED value. Poller/filter
 * task context: the sink MUST NOT block or touch storage directly
 * (queue/ring pushes only). ONE sink; autopid stays storage-ignorant —
 * main wires it (e.g. `autopid_set_value_sink(data_logger_autopid_sink)`).
 * NULL unregisters.
 */
typedef void (*autopid_value_sink_t)(const char *name, const char *unit,
                                     double value, bool changed);
void autopid_set_value_sink(autopid_value_sink_t sink);

typedef struct
{
    uint32_t polls_ok;
    uint32_t polls_failed;
    uint32_t params_loaded;
    uint32_t pids_loaded;
    uint32_t filters_loaded;
    uint32_t groups_loaded;
    int64_t  last_poll_us;   /* 0 = never */
    bool     running;        /* poller up and polling (not paused)     */
    bool     paused_voltage; /* pause_below_v engaged                  */
    bool     paused_client;  /* yielding to an external ELM app        */
    bool     paused_diag;    /* a diagnostic tool holds the bus (the   */
                             /* UDS Tool / J2534 "exclusive" option)   */
} autopid_stats_t;

esp_err_t autopid_stats(autopid_stats_t *out);

/** True while the poller runs AND an ECU answered within the last 30 s —
 *  the device-contract `ecu_status` source (ha_webhooks). */
bool autopid_ecu_online(void);

/** Reload /data/autopid/config.json into the pools and restart the
 *  scheduler over the new tables (the PUT /api/autopid/config path —
 *  the config FILE applies live; the settings KNOBS stay reboot-to-apply). */
esp_err_t autopid_reload_config(void);

/* ---- DTC (TASK_dtc.md; the script_engine binding surface) ---------------
 * Everything is double-gated by the `dtc_enabled` (+ `dtc_allow_clear`
 * for mode 04) settings, BOTH default false. */

/** Start the async DTC scan job. ESP_ERR_NOT_ALLOWED when dtc_enabled is
 *  off; ESP_ERR_INVALID_STATE while another chip job runs. */
esp_err_t autopid_dtc_scan_start(void);

/** True while a DTC job (scan or queued clear) is running. */
bool autopid_dtc_scanning(void);

/** The last scan report as JSON ({"valid","ts","mil","mil_count",
 *  "stored":[...],"pending":[...],"permanent":[...],"new":[...]}).
 *  Caller frees. */
esp_err_t autopid_dtc_report(cJSON **out);

/** Conditional mode-04 clear (SYNC, seconds of bus I/O — never call from
 *  the event dispatcher). @p codes = CSV ("P0420,P0171") or NULL;
 *  @p mode = "always"|"if_any"|"if_only" or NULL (default: always
 *  without codes, if_any with). Mode 04 clears ALL codes + readiness
 *  monitors — the condition gates the wipe, it cannot narrow it.
 *  @p out_cleared true when the condition held and 44 was confirmed. */
esp_err_t autopid_dtc_clear(const char *codes, const char *mode,
                            bool *out_cleared);

/** Look @p code up in the uploaded DTC databases (TASK_dtc_db.md;
 *  name-order priority, first hit wins). ESP_ERR_NOT_FOUND when no db
 *  carries it. Lookup is PSRAM-cache only (no flash) — safe from any
 *  task. */
esp_err_t autopid_dtc_desc(const char *code, char *out, size_t out_cap);

/** Register the /api/autopid routes (own-routes pattern; main wires it in
 *  HTTP compositions only). */
esp_err_t autopid_register_http(void);

/** Register the `autopid` console command (called internally on the
 *  settings boot apply when the `cli` setting is true — Standard §6b). */
esp_err_t autopid_register_cli(void);

#ifdef __cplusplus
}
#endif
