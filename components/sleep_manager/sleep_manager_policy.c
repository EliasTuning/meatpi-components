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
 * @file sleep_manager_policy.c
 * @brief The pure voltage ladder (legacy sleep_mode.c semantics,
 *        host-tested; no esp deps). Wrap-safe deadline math: deadlines
 *        are stored as absolute ms and compared by signed subtraction.
 *        Also the pure settings rules of v3: the sleep/wake pair
 *        resolver and the v2 -> v3 wake_mv migration value, and the
 *        critical battery floor tracker (2026-10-01). The countdown the
 *        surfaces report (2026-10-06): which of the two puts the device
 *        to sleep first, and when.
 */
#include "sleep_manager_private.h"

bool sm_resolve_thresholds(int sleep_mv, int wake_mv,
                           float *sleep_v, float *wake_v)
{
    bool clamped = false;
    int min_wake_mv = sleep_mv + 100; /* SM_WAKE_DELTA_V in mV */

    if (wake_mv < min_wake_mv)
    {
        wake_mv = min_wake_mv;
        clamped = true;
    }

    *sleep_v = (float)sleep_mv / 1000.0f;
    *wake_v = (float)wake_mv / 1000.0f;
    return clamped;
}

int sm_migrated_wake_delay_ms(uint32_t from_version, bool present)
{
    return (from_version < 4 && !present) ? (int)SM_WAKE_DELAY_MS : 0;
}

int sm_migrated_wake_mv(int sleep_mv, int lo, int hi)
{
    int wake_mv = sleep_mv + 100;

    if (wake_mv < lo)
    {
        return lo;
    }

    return (wake_mv > hi) ? hi : wake_mv;
}

static bool reached(uint32_t deadline, uint32_t now)
{
    return (int32_t)(now - deadline) >= 0;
}

void sm_critical_init(sm_critical_t *c)
{
    c->armed = false;
    c->t_trip = 0;
}

bool sm_critical_eval(sm_critical_t *c, float volts, uint32_t now_ms)
{
    if (volts >= SM_CRITICAL_V + SM_CRITICAL_HYST_V)
    {
        c->armed = false; /* recovered: the timer starts over next time */
        return false;
    }

    if (!c->armed)
    {
        if (volts < SM_CRITICAL_V)
        {
            c->armed = true;
            c->t_trip = now_ms + SM_CRITICAL_DELAY_MS;
        }

        return false; /* the hysteresis band alone never arms it */
    }

    if (reached(c->t_trip, now_ms))
    {
        c->armed = false;
        return true;
    }

    return false;
}

static uint32_t ms_left(uint32_t deadline, uint32_t now)
{
    int32_t left = (int32_t)(deadline - now);

    return (left > 0) ? (uint32_t)left : 0;
}

sleep_manager_pending_t sm_pending_eval(const sm_policy_t *p, bool ladder_on,
                                        const sm_critical_t *c,
                                        uint32_t now_ms,
                                        uint32_t *deadline_ms)
{
    sleep_manager_pending_t cause = SLEEP_MANAGER_PENDING_NONE;

    if (p->state == SLEEP_MANAGER_SLEEPING ||
        p->state == SLEEP_MANAGER_WAKE_PENDING)
    {
        return SLEEP_MANAGER_PENDING_NONE; /* asleep already */
    }

    if (ladder_on && p->state == SLEEP_MANAGER_LOW_VOLTAGE)
    {
        cause = SLEEP_MANAGER_PENDING_DELAY;
        *deadline_ms = p->t_low;
    }

    if (c->armed && (cause == SLEEP_MANAGER_PENDING_NONE ||
                     ms_left(c->t_trip, now_ms) <=
                         ms_left(p->t_low, now_ms)))
    {
        cause = SLEEP_MANAGER_PENDING_CRITICAL;
        *deadline_ms = c->t_trip;
    }

    return cause;
}

static void push_out(uint32_t *deadline, uint32_t until)
{
    if ((int32_t)(until - *deadline) > 0)
    {
        *deadline = until;
    }
}

bool sm_hold_apply(sm_policy_t *p, bool ladder_on, sm_critical_t *c,
                   uint32_t now_ms, uint32_t hold_ms)
{
    uint32_t until = now_ms + hold_ms;
    bool counting = false;

    if (p->state == SLEEP_MANAGER_SLEEPING ||
        p->state == SLEEP_MANAGER_WAKE_PENDING)
    {
        return false; /* asleep: nobody can be asking */
    }

    if (ladder_on && p->state == SLEEP_MANAGER_LOW_VOLTAGE)
    {
        push_out(&p->t_low, until);
        counting = true;
    }

    if (c->armed)
    {
        push_out(&c->t_trip, until);
        counting = true;
    }

    return counting;
}

void sm_activity_init(sm_activity_t *a)
{
    a->active = false;
    a->cap_until = 0;
    a->until = 0;
}

bool sm_activity_apply(sm_policy_t *p, bool ladder_on, sm_critical_t *c,
                       sm_activity_t *a, uint32_t now_ms, uint32_t hold_ms,
                       uint32_t cap_ms)
{
    bool ladder = ladder_on && p->state == SLEEP_MANAGER_LOW_VOLTAGE;

    if (p->state == SLEEP_MANAGER_SLEEPING ||
        p->state == SLEEP_MANAGER_WAKE_PENDING || (!ladder && !c->armed))
    {
        a->active = false; /* asleep, or nothing counts: nothing to hold */
        return false;
    }

    if (!a->active)
    {
        a->active = true;
        a->cap_until = now_ms + cap_ms; /* the countdown's first read */
    }

    uint32_t until = now_ms + hold_ms;

    if ((int32_t)(until - a->cap_until) > 0)
    {
        until = a->cap_until;
    }

    if (ladder)
    {
        push_out(&p->t_low, until);
    }

    if (c->armed)
    {
        push_out(&c->t_trip, until);
    }

    a->until = until;
    return true;
}

uint32_t sm_pending_pack(sleep_manager_pending_t cause, uint32_t deadline_ms)
{
    return ((uint32_t)cause << 30) | (deadline_ms & SM_PENDING_MS_MASK);
}

sleep_manager_pending_t sm_pending_unpack(uint32_t word, uint32_t now_ms,
                                          uint32_t *sleep_in_s)
{
    sleep_manager_pending_t cause = (sleep_manager_pending_t)(word >> 30);
    uint32_t left = (word - now_ms) & SM_PENDING_MS_MASK;

    if (cause == SLEEP_MANAGER_PENDING_NONE ||
        left > SM_PENDING_MS_MASK / 2u)
    {
        left = 0; /* nothing counts, or the deadline has passed */
    }

    *sleep_in_s = (left + 999u) / 1000u;
    return cause;
}

void sm_policy_init(sm_policy_t *p)
{
    p->state = SLEEP_MANAGER_NORMAL;
    p->t_low = 0;
    p->t_stable = 0;
    p->t_periodic = 0;
}

sm_action_t sm_policy_eval(sm_policy_t *p, const sm_cfg_t *cfg,
                           float volts, uint32_t now_ms)
{
    switch (p->state)
    {
        case SLEEP_MANAGER_NORMAL:
            if (volts < cfg->sleep_v)
            {
                p->state = SLEEP_MANAGER_LOW_VOLTAGE;
                p->t_low = now_ms + cfg->delay_ms;
            }

            break;

        case SLEEP_MANAGER_LOW_VOLTAGE:
            if (volts >= cfg->wake_v)
            {
                p->state = SLEEP_MANAGER_NORMAL;
            }
            else if (reached(p->t_low, now_ms))
            {
                p->state = SLEEP_MANAGER_SLEEPING;
                p->t_periodic = now_ms + cfg->interval_ms;
                return SM_ACT_ENTER_SLEEP;
            }

            break;

        case SLEEP_MANAGER_SLEEPING:
            if (volts >= cfg->wake_v)
            {
                p->state = SLEEP_MANAGER_WAKE_PENDING;
                p->t_stable = now_ms + cfg->wake_hold_ms;
            }
            else if (cfg->periodic && volts > SM_CRITICAL_V &&
                     reached(p->t_periodic, now_ms))
            {
                return SM_ACT_PERIODIC_WAKE;
            }

            break;

        case SLEEP_MANAGER_WAKE_PENDING:
            if (volts < cfg->wake_v)
            {
                p->state = SLEEP_MANAGER_SLEEPING; /* dipped again */
            }
            else if (reached(p->t_stable, now_ms))
            {
                return SM_ACT_WAKE_REBOOT;
            }

            break;
    }

    return SM_ACT_NONE;
}
