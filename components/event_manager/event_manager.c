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
 * @file event_manager.c
 * @brief The engine half: the bounded event queue + ONE dispatcher
 *        task, rule evaluation, the built-in timer source.
 *        Registries/ring/JSON live in event_manager_registry.c; the
 *        settings descriptor + parsed config in event_manager_settings.c
 *        (700-cap split).
 *
 * Ownership inversion (bridge_manager pattern): components register in
 * via <comp>_events.c; this file knows no component by name. Registration
 * calls are BOOT-CONTEXT ONLY (plain array writes). Publish is any-task,
 * never blocks (drop-oldest + counter). Actions run in the dispatcher
 * ONLY (8 KB PSRAM stack — no flash access in handlers, standard §2).
 * All time is esp_timer 64-bit µs.
 */
#include "event_manager.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "log_manager.h"

#include "event_manager_private.h"

static const char *TAG = "event_manager";

#define EM_QUEUE_LEN 32

/* bounded worker pool for `blocking` actions (network/bus) — keeps a slow
   action off the single dispatcher. Fixed cost regardless of event rate. */
#define EM_WORKERS       2
#define EM_JOB_QUEUE     8
#define EM_WORKER_STACK  4096
#define EM_RECHECK_US    1000000 /* live re-check of active undo rules */

/* ---- runtime halves of the settings-applied config --------------------------------
        event_manager_settings.c owns the PARSED config (rules, timer periods,
        enabled) — this file owns what mutates at runtime: per-rule state,
        timer handles, the stop latch. Both are index-aligned with the
        settings tables. ------------------------------------------------------------- */

static em_rule_state_t s_rule_state[EM_RULES_MAX] EXT_RAM_BSS_ATTR;
static esp_timer_handle_t s_timer_handles[EM_TIMERS_MAX];
static bool s_stopped; /* stop() latches this; init-once, no teardown (§3) */

/* ---- queue / task / ring ----------------------------------------------------------- */

static QueueHandle_t s_q;
static StaticQueue_t s_q_buf;                      /* internal object    */
static uint8_t s_q_store[EM_QUEUE_LEN * sizeof(em_event_t)]
    EXT_RAM_BSS_ATTR;

static TaskHandle_t s_task;
static StaticTask_t s_tcb;                         /* internal object    */
static StackType_t  s_stack[8192] EXT_RAM_BSS_ATTR;

/* worker pool: a job = a rendered blocking action + its trigger. The
   `with` cJSON's ownership is TRANSFERRED into the job (worker frees it). */
typedef struct
{
    const em_action_t *action;
    cJSON             *with;       /* owned by the job                    */
    em_event_t         trigger;
    int                rule;       /* index, for the per-rule counters    */
} em_job_t;

static QueueHandle_t s_jobq;
static StaticQueue_t s_jobq_buf;                   /* internal object    */
static uint8_t s_jobq_store[EM_JOB_QUEUE * sizeof(em_job_t)]
    EXT_RAM_BSS_ATTR;

static TaskHandle_t s_worker[EM_WORKERS];
static StaticTask_t s_worker_tcb[EM_WORKERS];      /* internal objects   */
static StackType_t  s_worker_stack[EM_WORKERS][EM_WORKER_STACK]
    EXT_RAM_BSS_ATTR;

static event_manager_stats_t s_stats;
/* `fired`/`action_errors` are bumped by the dispatcher (inline actions)
   AND the workers → guard those two with this spinlock. Other counters
   are single-writer. */
static portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_started;

static inline void stat_bump(uint32_t *counter)
{
    portENTER_CRITICAL(&s_stats_mux);
    (*counter)++;
    portEXIT_CRITICAL(&s_stats_mux);
}

/** Per-rule counters (the list's "fired 3x, 2 min ago"): dispatcher
 *  (inline actions) and workers both bump them. */
static void rule_fired(int r)
{
    if (r < 0 || r >= EM_RULES_MAX)
    {
        return;
    }

    portENTER_CRITICAL(&s_stats_mux);
    s_rule_state[r].fired++;
    s_rule_state[r].last_fired_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_stats_mux);
}

/* ---- publish -------------------------------------------------------------------------- */

esp_err_t event_manager_publish(const em_event_t *ev)
{
    if (ev == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_q == NULL || !em_settings_enabled() || s_stopped)
    {
        return ESP_ERR_INVALID_STATE;
    }

    em_event_t copy = *ev;

    if (copy.ts_us == 0)
    {
        copy.ts_us = esp_timer_get_time();
    }

    if (xQueueSend(s_q, &copy, 0) != pdTRUE)
    {
        /* drop-OLDEST: automation prefers the newest occurrence */
        em_event_t victim;

        (void)xQueueReceive(s_q, &victim, 0);
        s_stats.dropped++;

        if (xQueueSend(s_q, &copy, 0) != pdTRUE)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    s_stats.published++;
    return ESP_OK;
}

/* ---- rule-table introspection (hot-source pre-filters) ------------------------------ */

int event_manager_rule_match_values(const char *selector, const char *key,
                                    char (*out)[64], int max)
{
    const em_rule_t *rules = em_settings_rules();
    int n = 0;

    for (int r = 0; r < em_rule_count() && n < max; r++)
    {
        if (!rules[r].enabled ||
            strcmp(rules[r].on, selector) != 0)
        {
            continue;
        }

        for (uint8_t m = 0; m < rules[r].n_match; m++)
        {
            if (strcmp(rules[r].match[m].key, key) == 0 &&
                !rules[r].match[m].is_num)
            {
                bool dup = false;

                for (int i = 0; i < n; i++)
                {
                    if (strcmp(out[i], rules[r].match[m].str) == 0)
                    {
                        dup = true;
                        break;
                    }
                }

                if (!dup && n < max)
                {
                    snprintf(out[n], 64, "%s", rules[r].match[m].str);
                    n++;
                }
            }
        }
    }

    return n;
}

/* ---- dispatch ---------------------------------------------------------------------------- */

/** Render ${..} templates in every string leaf of @p node (in place). */
static void render_with(cJSON *node, const em_event_t *ev, char *buf,
                        size_t buf_len)
{
    cJSON *child = NULL;

    cJSON_ArrayForEach(child, node)
    {
        if (cJSON_IsString(child))
        {
            if (strstr(child->valuestring, "${") != NULL &&
                em_template_render(child->valuestring, ev,
                                   em_value_resolve, buf,
                                   buf_len) == ESP_OK)
            {
                cJSON_SetValuestring(child, buf);
            }
        }
        else if (cJSON_IsObject(child) || cJSON_IsArray(child))
        {
            render_with(child, ev, buf, buf_len);
        }
    }
}

/** Hand a rendered blocking action to the worker pool. Takes ownership of
 *  job->with. Never blocks the dispatcher: on a full queue it drops the
 *  OLDEST job (freeing its with) then enqueues the new one. Returns false
 *  only if the job could not be queued at all (caller frees its with). */
static bool enqueue_job(em_job_t *job)
{
    if (s_jobq == NULL)
    {
        return false;
    }

    if (xQueueSend(s_jobq, job, 0) == pdTRUE)
    {
        return true;
    }

    em_job_t victim;

    if (xQueueReceive(s_jobq, &victim, 0) == pdTRUE)
    {
        cJSON_Delete(victim.with);
        s_stats.blocking_dropped++; /* dispatcher-only counter */
    }

    return xQueueSend(s_jobq, job, 0) == pdTRUE;
}

static void worker_task(void *arg)
{
    (void)arg;

    while (true)
    {
        em_job_t job;

        if (xQueueReceive(s_jobq, &job, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        esp_err_t err = job.action->run(job.with, &job.trigger);

        cJSON_Delete(job.with);

        if (err == ESP_OK)
        {
            stat_bump(&s_stats.fired);
            rule_fired(job.rule);
        }
        else
        {
            ESP_LOGW(TAG, "action %s failed (%s)", job.action->name,
                     esp_err_to_name(err));
            stat_bump(&s_stats.action_errors);
        }
    }
}

/** Render the rule's `with` and run (inline) or queue (blocking) its
 *  action; @p undo adds "undo":true so an undoable action reverses
 *  itself. True = ran OK or was queued (the ring marks it fired). */
static bool run_action(const em_rule_t *rule, int r, const em_event_t *ev,
                       bool undo)
{
    const em_action_t *action = em_find_action(rule->action);

    if (action == NULL)
    {
        ESP_LOGW(TAG, "%s: unknown action '%s'", rule->name,
                 rule->action);
        s_stats.action_errors++;
        return false;
    }

    static char s_render[2048] EXT_RAM_BSS_ATTR; /* dispatcher only */
    cJSON *with = (rule->with_json[0] != '\0')
                      ? cJSON_Parse(rule->with_json)
                      : cJSON_CreateObject();

    if (with == NULL)
    {
        s_stats.action_errors++;
        return false;
    }

    render_with(with, ev, s_render, sizeof(s_render));

    if (undo)
    {
        cJSON_AddBoolToObject(with, "undo", true);
        ESP_LOGI(TAG, "%s: conditions no longer hold, undoing %s",
                 rule->name, rule->action);
    }

    if (action->blocking)
    {
        /* offload to the worker pool — ownership of `with` moves to
           the job; do NOT delete it here. Marks the ring as
           dispatched; the worker records the real fired/error stat. */
        em_job_t job = { .action = action, .with = with,
                         .trigger = *ev, .rule = r };

        if (enqueue_job(&job))
        {
            return true;
        }

        cJSON_Delete(with); /* couldn't queue at all */
        s_stats.blocking_dropped++;
        return false;
    }

    esp_err_t err = action->run(with, ev);

    cJSON_Delete(with);

    if (err == ESP_OK)
    {
        stat_bump(&s_stats.fired);
        rule_fired(r);
        return true;
    }

    ESP_LOGW(TAG, "%s: action %s failed (%s)", rule->name, rule->action,
             esp_err_to_name(err));
    stat_bump(&s_stats.action_errors);
    return false;
}

static void dispatch_one(const em_event_t *ev)
{
    char selector[EM_SEL_LEN];
    uint16_t fired = 0;

    snprintf(selector, sizeof(selector), "%s.%s", ev->source, ev->name);

    const em_rule_t *rules = em_settings_rules();
    int64_t now = esp_timer_get_time();

    for (int r = 0; r < em_rule_count(); r++)
    {
        const em_rule_t *rule = &rules[r];
        em_rule_state_t *st = &s_rule_state[r];
        bool suppressed = false;

        /* the decision is pure (em_rule_decide, host-tested with whole
           scenarios); this loop only runs actions and keeps the stats */
        switch (em_rule_decide(rule, st, ev, selector, now, em_value_resolve,
                               &suppressed))
        {
            case EM_STEP_RUN:
                if (run_action(rule, r, ev, false))
                {
                    em_rule_applied(rule, st);
                    fired |= (uint16_t)(1u << r);
                }
                break;
            case EM_STEP_UNDO:
                if (run_action(rule, r, ev, true))
                {
                    fired |= (uint16_t)(1u << r);
                }
                break;
            default:
                break;
        }

        if (suppressed)
        {
            s_stats.suppressed++;
        }
    }

    em_ring_append(ev, fired);
}

/** Between events: an ACTIVE undo rule whose live-value conditions no
 *  longer hold gets its undo — its trigger may stay silent (SOC drifting
 *  down while CHARGING never changes). Runs every EM_RECHECK_US. */
static void recheck_active(void)
{
    const em_rule_t *rules = em_settings_rules();

    for (int r = 0; r < em_rule_count(); r++)
    {
        em_rule_state_t *st = &s_rule_state[r];

        if (em_rule_recheck(&rules[r], st, em_value_resolve))
        {
            (void)run_action(&rules[r], r, &st->last_ev, true);
        }
    }
}

cJSON *em_core_rules_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    const em_rule_t *rules = em_settings_rules();
    int64_t now = esp_timer_get_time();

    for (int r = 0; r < em_rule_count() && arr != NULL; r++)
    {
        const em_rule_state_t *st = &s_rule_state[r];
        cJSON *o = cJSON_CreateObject();

        cJSON_AddStringToObject(o, "name", rules[r].name);
        cJSON_AddBoolToObject(o, "enabled", rules[r].enabled);
        cJSON_AddBoolToObject(o, "undo", rules[r].undo);
        cJSON_AddBoolToObject(o, "active", st->active);
        cJSON_AddNumberToObject(o, "fired", st->fired);
        cJSON_AddNumberToObject(o, "last_fired_age_s",
                                st->last_fired_us
                                    ? (double)((now - st->last_fired_us)
                                               / 1000000)
                                    : -1);
        cJSON_AddItemToArray(arr, o);
    }

    return arr;
}

static void dispatcher_task(void *arg)
{
    (void)arg;

    int64_t last_check_us = 0;

    while (true)
    {
        em_event_t ev;

        if (xQueueReceive(s_q, &ev, pdMS_TO_TICKS(500)) == pdTRUE)
        {
            dispatch_one(&ev);
        }

        int64_t now = esp_timer_get_time();

        if (now - last_check_us >= EM_RECHECK_US)
        {
            last_check_us = now;
            recheck_active();
        }
    }
}

/* ---- built-ins: the timer source + log.note ------------------------------------------ */

static void timer_cb(void *arg)
{
    const em_timer_cfg_t *t = (const em_timer_cfg_t *)arg;
    em_event_t ev = { 0 };

    snprintf(ev.source, sizeof(ev.source), "timer");
    snprintf(ev.name, sizeof(ev.name), "tick");
    ev.kv[0] = em_kv_str("timer", t->name);
    ev.n = 1;
    (void)event_manager_publish(&ev);
}

static esp_err_t act_log_note(const cJSON *with, const em_event_t *ev)
{
    const cJSON *msg = cJSON_GetObjectItemCaseSensitive(with, "message");

    ESP_LOGI(TAG, "note: %s (on %s.%s)",
             cJSON_IsString(msg) ? msg->valuestring : "(no message)",
             ev->source, ev->name);
    return ESP_OK;
}

/* ---- lifecycle ------------------------------------------------------------------------------ */

esp_err_t event_manager_init(void)
{
    static const log_descriptor_t LOG_DESC =
        { "event_manager", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);

    if (s_q == NULL)
    {
        s_q = xQueueCreateStatic(EM_QUEUE_LEN, sizeof(em_event_t),
                                 s_q_store, &s_q_buf);
    }

    if (s_jobq == NULL)
    {
        s_jobq = xQueueCreateStatic(EM_JOB_QUEUE, sizeof(em_job_t),
                                    s_jobq_store, &s_jobq_buf);
    }

    /* built-ins */
    static const em_key_decl_t TICK_KEYS[] =
    {
        { "timer", EM_VAL_STR },
    };
    static const em_source_decl_t TICK =
    {
        .source = "timer", .name = "tick",
        .description = "settings-defined periodic timers",
        .keys = TICK_KEYS, .n_keys = 1,
    };
    static const em_action_t LOG_NOTE =
    {
        .name = "log.note",
        .params_schema = "{\"type\":\"object\",\"properties\":{"
                         "\"message\":{\"type\":\"string\"}}}",
        .run = act_log_note,
    };

    (void)event_manager_declare_source(&TICK);
    (void)event_manager_register_action(&LOG_NOTE);

    return em_settings_register();
}

esp_err_t event_manager_start(void)
{
    if (!em_settings_is_configured())
    {
        ESP_LOGE(TAG, "unconfigured; not starting");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_started)
    {
        return ESP_OK;
    }

    s_task = xTaskCreateStatic(dispatcher_task, "em_dispatch",
                               sizeof(s_stack) / sizeof(s_stack[0]),
                               NULL, 4, s_stack, &s_tcb);

    if (s_task == NULL)
    {
        return ESP_FAIL;
    }

    /* worker pool for blocking actions (priority just below the
       dispatcher so it drains events first) */
    for (int i = 0; i < EM_WORKERS; i++)
    {
        s_worker[i] = xTaskCreateStatic(worker_task, "em_worker",
                                        EM_WORKER_STACK, NULL, 3,
                                        s_worker_stack[i],
                                        &s_worker_tcb[i]);

        if (s_worker[i] == NULL)
        {
            ESP_LOGE(TAG, "worker %d create failed", i);
        }
    }

    for (int i = 0; i < em_settings_timer_count(); i++)
    {
        const em_timer_cfg_t *cfg = em_settings_timer_cfg(i);
        const esp_timer_create_args_t args =
        {
            .callback = timer_cb,
            .arg = (void *)cfg,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "em_timer",
        };

        if (esp_timer_create(&args, &s_timer_handles[i]) == ESP_OK)
        {
            (void)esp_timer_start_periodic(
                s_timer_handles[i],
                (uint64_t)cfg->period_s * 1000000ULL);
        }
    }

    s_started = true;
    s_stats.running = true;
    ESP_LOGI(TAG,
             "started (%d sources, %d actions, %d values, %d rules, "
             "%d timers)",
             em_source_count(), em_action_count(), em_value_count(),
             em_rule_count(), em_settings_timer_count());
    return ESP_OK;
}

esp_err_t event_manager_stop(void)
{
    for (int i = 0; i < em_settings_timer_count(); i++)
    {
        if (s_timer_handles[i] != NULL)
        {
            esp_timer_stop(s_timer_handles[i]);
        }
    }

    s_stopped = true; /* the task idles; init-once, no teardown (§3) */
    s_stats.running = false;
    return ESP_OK;
}

esp_err_t event_manager_stats(event_manager_stats_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out = s_stats;
    out->sources = (uint16_t)em_source_count();
    out->actions = (uint16_t)em_action_count();
    out->values = (uint16_t)em_value_count();
    out->rules = em_rule_count();
    out->timers = em_settings_timer_count();
    out->running = s_started && em_settings_enabled() && !s_stopped;
    return ESP_OK;
}
