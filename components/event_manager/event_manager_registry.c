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
 * @file event_manager_registry.c
 * @brief The registry half: sources/actions/pull-values (boot-written,
 *        read-only after), the em_kv_t constructors, the event log ring,
 *        and the JSON builders for the /api/events surface. The engine
 *        half (event_manager.c) owns the queue/dispatcher/rules.
 */
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "event_manager_private.h"

static const char *TAG = "event_manager";

static em_source_decl_t s_sources[EM_SOURCES_MAX] EXT_RAM_BSS_ATTR;
static int s_n_sources;

static em_action_t s_actions[EM_ACTIONS_MAX] EXT_RAM_BSS_ATTR;
static int s_n_actions;

static struct
{
    const char     *name;       /* exact, or prefix when ending in '.'   */
    em_value_read_t read;
} s_values[EM_VALUES_MAX] EXT_RAM_BSS_ATTR;
static int s_n_values;

typedef struct
{
    em_event_t ev;
    uint16_t   fired;           /* bitmask of rule indices that fired    */
} em_ring_entry_t;

static em_ring_entry_t s_ring[EM_RING] EXT_RAM_BSS_ATTR;
static uint32_t s_ring_next;
static SemaphoreHandle_t s_ring_lock;
static StaticSemaphore_t s_ring_lock_buf;          /* internal object    */

/* ---- registration ------------------------------------------------------------------- */

esp_err_t event_manager_declare_source(const em_source_decl_t *decl)
{
    if (decl == NULL || decl->source == NULL || decl->name == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_n_sources >= EM_SOURCES_MAX)
    {
        ESP_LOGE(TAG, "source registry full (%s.%s)", decl->source,
                 decl->name);
        return ESP_ERR_NO_MEM;
    }

    s_sources[s_n_sources++] = *decl;
    return ESP_OK;
}

esp_err_t event_manager_register_action(const em_action_t *action)
{
    if (action == NULL || action->name == NULL || action->run == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_n_actions >= EM_ACTIONS_MAX)
    {
        ESP_LOGE(TAG, "action registry full (%s)", action->name);
        return ESP_ERR_NO_MEM;
    }

    s_actions[s_n_actions++] = *action;
    return ESP_OK;
}

esp_err_t event_manager_register_value(const char *name,
                                       em_value_read_t read)
{
    if (name == NULL || read == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_n_values >= EM_VALUES_MAX)
    {
        ESP_LOGE(TAG, "value registry full (%s)", name);
        return ESP_ERR_NO_MEM;
    }

    s_values[s_n_values].name = name;
    s_values[s_n_values].read = read;
    s_n_values++;
    return ESP_OK;
}

/* ---- lookups -------------------------------------------------------------------------- */

const em_action_t *em_find_action(const char *name)
{
    for (int i = 0; i < s_n_actions; i++)
    {
        if (strcmp(s_actions[i].name, name) == 0)
        {
            return &s_actions[i];
        }
    }

    return NULL;
}

esp_err_t em_value_resolve(const char *name, char *out, size_t out_len)
{
    /* exact names win over prefixes */
    for (int i = 0; i < s_n_values; i++)
    {
        if (strcmp(s_values[i].name, name) == 0)
        {
            return s_values[i].read(name, out, out_len);
        }
    }

    for (int i = 0; i < s_n_values; i++)
    {
        size_t plen = strlen(s_values[i].name);

        if (s_values[i].name[plen - 1] == '.' &&
            strncmp(s_values[i].name, name, plen) == 0)
        {
            return s_values[i].read(name, out, out_len);
        }
    }

    return ESP_ERR_NOT_FOUND;
}

bool em_selector_known(const char *selector)
{
    for (int s = 0; s < s_n_sources; s++)
    {
        char sel[EM_SEL_LEN];

        snprintf(sel, sizeof(sel), "%s.%s", s_sources[s].source,
                 s_sources[s].name);

        if (strcmp(sel, selector) == 0)
        {
            return true;
        }
    }

    return false;
}

void event_manager_capacity(size_t *src_used, size_t *src_cap,
                            size_t *act_used, size_t *act_cap)
{
    if (src_used != NULL)
    {
        *src_used = (size_t)s_n_sources;
    }

    if (src_cap != NULL)
    {
        *src_cap = EM_SOURCES_MAX;
    }

    if (act_used != NULL)
    {
        *act_used = (size_t)s_n_actions;
    }

    if (act_cap != NULL)
    {
        *act_cap = EM_ACTIONS_MAX;
    }
}

int em_source_count(void) { return s_n_sources; }
int em_action_count(void) { return s_n_actions; }
int em_value_count(void)  { return s_n_values; }

/* ---- kv constructors --------------------------------------------------------------- */

em_kv_t em_kv_f64(const char *key, double v)
{
    em_kv_t kv = { .key = key, .type = EM_VAL_F64 };

    kv.v.f64 = v;
    return kv;
}

em_kv_t em_kv_i64(const char *key, int64_t v)
{
    em_kv_t kv = { .key = key, .type = EM_VAL_I64 };

    kv.v.i64 = v;
    return kv;
}

em_kv_t em_kv_bool(const char *key, bool v)
{
    em_kv_t kv = { .key = key, .type = EM_VAL_BOOL };

    kv.v.b = v;
    return kv;
}

em_kv_t em_kv_str(const char *key, const char *v)
{
    em_kv_t kv = { .key = key, .type = EM_VAL_STR };

    snprintf(kv.v.str, sizeof(kv.v.str), "%s", (v != NULL) ? v : "");
    return kv;
}

/* ---- the event log ring --------------------------------------------------------------- */

void em_ring_append(const em_event_t *ev, uint16_t fired)
{
    if (s_ring_lock == NULL)
    {
        s_ring_lock = xSemaphoreCreateMutexStatic(&s_ring_lock_buf);
    }

    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    s_ring[s_ring_next % EM_RING].ev = *ev;
    s_ring[s_ring_next % EM_RING].fired = fired;
    s_ring_next++;
    xSemaphoreGive(s_ring_lock);
}

/* ---- JSON builders (the /api/events surface) ------------------------------------------ */

cJSON *em_core_sources_json(void)
{
    cJSON *arr = cJSON_CreateArray();

    for (int i = 0; i < s_n_sources && arr != NULL; i++)
    {
        cJSON *o = cJSON_CreateObject();
        char sel[EM_SEL_LEN];

        snprintf(sel, sizeof(sel), "%s.%s", s_sources[i].source,
                 s_sources[i].name);
        cJSON_AddStringToObject(o, "event", sel);
        cJSON_AddStringToObject(o, "description",
                                s_sources[i].description
                                    ? s_sources[i].description : "");

        cJSON *keys = cJSON_AddArrayToObject(o, "keys");
        static const char *const TYPES[] =
        {
            "number", "integer", "string", "boolean",
        };

        for (uint8_t k = 0; k < s_sources[i].n_keys; k++)
        {
            cJSON *ko = cJSON_CreateObject();

            cJSON_AddStringToObject(ko, "key", s_sources[i].keys[k].key);
            cJSON_AddStringToObject(ko, "type",
                                    TYPES[s_sources[i].keys[k].type]);
            cJSON_AddItemToArray(keys, ko);
        }

        cJSON_AddItemToArray(arr, o);
    }

    return arr;
}

cJSON *em_core_actions_json(void)
{
    cJSON *arr = cJSON_CreateArray();

    for (int i = 0; i < s_n_actions && arr != NULL; i++)
    {
        cJSON *o = cJSON_CreateObject();

        cJSON_AddStringToObject(o, "name", s_actions[i].name);

        cJSON *schema =
            s_actions[i].params_schema
                ? cJSON_Parse(s_actions[i].params_schema) : NULL;

        cJSON_AddItemToObject(o, "params_schema",
                              schema ? schema : cJSON_CreateObject());
        cJSON_AddBoolToObject(o, "undoable", s_actions[i].undoable);
        cJSON_AddItemToArray(arr, o);
    }

    return arr;
}

cJSON *em_core_values_json(void)
{
    cJSON *arr = cJSON_CreateArray();

    for (int i = 0; i < s_n_values && arr != NULL; i++)
    {
        cJSON_AddItemToArray(arr, cJSON_CreateString(s_values[i].name));
    }

    return arr;
}

cJSON *em_core_log_json(void)
{
    cJSON *o = cJSON_CreateObject();

    if (o == NULL)
    {
        return NULL;
    }

    event_manager_stats_t st;

    (void)event_manager_stats(&st);

    cJSON *stats = cJSON_AddObjectToObject(o, "stats");

    cJSON_AddBoolToObject(stats, "running", st.running);
    cJSON_AddNumberToObject(stats, "published", st.published);
    cJSON_AddNumberToObject(stats, "dropped", st.dropped);
    cJSON_AddNumberToObject(stats, "fired", st.fired);
    cJSON_AddNumberToObject(stats, "action_errors", st.action_errors);
    cJSON_AddNumberToObject(stats, "suppressed", st.suppressed);
    cJSON_AddNumberToObject(stats, "blocking_dropped", st.blocking_dropped);

    cJSON *events = cJSON_AddArrayToObject(o, "events");

    if (s_ring_lock == NULL)
    {
        return o;
    }

    xSemaphoreTake(s_ring_lock, portMAX_DELAY);

    uint32_t count = (s_ring_next < EM_RING) ? s_ring_next : EM_RING;

    for (uint32_t i = 0; i < count; i++)
    {
        /* oldest first */
        const em_ring_entry_t *e =
            &s_ring[(s_ring_next - count + i) % EM_RING];
        cJSON *eo = cJSON_CreateObject();

        cJSON_AddStringToObject(eo, "source", e->ev.source);
        cJSON_AddStringToObject(eo, "name", e->ev.name);
        cJSON_AddNumberToObject(eo, "ts_us", (double)e->ev.ts_us);

        cJSON *kv = cJSON_AddObjectToObject(eo, "data");

        for (uint8_t k = 0; k < e->ev.n && k < EM_KV_MAX; k++)
        {
            const em_kv_t *item = &e->ev.kv[k];

            switch (item->type)
            {
                case EM_VAL_F64:
                    cJSON_AddNumberToObject(kv, item->key, item->v.f64);
                    break;
                case EM_VAL_I64:
                    cJSON_AddNumberToObject(kv, item->key,
                                            (double)item->v.i64);
                    break;
                case EM_VAL_BOOL:
                    cJSON_AddBoolToObject(kv, item->key, item->v.b);
                    break;
                default:
                    cJSON_AddStringToObject(kv, item->key, item->v.str);
                    break;
            }
        }

        cJSON *fired = cJSON_AddArrayToObject(eo, "fired");

        for (int r = 0; r < em_rule_count(); r++)
        {
            if (e->fired & (1u << r))
            {
                cJSON_AddItemToArray(fired,
                                     cJSON_CreateString(em_rule_name(r)));
            }
        }

        cJSON_AddItemToArray(events, eo);
    }

    xSemaphoreGive(s_ring_lock);
    return o;
}
