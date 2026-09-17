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
 * @file event_manager_eval.c
 * @brief PURE rule evaluation (host-tested): `match` equality pre-filter,
 *        the `when` condition list (AND; ==/!=/>/>=/</<=/changed/contains)
 *        over trigger fields AND live values (`value:"${...}"`, rendered
 *        through the caller's resolver), the while-rule transition
 *        (em_rule_step) and its between-events live re-check
 *        (em_rule_recheck), cooldown arithmetic. Parsing lives in
 *        event_manager_rules.c. No IDF types beyond cJSON; time is a
 *        caller-supplied 64-bit µs.
 */
#include "event_manager_private.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const em_kv_t *em_event_get(const em_event_t *ev, const char *key)
{
    for (uint8_t i = 0; i < ev->n && i < EM_KV_MAX; i++)
    {
        if (ev->kv[i].key != NULL && strcmp(ev->kv[i].key, key) == 0)
        {
            return &ev->kv[i];
        }
    }

    return NULL;
}

/** kv as a number (bools 0/1); false when it is a string. */
static bool kv_num(const em_kv_t *kv, double *out)
{
    switch (kv->type)
    {
        case EM_VAL_F64:  *out = kv->v.f64;            return true;
        case EM_VAL_I64:  *out = (double)kv->v.i64;    return true;
        case EM_VAL_BOOL: *out = kv->v.b ? 1 : 0;      return true;
        default:                                        return false;
    }
}

static bool operand_equals_kv(const em_operand_t *o, const em_kv_t *kv)
{
    double n;

    if (o->is_num)
    {
        return kv_num(kv, &n) && n == o->num;
    }

    return kv->type == EM_VAL_STR && strcmp(kv->v.str, o->str) == 0;
}

bool em_rule_match(const em_rule_t *r, const em_event_t *ev)
{
    for (uint8_t i = 0; i < r->n_match; i++)
    {
        const em_kv_t *kv = em_event_get(ev, r->match[i].key);

        if (kv == NULL || !operand_equals_kv(&r->match[i], kv))
        {
            return false;
        }
    }

    return true;
}

static bool when_holds(const em_when_t *c, const em_kv_t *kv,
                       em_rule_state_t *st, int slot)
{
    double n;

    switch (c->op)
    {
        case EM_OP_EQ:
            return operand_equals_kv(&c->val, kv);
        case EM_OP_NE:
            return !operand_equals_kv(&c->val, kv);
        case EM_OP_GT:
            return kv_num(kv, &n) && n > c->val.num;
        case EM_OP_GE:
            return kv_num(kv, &n) && n >= c->val.num;
        case EM_OP_LT:
            return kv_num(kv, &n) && n < c->val.num;
        case EM_OP_LE:
            return kv_num(kv, &n) && n <= c->val.num;
        case EM_OP_CONTAINS:
            return kv->type == EM_VAL_STR && !c->val.is_num &&
                   strstr(kv->v.str, c->val.str) != NULL;
        case EM_OP_CHANGED:
        {
            if (st == NULL)
            {
                return false;   /* no slots to compare against       */
            }

            /* differs from the previous match-passing occurrence; the
               first one counts as changed (slot invalid) */
            bool is_num = kv_num(kv, &n);
            bool changed;

            if (!st->last[slot].valid)
            {
                changed = true;
            }
            else if (is_num != st->last[slot].is_num)
            {
                changed = true;
            }
            else if (is_num)
            {
                changed = (n != st->last[slot].num);
            }
            else
            {
                changed = (strcmp(kv->v.str, st->last[slot].str) != 0);
            }

            /* slot updates happen for every matched event (caller
               invokes em_rule_when on each) */
            st->last[slot].valid = true;
            st->last[slot].is_num = is_num;
            st->last[slot].num = is_num ? n : 0;

            if (!is_num)
            {
                snprintf(st->last[slot].str, EM_STR_MAX, "%s",
                         (kv->type == EM_VAL_STR) ? kv->v.str : "");
            }

            return changed;
        }
        default:
            return false;
    }
}

/** A rendered live value as a kv: numbers numeric, true/false bools,
 *  anything else a string; "null" (unresolved) or empty = none. */
static bool kv_from_text(const char *txt, em_kv_t *out)
{
    if (txt[0] == '\0' || strcmp(txt, "null") == 0)
    {
        return false;
    }

    out->key = "value";

    if (strcmp(txt, "true") == 0 || strcmp(txt, "false") == 0)
    {
        out->type = EM_VAL_BOOL;
        out->v.b = (txt[0] == 't');
        return true;
    }

    char *end = NULL;
    double n = strtod(txt, &end);

    if (end != txt && *end == '\0')
    {
        out->type = EM_VAL_F64;
        out->v.f64 = n;
        return true;
    }

    out->type = EM_VAL_STR;
    snprintf(out->v.str, EM_STR_MAX, "%s", txt);
    return true;
}

/** The kv a condition looks at: the trigger's field, or the live value
 *  rendered into @p tmp. NULL = not available (the condition fails). */
static const em_kv_t *cond_kv(const em_when_t *c, const em_event_t *ev,
                              em_tpl_resolver_t resolver, em_kv_t *tmp)
{
    if (c->value[0] == '\0')
    {
        return em_event_get(ev, c->key);
    }

    char txt[EM_STR_MAX];

    if (em_template_render(c->value, ev, resolver, txt,
                           sizeof(txt)) != ESP_OK ||
        !kv_from_text(txt, tmp))
    {
        return NULL;
    }

    return tmp;
}

bool em_rule_when_ex(const em_rule_t *r, const em_event_t *ev,
                     em_rule_state_t *st, em_tpl_resolver_t resolver)
{
    bool all = true;

    for (uint8_t i = 0; i < r->n_when; i++)
    {
        em_kv_t tmp;
        const em_kv_t *kv = cond_kv(&r->when[i], ev, resolver, &tmp);

        if (kv == NULL)
        {
            all = false;    /* keep going: changed slots still update    */
            continue;
        }

        if (!when_holds(&r->when[i], kv, st, i))
        {
            all = false;
        }
    }

    return all;
}

bool em_rule_when(const em_rule_t *r, const em_event_t *ev,
                  em_rule_state_t *st)
{
    return em_rule_when_ex(r, ev, st, NULL);
}

bool em_rule_has_live(const em_rule_t *r)
{
    for (uint8_t i = 0; i < r->n_when; i++)
    {
        if (r->when[i].value[0] != '\0')
        {
            return true;
        }
    }

    return false;
}

bool em_rule_live_holds(const em_rule_t *r, const em_rule_state_t *st,
                        em_tpl_resolver_t resolver)
{
    for (uint8_t i = 0; i < r->n_when; i++)
    {
        const em_when_t *c = &r->when[i];

        if (c->value[0] == '\0' || c->op == EM_OP_CHANGED)
        {
            continue;
        }

        em_kv_t tmp;
        const em_kv_t *kv = cond_kv(c, &st->last_ev, resolver, &tmp);

        /* no slots needed: `changed` is skipped above */
        if (kv == NULL || !when_holds(c, kv, NULL, i))
        {
            return false;
        }
    }

    return true;
}

bool em_rule_cooldown_ok(const em_rule_t *r, em_rule_state_t *st,
                         int64_t now_us)
{
    if (r->cooldown_ms > 0 && st->last_fire_us != 0 &&
        now_us - st->last_fire_us < (int64_t)r->cooldown_ms * 1000)
    {
        return false;
    }

    st->last_fire_us = now_us;
    return true;
}

/* ---- the while-rule transition + the between-events re-check (pure) ----- */

em_step_t em_rule_step(const em_rule_t *r, em_rule_state_t *st, bool holds,
                       const em_event_t *ev)
{
    if (!r->undo)
    {
        return holds ? EM_STEP_RUN : EM_STEP_NONE;
    }

    if (holds)
    {
        st->last_ev = *ev;          /* what the live re-check reads      */
        return st->active ? EM_STEP_NONE : EM_STEP_RUN;
    }

    if (st->active)
    {
        st->active = false;
        return EM_STEP_UNDO;
    }

    return EM_STEP_NONE;
}

bool em_rule_recheck(const em_rule_t *r, em_rule_state_t *st,
                     em_tpl_resolver_t resolver)
{
    if (!r->enabled || !r->undo || !st->active || !em_rule_has_live(r) ||
        em_rule_live_holds(r, st, resolver))
    {
        return false;
    }

    st->active = false;
    return true;
}

/* ---- the engine's whole per-event decision for one rule (pure) ----------- */

em_step_t em_rule_decide(const em_rule_t *r, em_rule_state_t *st,
                         const em_event_t *ev, const char *selector,
                         int64_t now_us, em_tpl_resolver_t resolver,
                         bool *suppressed)
{
    if (!r->enabled || strcmp(r->on, selector) != 0 || !em_rule_match(r, ev))
    {
        return EM_STEP_NONE;
    }

    /* when-eval also updates the `changed` slots: run it for every matched
       event even if the cooldown then suppresses the action */
    bool holds = em_rule_when_ex(r, ev, st, resolver);
    em_step_t step = em_rule_step(r, st, holds, ev);

    if (step == EM_STEP_RUN && !em_rule_cooldown_ok(r, st, now_us))
    {
        if (suppressed != NULL)
        {
            *suppressed = true;
        }

        return EM_STEP_NONE;
    }

    return step;
}

void em_rule_applied(const em_rule_t *r, em_rule_state_t *st)
{
    st->active = r->undo;
}

