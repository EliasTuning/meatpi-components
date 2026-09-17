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
 * @file event_manager.h
 * @brief The device automation engine: components PUBLISH typed events
 *        and REGISTER named actions/pull-values; user rules (settings,
 *        UI-edited) wire them together with conditions and templates.
 *        The manager knows no component by name (bridge_manager's
 *        ownership inversion). Design: TASK_event_manager.md.
 *
 * Threading:
 *  - declare/register calls are BOOT-CONTEXT ONLY (component init/start;
 *    plain static-array writes, no lock) and live forever.
 *  - event_manager_publish() is callable from ANY TASK (not ISRs): it
 *    copies into a bounded queue and never blocks (queue full =
 *    drop-oldest + counter).
 *  - Inline action + value handlers run in the DISPATCHER task (8 KB
 *    PSRAM stack). A `blocking` action instead runs on a bounded WORKER
 *    POOL (PSRAM stacks) so a slow network/bus call can't stall the
 *    dispatcher. Either way a handler MUST NOT touch flash/LittleFS
 *    (standard §2 corollary — all these stacks are PSRAM); network calls
 *    use their component's existing timeouts.
 *
 * All timestamps are esp_timer 64-bit µs — the 32-bit tick types never
 * appear here (meatpi 2026-07-06).
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

#define EM_KV_MAX   6
#define EM_STR_MAX  48

typedef enum
{
    EM_VAL_F64 = 0,
    EM_VAL_I64,
    EM_VAL_STR,
    EM_VAL_BOOL,
} em_val_type_t;

typedef struct
{
    const char   *key;              /* "param", "value", "topic" …        */
    em_val_type_t type;
    union
    {
        double  f64;
        int64_t i64;
        bool    b;
        char    str[EM_STR_MAX];
    } v;
} em_kv_t;

/** One occurrence: flat, bounded, trivially JSON-serializable. */
typedef struct
{
    char    source[16];             /* component: "autopid", "mqtt" …      */
    char    name[16];               /* event type: "param", "rx" …         */
    int64_t ts_us;                  /* esp_timer_get_time() at publish     */
    uint8_t n;
    em_kv_t kv[EM_KV_MAX];
} em_event_t;

/* ---- source declaration (UI dropdowns + validation) ------------------------ */

typedef struct
{
    const char   *key;
    em_val_type_t type;
} em_key_decl_t;

typedef struct
{
    const char          *source;    /* "battery"                           */
    const char          *name;      /* "threshold"                         */
    const char          *description;
    const em_key_decl_t *keys;      /* what the event carries              */
    uint8_t              n_keys;
} em_source_decl_t;

/* ---- action / pull-value registration --------------------------------------- */

typedef struct
{
    const char *name;               /* "mqtt.publish", "obd.request"       */
    const char *params_schema;      /* JSON Schema for `with` (flash str)  */
    esp_err_t (*run)(const cJSON *with, const em_event_t *trigger);
    /* true = a SLOW action (network/bus round-trip). It runs on the
       bounded worker pool instead of the dispatcher, so it can't stall
       other events. Like every action, run() MUST NOT touch flash (the
       workers are PSRAM stacks — §2 corollary). Default false = inline. */
    bool        blocking;
    /* true = the action can be reversed: a rule with `undo:true` (a
       "while" rule) calls run() again with "undo":true added to `with`
       when its conditions stop holding (autopid.group restores the
       configured group state, led.indicate clears). Default false =
       settings validation rejects `undo` on the action. 2026-09-17 */
    bool        undoable;
} em_action_t;

/* ---- lifecycle ---------------------------------------------------------------- */

/** Register settings ("event_manager") + log descriptors, create the
 *  queue. Call EARLY in main's init pass (before components that declare
 *  sources/actions is not required — declares are static writes — but
 *  before settings_manager_start). */
esp_err_t event_manager_init(void);

/** Create the dispatcher task + arm the settings-defined timers.
 *  ESP_ERR_INVALID_STATE when unconfigured (§4.3 step 5). */
esp_err_t event_manager_start(void);
esp_err_t event_manager_stop(void);

/* ---- the four registries ------------------------------------------------------- */

esp_err_t event_manager_declare_source(const em_source_decl_t *decl);
esp_err_t event_manager_register_action(const em_action_t *action);

/**
 * Register a pull-value provider: templates resolve `${<name>}` through
 * it at dispatch time (dispatcher context, bounded; NO flash access).
 * The provider RENDERS the value as a string (numbers formatted by the
 * owner — templates paste raw). A @p name ending in '.' registers a
 * PREFIX: `${autopid.rpm}` resolves through the "autopid." provider,
 * which receives the FULL name. Exact names win over prefixes.
 */
typedef esp_err_t (*em_value_read_t)(const char *name, char *out,
                                     size_t out_len);
esp_err_t event_manager_register_value(const char *name,
                                       em_value_read_t read);

/** Copy @p ev into the queue (never blocks; drop-oldest when full).
 *  ts_us is stamped here when the caller left it 0. Any task, not ISRs. */
esp_err_t event_manager_publish(const em_event_t *ev);

/* ---- helpers for publishers ----------------------------------------------------- */

/** Convenience kv constructors (value copied; key must be static). */
em_kv_t em_kv_f64(const char *key, double v);
em_kv_t em_kv_i64(const char *key, int64_t v);
em_kv_t em_kv_bool(const char *key, bool v);
em_kv_t em_kv_str(const char *key, const char *v);   /* truncates at 47 */

/**
 * Enabled rules' `match` values for one selector/key — how a source
 * derives its pre-filter (the hot-source rule): mqtt asks for
 * (`"mqtt.rx"`, `"topic"`) at start and subscribes ONLY those topics.
 * Valid after the settings boot pass. @return entries written.
 */
int event_manager_rule_match_values(const char *selector, const char *key,
                                    char (*out)[64], int max);

/* ---- observability ---------------------------------------------------------------- */

typedef struct
{
    uint32_t published;
    uint32_t dropped;           /* event queue full (oldest evicted)       */
    uint32_t fired;             /* rule matched + action ran OK            */
    uint32_t action_errors;
    uint32_t suppressed;        /* cooldown holds                          */
    uint32_t blocking_dropped;  /* worker-pool job queue full (oldest)     */
    uint16_t sources, actions, values, rules, timers;
    bool     running;
} event_manager_stats_t;

esp_err_t event_manager_stats(event_manager_stats_t *out);

/** Registry occupancy for the WICAN CAPS boot line / health surface
 *  (standard §12): sources and actions are the cross-component tables. */
void event_manager_capacity(size_t *src_used, size_t *src_cap,
                            size_t *act_used, size_t *act_cap);

/** The /api/events discovery + log routes (own-routes pattern §9.1). */
esp_err_t event_manager_register_http(void);

#ifdef __cplusplus
}
#endif
