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
 * @file event_manager_settings.c
 * @brief settings_manager descriptor for event_manager: field-table schema
 *        (source of truth for shape/ranges/defaults), on_validate (rule
 *        parse + registry checks), on_apply (parses into the boot-applied
 *        rule/timer tables). The engine half (event_manager.c) reads the
 *        applied config through the em_settings_* getters and owns all
 *        runtime state (rule states, timer handles).
 */
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"

#include "settings_manager.h"

#include "event_manager_private.h"

static const char *TAG = "event_manager";

/* clang-format off */
static const settings_field_t TIMER_ITEMS[] =
{
    SETTINGS_STR_REQ("name", 1, 15, ""),
    SETTINGS_INT_REQ("period_s", 1, 604800, 60),
};

/* Rules are free-form objects (validated by em_rules_parse in on_validate);
   the defaults REPLACE main_events.c (design §5): device events ->
   <prefix>/events via the async mqtt path, payload-compatible with the
   legacy glue. */
static const settings_field_t FIELDS[] =
{
    SETTINGS_BOOL ("enabled", true),
    SETTINGS_ARRAY("timers", EM_TIMERS_MAX, TIMER_ITEMS, NULL),
    /* payload values are JSON-in-JSON: the \" below is JSON's own string
       escaping (SETTINGS_JSON removed the C level) */
    SETTINGS_ARRAY_ANY("rules", EM_RULES_MAX,
        SETTINGS_JSON([
            {"name":"bump_event","on":"imu.bump","do":"mqtt.publish",
             "with":{"topic":"~/events","payload":
             "{\"event\":\"bump\",\"axes\":${axes},\"ts\":\"${time.iso}\"}"}},
            {"name":"motion_event","on":"imu.motion","do":"mqtt.publish",
             "with":{"topic":"~/events","payload":
             "{\"event\":\"motion_${state}\",\"ts\":\"${time.iso}\"}"}},
            {"name":"battery_event","on":"battery.threshold",
             "do":"mqtt.publish",
             "with":{"topic":"~/events","payload":
    "{\"event\":\"battery_${edge}\",\"voltage\":${volts},\"ts\":\"${time.iso}\"}"}}
        ])),
};
/* clang-format on */

static em_rule_t s_rules[EM_RULES_MAX] EXT_RAM_BSS_ATTR;
static int  s_n_rules;
static bool s_enabled = true; /* permissive pre-apply (boot-time publish) */

static em_timer_cfg_t s_timers[EM_TIMERS_MAX];
static int  s_n_timers;
static bool s_configured;

bool em_settings_enabled(void)
{
    return s_enabled;
}

bool em_settings_is_configured(void)
{
    return s_configured;
}

const em_rule_t *em_settings_rules(void)
{
    return s_rules;
}

int em_rule_count(void)
{
    return s_n_rules;
}

const char *em_rule_name(int idx)
{
    return (idx >= 0 && idx < s_n_rules) ? s_rules[idx].name : "";
}

const em_timer_cfg_t *em_settings_timer_cfg(int idx)
{
    return (idx >= 0 && idx < s_n_timers) ? &s_timers[idx] : NULL;
}

int em_settings_timer_count(void)
{
    return s_n_timers;
}

static bool action_undoable(const char *name)
{
    const em_action_t *a = em_find_action(name);

    return a != NULL && a->undoable;
}

static esp_err_t on_validate(const cJSON *settings, char *err,
                             size_t err_len)
{
    static em_rule_t probe[EM_RULES_MAX] EXT_RAM_BSS_ATTR;
    int count = 0;

    esp_err_t rc = em_rules_parse(
        cJSON_GetObjectItemCaseSensitive(settings, "rules"), probe,
        EM_RULES_MAX, &count, err, err_len);

    if (rc != ESP_OK)
    {
        return rc;
    }

    /* registry-existence checks only when the registries are populated:
       at the BOOT apply, components register during their own init in
       main's sequence — a PUT at runtime sees everything and is strict */
    for (int i = 0; i < count; i++)
    {
        if (em_action_count() > 0 &&
            em_find_action(probe[i].action) == NULL)
        {
            snprintf(err, err_len, "%s: unknown action '%s'",
                     probe[i].name, probe[i].action);
            return ESP_ERR_INVALID_ARG;
        }

        if (em_source_count() > 0 && !em_selector_known(probe[i].on))
        {
            snprintf(err, err_len, "%s: unknown event '%s'",
                     probe[i].name, probe[i].on);
            return ESP_ERR_INVALID_ARG;
        }
    }

    /* `undo` only on actions that can reverse themselves (pure check, the
       registry lookup injected) — skipped at the boot apply like the rest */
    if (em_action_count() > 0)
    {
        rc = em_rules_validate_undo(probe, count, action_undoable, err,
                                    err_len);

        if (rc != ESP_OK)
        {
            return rc;
        }
    }

    return ESP_OK;
}

static esp_err_t on_apply(const cJSON *settings)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(settings,
                                                      "enabled");

    s_enabled = !cJSON_IsFalse(v);

    char err[96] = "";

    if (em_rules_parse(cJSON_GetObjectItemCaseSensitive(settings,
                                                        "rules"),
                       s_rules, EM_RULES_MAX, &s_n_rules, err,
                       sizeof(err)) != ESP_OK)
    {
        ESP_LOGE(TAG, "rules rejected at apply: %s", err);
        s_n_rules = 0;
    }

    /* timers: store config; the engine arms them in start() (per-rule
       runtime state lives there too and is zero at boot — on_apply always
       runs before the engine's start()) */
    s_n_timers = 0;

    const cJSON *timers = cJSON_GetObjectItemCaseSensitive(settings,
                                                           "timers");
    const cJSON *t = NULL;

    cJSON_ArrayForEach(t, timers)
    {
        if (s_n_timers >= EM_TIMERS_MAX)
        {
            break;
        }

        const cJSON *name = cJSON_GetObjectItemCaseSensitive(t, "name");
        const cJSON *per = cJSON_GetObjectItemCaseSensitive(t,
                                                            "period_s");

        if (cJSON_IsString(name) && cJSON_IsNumber(per))
        {
            em_timer_cfg_t *tm = &s_timers[s_n_timers++];

            snprintf(tm->name, sizeof(tm->name), "%s",
                     name->valuestring);
            tm->period_s = (uint32_t)per->valuedouble;
        }
    }

    s_configured = true;
    return ESP_OK;
}

esp_err_t em_settings_register(void)
{
    static const settings_descriptor_t DESC =
    {
        .name        = "event_manager",
        .version     = 1,
        .fields      = FIELDS,
        .field_count = sizeof(FIELDS) / sizeof(FIELDS[0]),
        .on_apply    = on_apply,
        .on_validate = on_validate,
    };

    return settings_manager_register(&DESC);
}
