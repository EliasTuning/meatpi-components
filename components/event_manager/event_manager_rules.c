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
 * @file event_manager_rules.c
 * @brief PURE rule parsing (host-tested): parse/shape-validate the rules
 *        array (name/on/match/when incl. live-value conditions/do/with/
 *        undo/cooldown_ms) and the undo-vs-action check the settings
 *        validator runs with the live registry. Evaluation lives in
 *        event_manager_eval.c. No IDF types beyond cJSON.
 */
#include "event_manager_private.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- parse -------------------------------------------------------------------- */

static const char *const OP_NAMES[] =
{
    "==", "!=", ">", ">=", "<", "<=", "changed", "contains",
};

static int op_from_string(const char *s)
{
    for (size_t i = 0; i < sizeof(OP_NAMES) / sizeof(OP_NAMES[0]); i++)
    {
        if (strcmp(s, OP_NAMES[i]) == 0)
        {
            return (int)i;
        }
    }

    return -1;
}

static bool copy_bounded(char *dst, size_t cap, const cJSON *v,
                         bool required)
{
    if (!cJSON_IsString(v) || v->valuestring[0] == '\0')
    {
        dst[0] = '\0';
        return !required;
    }

    if (strlen(v->valuestring) >= cap)
    {
        return false;
    }

    strcpy(dst, v->valuestring);
    return true;
}

/** JSON value -> typed operand (numbers+bools numeric, strings string). */
static bool operand_from_json(const cJSON *v, em_operand_t *out)
{
    if (cJSON_IsNumber(v))
    {
        out->is_num = true;
        out->num = v->valuedouble;
        out->str[0] = '\0';
        return true;
    }

    if (cJSON_IsBool(v))
    {
        out->is_num = true;
        out->num = cJSON_IsTrue(v) ? 1 : 0;
        out->str[0] = '\0';
        return true;
    }

    if (cJSON_IsString(v) && strlen(v->valuestring) < EM_STR_MAX)
    {
        out->is_num = false;
        out->num = 0;
        strcpy(out->str, v->valuestring);
        return true;
    }

    return false;
}

static void perr(char *err, size_t err_len, int idx, const char *what)
{
    if (err != NULL && err_len > 0)
    {
        snprintf(err, err_len, "rules[%d]: %s", idx, what);
    }
}

esp_err_t em_rules_parse(const cJSON *rules, em_rule_t *out, int max,
                         int *count, char *err, size_t err_len)
{
    *count = 0;

    if (rules == NULL || cJSON_IsNull(rules))
    {
        return ESP_OK;              /* no rules = valid                   */
    }

    if (!cJSON_IsArray(rules))
    {
        perr(err, err_len, 0, "not an array");
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *item = NULL;
    int idx = 0;

    cJSON_ArrayForEach(item, rules)
    {
        if (*count >= max)
        {
            perr(err, err_len, idx, "too many rules");
            return ESP_ERR_INVALID_ARG;
        }

        em_rule_t *r = &out[*count];

        memset(r, 0, sizeof(*r));

        if (!copy_bounded(r->name, sizeof(r->name),
                          cJSON_GetObjectItemCaseSensitive(item, "name"),
                          true) ||
            r->name[0] == '\0')
        {
            perr(err, err_len, idx, "name required");
            return ESP_ERR_INVALID_ARG;
        }

        const cJSON *en = cJSON_GetObjectItemCaseSensitive(item,
                                                           "enabled");

        r->enabled = !cJSON_IsFalse(en);
        r->undo = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item,
                                                              "undo"));

        if (!copy_bounded(r->on, sizeof(r->on),
                          cJSON_GetObjectItemCaseSensitive(item, "on"),
                          true) ||
            strchr(r->on, '.') == NULL)
        {
            perr(err, err_len, idx, "on must be \"source.event\"");
            return ESP_ERR_INVALID_ARG;
        }

        /* body: either an action ("do" + optional "with"), or the script
         * sugar ("script":"name" — rewritten to script.run{name}) */
        const cJSON *script = cJSON_GetObjectItemCaseSensitive(item,
                                                               "script");

        if (cJSON_IsString(script) && script->valuestring[0] != '\0')
        {
            if (cJSON_GetObjectItemCaseSensitive(item, "do") != NULL)
            {
                perr(err, err_len, idx, "script and do are exclusive");
                return ESP_ERR_INVALID_ARG;
            }

            int w = snprintf(r->with_json, sizeof(r->with_json),
                             "{\"name\":\"%s\"}", script->valuestring);

            if (strchr(script->valuestring, '"') != NULL ||
                strchr(script->valuestring, '\\') != NULL ||
                w < 0 || (size_t)w >= sizeof(r->with_json))
            {
                perr(err, err_len, idx, "bad script name");
                return ESP_ERR_INVALID_ARG;
            }

            strcpy(r->action, "script.run");
        }
        else if (!copy_bounded(r->action, sizeof(r->action),
                          cJSON_GetObjectItemCaseSensitive(item, "do"),
                          true) ||
            r->action[0] == '\0')
        {
            perr(err, err_len, idx, "do (action) or script required");
            return ESP_ERR_INVALID_ARG;
        }

        /* match: object of key -> equality value */
        const cJSON *match = cJSON_GetObjectItemCaseSensitive(item,
                                                              "match");

        if (match != NULL && !cJSON_IsNull(match))
        {
            if (!cJSON_IsObject(match))
            {
                perr(err, err_len, idx, "match must be an object");
                return ESP_ERR_INVALID_ARG;
            }

            const cJSON *m = NULL;

            cJSON_ArrayForEach(m, match)
            {
                if (r->n_match >= EM_MATCH_MAX)
                {
                    perr(err, err_len, idx, "too many match keys");
                    return ESP_ERR_INVALID_ARG;
                }

                em_operand_t *o = &r->match[r->n_match];

                if (m->string == NULL ||
                    strlen(m->string) >= EM_KEY_LEN ||
                    !operand_from_json(m, o))
                {
                    perr(err, err_len, idx, "bad match entry");
                    return ESP_ERR_INVALID_ARG;
                }

                strcpy(o->key, m->string);
                r->n_match++;
            }
        }

        /* when: array of {key, op, val} */
        const cJSON *when = cJSON_GetObjectItemCaseSensitive(item,
                                                             "when");

        if (when != NULL && !cJSON_IsNull(when))
        {
            if (!cJSON_IsArray(when))
            {
                perr(err, err_len, idx, "when must be an array");
                return ESP_ERR_INVALID_ARG;
            }

            const cJSON *w = NULL;

            cJSON_ArrayForEach(w, when)
            {
                if (r->n_when >= EM_WHEN_MAX)
                {
                    perr(err, err_len, idx, "too many when conditions");
                    return ESP_ERR_INVALID_ARG;
                }

                em_when_t *c = &r->when[r->n_when];
                char op[12];
                const cJSON *wk = cJSON_GetObjectItemCaseSensitive(w,
                                                                   "key");
                const cJSON *wv = cJSON_GetObjectItemCaseSensitive(w,
                                                                   "value");
                bool has_key = cJSON_IsString(wk) &&
                               wk->valuestring[0] != '\0';
                bool has_val = cJSON_IsString(wv) &&
                               wv->valuestring[0] != '\0';

                /* key = a field of the trigger; value = a live-value
                   template ("${autopid.SOC}") read when the rule fires.
                   Exactly one of the two. */
                if (has_key == has_val)
                {
                    perr(err, err_len, idx, "when needs key or value");
                    return ESP_ERR_INVALID_ARG;
                }

                if ((has_key && !copy_bounded(c->key, sizeof(c->key), wk,
                                              true)) ||
                    (has_val && (!copy_bounded(c->value, sizeof(c->value),
                                               wv, true) ||
                                 strstr(c->value, "${") == NULL)) ||
                    !copy_bounded(op, sizeof(op),
                                  cJSON_GetObjectItemCaseSensitive(w,
                                                                   "op"),
                                  true))
                {
                    perr(err, err_len, idx,
                         "when needs op; value needs ${...}");
                    return ESP_ERR_INVALID_ARG;
                }

                int opi = op_from_string(op);

                if (opi < 0)
                {
                    perr(err, err_len, idx, "unknown op");
                    return ESP_ERR_INVALID_ARG;
                }

                c->op = (em_op_t)opi;

                const cJSON *val = cJSON_GetObjectItemCaseSensitive(w,
                                                                    "val");

                if (c->op != EM_OP_CHANGED)
                {
                    if (!operand_from_json(val, &c->val))
                    {
                        perr(err, err_len, idx, "when needs val");
                        return ESP_ERR_INVALID_ARG;
                    }

                    if (!c->val.is_num &&
                        (c->op == EM_OP_GT || c->op == EM_OP_GE ||
                         c->op == EM_OP_LT || c->op == EM_OP_LE))
                    {
                        perr(err, err_len, idx,
                             "ordered op needs a number");
                        return ESP_ERR_INVALID_ARG;
                    }
                }

                r->n_when++;
            }
        }

        /* with: any object; stored serialized (rendered at dispatch) */
        const cJSON *with = cJSON_GetObjectItemCaseSensitive(item,
                                                             "with");

        if (cJSON_IsString(script) && script->valuestring[0] != '\0' &&
            with != NULL && !cJSON_IsNull(with))
        {
            perr(err, err_len, idx, "script takes no with");
            return ESP_ERR_INVALID_ARG;
        }

        if (with != NULL && !cJSON_IsNull(with))
        {
            if (!cJSON_IsObject(with))
            {
                perr(err, err_len, idx, "with must be an object");
                return ESP_ERR_INVALID_ARG;
            }

            char *s = cJSON_PrintUnformatted(with);

            if (s == NULL || strlen(s) >= EM_WITH_MAX)
            {
                cJSON_free(s);
                perr(err, err_len, idx, "with too large");
                return ESP_ERR_INVALID_ARG;
            }

            strcpy(r->with_json, s);
            cJSON_free(s);
        }

        const cJSON *cd = cJSON_GetObjectItemCaseSensitive(item,
                                                           "cooldown_ms");

        if (cd != NULL && !cJSON_IsNull(cd))
        {
            if (!cJSON_IsNumber(cd) || cd->valuedouble < 0 ||
                cd->valuedouble > 86400000.0)
            {
                perr(err, err_len, idx, "cooldown_ms 0..86400000");
                return ESP_ERR_INVALID_ARG;
            }

            r->cooldown_ms = (uint32_t)cd->valuedouble;
        }

        /* duplicate names break the log/UI keying */
        for (int i = 0; i < *count; i++)
        {
            if (strcmp(out[i].name, r->name) == 0)
            {
                perr(err, err_len, idx, "duplicate rule name");
                return ESP_ERR_INVALID_ARG;
            }
        }

        (*count)++;
        idx++;
    }

    return ESP_OK;
}

/* ---- evaluation --------------------------------------------------------------- */

esp_err_t em_rules_validate_undo(const em_rule_t *rules, int count,
                                 bool (*undoable)(const char *action),
                                 char *err, size_t err_len)
{
    for (int i = 0; i < count; i++)
    {
        if (rules[i].undo && !undoable(rules[i].action))
        {
            snprintf(err, err_len, "%s: '%s' cannot undo", rules[i].name,
                     rules[i].action);
            return ESP_ERR_INVALID_ARG;
        }
    }

    return ESP_OK;
}
