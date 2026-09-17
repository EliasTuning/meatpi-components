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
 * @file autopid_events.c
 * @brief event_manager glue (Phase 3, TASK_autopid.md §8): the
 *        `autopid.param` / `autopid.pid_failed` / `autopid.scan_done`
 *        events, the `autopid.group` action (§5b context switching),
 *        and the `${autopid.*}` pull values.
 *
 * autopid.param emission is ON CHANGE + per-parameter
 * `min_event_interval_ms` (settings; schema minimum 10 ms): a value
 * that keeps repeating emits nothing; a changing value emits at most
 * once per interval. Poller/filter task context — publish never blocks.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_timer.h"

#include "event_manager.h"

#include "autopid.h"
#include "autopid_private.h"

/* per-slot emission memory (indexed like the value cache) */
typedef struct
{
    double  value;
    int64_t last_emit_us;
    bool    valid;
} ap_emit_slot_t;

static ap_emit_slot_t s_emit[AP_MAX_PARAMS] EXT_RAM_BSS_ATTR;

/* the ONE value sink (data_logger's autopid_log path — main wires it);
 * fed every accepted sample ahead of the event gating below */
static autopid_value_sink_t s_value_sink;

void autopid_set_value_sink(autopid_value_sink_t sink)
{
    s_value_sink = sink;
}

void ap_events_reset(void)
{
    memset(s_emit, 0, sizeof(s_emit));
}

void ap_events_param(const ap_param_t *prm, uint16_t slot, int group,
                     double value)
{
    ap_emit_slot_t *e = &s_emit[slot % AP_MAX_PARAMS];
    int64_t now = esp_timer_get_time();

    if (s_value_sink != NULL)
    {
        s_value_sink(prm->name, prm->unit, value,
                     !(e->valid && e->value == value));
    }

    if (e->valid && e->value == value)
    {
        return;                 /* on-change only                       */
    }

    if (e->valid &&
        now - e->last_emit_us <
            (int64_t)ap_core_min_event_interval_ms() * 1000)
    {
        return;                 /* rate floor (min 10 ms via schema)    */
    }

    e->value = value;
    e->last_emit_us = now;
    e->valid = true;

    const ap_config_t *cfg = ap_core_config();
    em_event_t ev = { 0 };

    snprintf(ev.source, sizeof(ev.source), "autopid");
    snprintf(ev.name, sizeof(ev.name), "param");
    ev.kv[0] = em_kv_str("param", prm->name);
    ev.kv[1] = em_kv_f64("value", value);
    ev.kv[2] = em_kv_str("unit", prm->unit);
    ev.kv[3] = em_kv_str("group",
                         (group >= 0 && group < cfg->n_groups)
                             ? cfg->groups[group].name : "");
    ev.n = 4;
    (void)event_manager_publish(&ev);
}

void ap_events_external(const char *name, const char *unit, double value,
                        bool changed)
{
    /* injected values (GPS) take the SAME downstream as a polled sample:
       fire the value sink every time, emit autopid.param on change. The
       on-change/rate gating lives in ap_ext_put's `changed` flag — no
       s_emit slot (externals aren't config-indexed). */
    if (s_value_sink != NULL)
    {
        s_value_sink(name, unit, value, changed);
    }

    if (!changed)
    {
        return;
    }

    em_event_t ev = { 0 };

    snprintf(ev.source, sizeof(ev.source), "autopid");
    snprintf(ev.name, sizeof(ev.name), "param");
    ev.kv[0] = em_kv_str("param", name);
    ev.kv[1] = em_kv_f64("value", value);
    ev.kv[2] = em_kv_str("unit", unit);
    ev.kv[3] = em_kv_str("group", "external");
    ev.n = 4;
    (void)event_manager_publish(&ev);
}

void ap_events_pid_failed(const char *name, uint16_t streak)
{
    em_event_t ev = { 0 };

    snprintf(ev.source, sizeof(ev.source), "autopid");
    snprintf(ev.name, sizeof(ev.name), "pid_failed");
    ev.kv[0] = em_kv_str("pid", name);
    ev.kv[1] = em_kv_i64("streak", streak);
    ev.n = 2;
    (void)event_manager_publish(&ev);
}

void ap_events_scan_done(uint16_t found)
{
    em_event_t ev = { 0 };

    snprintf(ev.source, sizeof(ev.source), "autopid");
    snprintf(ev.name, sizeof(ev.name), "scan_done");
    ev.kv[0] = em_kv_i64("found", found);
    ev.n = 1;
    (void)event_manager_publish(&ev);
}

/* ---- the autopid.group action (§5b: rules switch polling contexts) ------------ */

static esp_err_t act_group(const cJSON *with, const em_event_t *trigger)
{
    (void)trigger;

    const cJSON *group = cJSON_GetObjectItemCaseSensitive(with, "group");
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(with,
                                                            "enabled");
    const cJSON *period = cJSON_GetObjectItemCaseSensitive(with,
                                                           "period_ms");

    if (!cJSON_IsString(group))
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* the engine's undo of a "while" rule: back to the configured state */
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(with, "undo")))
    {
        return autopid_group_restore(group->valuestring);
    }

    if (!cJSON_IsBool(enabled))
    {
        return ESP_ERR_INVALID_ARG;
    }

    return autopid_group_set(group->valuestring, cJSON_IsTrue(enabled),
                             cJSON_IsNumber(period)
                                 ? (int32_t)period->valuedouble : -1);
}

/* ---- DTC events + actions (TASK_dtc.md §7) ------------------------------------- */

void ap_events_dtc_new_code(const char *code, const char *status, bool mil)
{
    em_event_t ev = { 0 };

    snprintf(ev.source, sizeof(ev.source), "autopid");
    snprintf(ev.name, sizeof(ev.name), "dtc");
    ev.kv[0] = em_kv_str("code", code);
    ev.kv[1] = em_kv_str("status", status);
    ev.kv[2] = em_kv_bool("mil", mil);
    ev.n = 3;
    (void)event_manager_publish(&ev);
}

void ap_events_dtc_scan(const ap_dtc_report_t *r, bool ok)
{
    em_event_t ev = { 0 };

    snprintf(ev.source, sizeof(ev.source), "autopid");
    snprintf(ev.name, sizeof(ev.name), "dtc_scan");
    ev.kv[0] = em_kv_bool("ok", ok);
    ev.kv[1] = em_kv_i64("stored", r->n_stored);
    ev.kv[2] = em_kv_i64("pending", r->n_pending);
    ev.kv[3] = em_kv_i64("permanent", r->n_permanent);
    ev.kv[4] = em_kv_i64("new", r->n_new);
    ev.kv[5] = em_kv_bool("mil", r->mil);
    ev.n = 6;
    (void)event_manager_publish(&ev);
}

void ap_events_dtc_clear(bool ok, bool cleared, uint8_t before,
                         uint8_t after)
{
    em_event_t ev = { 0 };

    snprintf(ev.source, sizeof(ev.source), "autopid");
    snprintf(ev.name, sizeof(ev.name), "dtc_clear");
    ev.kv[0] = em_kv_bool("ok", ok);
    ev.kv[1] = em_kv_bool("cleared", cleared);
    ev.kv[2] = em_kv_i64("before", before);
    ev.kv[3] = em_kv_i64("after", after);
    ev.n = 4;
    (void)event_manager_publish(&ev);
}

static esp_err_t act_dtc_scan(const cJSON *with, const em_event_t *trigger)
{
    (void)with;
    (void)trigger;

    /* fire-and-forget: the scan runs on its own job task; busy/disabled
     * outcomes surface through the autopid.dtc_scan event + the log */
    return ap_dtc_scan_start();
}

static esp_err_t act_dtc_clear(const cJSON *with, const em_event_t *trigger)
{
    (void)trigger;

    const cJSON *codes = cJSON_GetObjectItemCaseSensitive(with, "codes");
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(with, "mode");

    /* dispatcher context: NEVER touch the bus here — queue the job */
    return ap_dtc_clear_queue(
        cJSON_IsString(codes) ? codes->valuestring : NULL,
        cJSON_IsString(mode) ? mode->valuestring : NULL);
}

/* ---- pull values: ${autopid.data} + ${autopid.dtc_*} + ${autopid.<param>} ------ */

static esp_err_t dtc_value_read(const char *key, char *out, size_t out_len)
{
    if (strcmp(key, "dtc_json") == 0)
    {
        cJSON *report = ap_dtc_report_json();

        if (report == NULL)
        {
            return ESP_FAIL;
        }

        char *body = cJSON_PrintUnformatted(report);

        cJSON_Delete(report);

        if (body == NULL)
        {
            return ESP_FAIL;
        }

        int n = snprintf(out, out_len, "%s", body);

        cJSON_free(body);
        return (n >= 0 && (size_t)n < out_len) ? ESP_OK
                                               : ESP_ERR_INVALID_SIZE;
    }

    ap_dtc_report_t r;

    if (ap_dtc_report_get(&r) != ESP_OK)
    {
        return ESP_FAIL;
    }

    if (strcmp(key, "dtc_codes") == 0)
    {
        size_t w = 0;

        out[0] = '\0';

        for (uint8_t i = 0; i < r.n_stored && w + AP_DTC_CODE_LEN + 1 <
                                                  out_len; i++)
        {
            w += (size_t)snprintf(out + w, out_len - w, "%s%s",
                                  (i > 0) ? "," : "", r.stored[i]);
        }

        return ESP_OK;
    }

    if (strcmp(key, "dtc_count") == 0)
    {
        snprintf(out, out_len, "%u", r.n_stored);
        return ESP_OK;
    }

    if (strcmp(key, "dtc_mil") == 0)
    {
        snprintf(out, out_len, "%s", r.mil ? "true" : "false");
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

static esp_err_t value_read(const char *name, char *out, size_t out_len)
{
    if (strncmp(name, "autopid.dtc_", 12) == 0)
    {
        return dtc_value_read(name + 8, out, out_len);
    }

    if (strcmp(name, "autopid.data") == 0)
    {
        cJSON *snap = NULL;

        if (autopid_snapshot(&snap) != ESP_OK)
        {
            return ESP_FAIL;
        }

        char *body = cJSON_PrintUnformatted(snap);

        cJSON_Delete(snap);

        if (body == NULL)
        {
            return ESP_FAIL;
        }

        int n = snprintf(out, out_len, "%s", body);

        cJSON_free(body);
        return (n >= 0 && (size_t)n < out_len) ? ESP_OK
                                               : ESP_ERR_INVALID_SIZE;
    }

    /* ${autopid.<param>} — the cached value */
    if (strncmp(name, "autopid.", 8) == 0)
    {
        double value = 0;
        int64_t ts = 0;

        if (autopid_get_value(name + 8, &value, &ts) != ESP_OK)
        {
            return ESP_ERR_NOT_FOUND;
        }

        snprintf(out, out_len, "%g", value);
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

/* ---- registration (autopid_init context) ------------------------------------------ */

void ap_events_register(void)
{
    static const em_key_decl_t PARAM_KEYS[] =
    {
        { "param", EM_VAL_STR },
        { "value", EM_VAL_F64 },
        { "unit", EM_VAL_STR },
        { "group", EM_VAL_STR },
    };
    static const em_source_decl_t PARAM =
    {
        .source = "autopid", .name = "param",
        .description = "a parameter changed (rate-limited by "
                       "min_event_interval_ms)",
        .keys = PARAM_KEYS, .n_keys = 4,
    };
    static const em_key_decl_t FAILED_KEYS[] =
    {
        { "pid", EM_VAL_STR },
        { "streak", EM_VAL_I64 },
    };
    static const em_source_decl_t FAILED =
    {
        .source = "autopid", .name = "pid_failed",
        .description = "a poll failed (streak = consecutive failures)",
        .keys = FAILED_KEYS, .n_keys = 2,
    };
    static const em_key_decl_t SCAN_KEYS[] =
    {
        { "found", EM_VAL_I64 },
    };
    static const em_source_decl_t SCAN =
    {
        .source = "autopid", .name = "scan_done",
        .description = "the standard-PID support scan finished",
        .keys = SCAN_KEYS, .n_keys = 1,
    };
    static const em_action_t GROUP =
    {
        .name = "autopid.group",
        .params_schema =
            "{\"type\":\"object\",\"properties\":{"
            "\"group\":{\"type\":\"string\",\"minLength\":1},"
            "\"enabled\":{\"type\":\"boolean\"},"
            "\"period_ms\":{\"type\":\"integer\",\"minimum\":0}},"
            "\"required\":[\"group\",\"enabled\"]}",
        .run = act_group,
        .undoable = true,
    };

    /* DTC (TASK_dtc.md §7) */
    static const em_key_decl_t DTC_KEYS[] =
    {
        { "code", EM_VAL_STR },
        { "status", EM_VAL_STR },
        { "mil", EM_VAL_BOOL },
    };
    static const em_source_decl_t DTC =
    {
        .source = "autopid", .name = "dtc",
        .description = "a NEW trouble code appeared (once per code per "
                       "scan; re-fires only after a clear/reboot)",
        .keys = DTC_KEYS, .n_keys = 3,
    };
    static const em_key_decl_t DTC_SCAN_KEYS[] =
    {
        { "ok", EM_VAL_BOOL },
        { "stored", EM_VAL_I64 },
        { "pending", EM_VAL_I64 },
        { "permanent", EM_VAL_I64 },
        { "new", EM_VAL_I64 },
        { "mil", EM_VAL_BOOL },
    };
    static const em_source_decl_t DTC_SCAN =
    {
        .source = "autopid", .name = "dtc_scan",
        .description = "a DTC scan finished (ok=false = no ECU response)",
        .keys = DTC_SCAN_KEYS, .n_keys = 6,
    };
    static const em_key_decl_t DTC_CLEAR_KEYS[] =
    {
        { "ok", EM_VAL_BOOL },
        { "cleared", EM_VAL_BOOL },
        { "before", EM_VAL_I64 },
        { "after", EM_VAL_I64 },
    };
    static const em_source_decl_t DTC_CLEAR =
    {
        .source = "autopid", .name = "dtc_clear",
        .description = "a clear attempt finished (cleared=false = "
                       "condition not met)",
        .keys = DTC_CLEAR_KEYS, .n_keys = 4,
    };
    static const em_action_t DTC_SCAN_ACT =
    {
        .name = "autopid.dtc_scan",
        .params_schema = "{\"type\":\"object\",\"properties\":{}}",
        .run = act_dtc_scan,
    };
    static const em_action_t DTC_CLEAR_ACT =
    {
        .name = "autopid.dtc_clear",
        .params_schema =
            "{\"type\":\"object\",\"properties\":{"
            "\"codes\":{\"type\":\"string\","
            "\"description\":\"CSV, e.g. P0420,P0171\"},"
            "\"mode\":{\"type\":\"string\","
            "\"enum\":[\"always\",\"if_any\",\"if_only\"]}}}",
        .run = act_dtc_clear,
    };

    (void)event_manager_declare_source(&PARAM);
    (void)event_manager_declare_source(&FAILED);
    (void)event_manager_declare_source(&SCAN);
    (void)event_manager_declare_source(&DTC);
    (void)event_manager_declare_source(&DTC_SCAN);
    (void)event_manager_declare_source(&DTC_CLEAR);
    (void)event_manager_register_action(&GROUP);
    (void)event_manager_register_action(&DTC_SCAN_ACT);
    (void)event_manager_register_action(&DTC_CLEAR_ACT);
    (void)event_manager_register_value("autopid.data", value_read);
    (void)event_manager_register_value("autopid.", value_read);
}
