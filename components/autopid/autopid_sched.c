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
 * @file autopid_sched.c
 * @brief PURE scheduler (host-tested): the PID is the scheduling unit.
 *
 * Fix #1 from TASK_autopid.md by construction: one slot per PID (or
 * filter) — parameters do not exist here, so the same PID can never be
 * requested once per parameter the way legacy did. Time base is the
 * caller's 64-bit µs clock (esp_timer on target, a fake in host tests).
 */
#include "autopid_private.h"

#include <string.h>

static int entry_count(const ap_config_t *cfg)
{
    return cfg->n_pids + cfg->n_filters;
}

bool ap_sched_client_hold(uint32_t idle_ms)
{
    return idle_ms < AP_CLIENT_YIELD_MS;
}

static int entry_group(const ap_config_t *cfg, int i)
{
    return (i < cfg->n_pids) ? cfg->pids[i].group
                             : cfg->filters[i - cfg->n_pids].group;
}

static uint32_t entry_period_ms(const ap_config_t *cfg, int i)
{
    return (i < cfg->n_pids) ? cfg->pids[i].period_ms
                             : cfg->filters[i - cfg->n_pids].period_ms;
}

static bool entry_self_enabled(const ap_config_t *cfg, int i)
{
    return (i < cfg->n_pids) ? cfg->pids[i].enabled
                             : cfg->filters[i - cfg->n_pids].enabled;
}

void ap_sched_reset(ap_sched_t *st, const ap_config_t *cfg, int64_t now_us)
{
    memset(st->slots, 0, sizeof(st->slots));

    /* stagger initial polls so a big table doesn't fire as one burst */
    for (int i = 0; i < entry_count(cfg); i++)
    {
        st->slots[i].due_us = now_us + (int64_t)i * AP_SCHED_STAGGER_US;
        st->slots[i].last_run_us = 0;
        st->slots[i].fail_streak = 0;
    }

    for (int g = 0; g < cfg->n_groups; g++)
    {
        st->group_enabled[g] = cfg->groups[g].enabled_default;
        st->group_period_override[g] = -1;
    }
}

bool ap_sched_entry_enabled(const ap_sched_t *st, const ap_config_t *cfg,
                            int i)
{
    if (!entry_self_enabled(cfg, i))
    {
        return false;
    }

    int g = entry_group(cfg, i);

    if (g < 0 || g >= cfg->n_groups || !st->group_enabled[g])
    {
        return false;
    }

    if (i < cfg->n_pids && !st->type_enabled[cfg->pids[i].type])
    {
        return false;
    }

    return true;
}

int64_t ap_sched_period_us(const ap_sched_t *st, const ap_config_t *cfg,
                           int i)
{
    int g = entry_group(cfg, i);
    int64_t period_ms = entry_period_ms(cfg, i);

    if (period_ms == 0) /* inherit the group's */
    {
        if (g >= 0 && g < cfg->n_groups)
        {
            period_ms = (st->group_period_override[g] >= 0)
                            ? st->group_period_override[g]
                            : (int64_t)cfg->groups[g].period_ms;
        }
    }
    else if (g >= 0 && g < cfg->n_groups &&
             st->group_period_override[g] >= 0)
    {
        /* a runtime override retargets the WHOLE group (§5b action) */
        period_ms = st->group_period_override[g];
    }

    /* fail backoff: ×4 once the streak passes the threshold */
    if (st->slots[i].fail_streak >= AP_SCHED_BACKOFF_STREAK)
    {
        int64_t backed = (period_ms == 0) ? 1000 : period_ms;

        period_ms = backed * AP_SCHED_BACKOFF_MULT;
    }

    return period_ms * 1000;
}

void ap_sched_group_set(ap_sched_t *st, int g, bool enabled,
                        int32_t period_override_ms)
{
    st->group_enabled[g] = enabled;

    if (period_override_ms >= 0)
    {
        st->group_period_override[g] = period_override_ms;
    }
}

void ap_sched_group_restore(ap_sched_t *st, const ap_config_t *cfg, int g)
{
    st->group_enabled[g] = cfg->groups[g].enabled_default;
    st->group_period_override[g] = -1;
}

int ap_sched_next(const ap_sched_t *st, const ap_config_t *cfg,
                  int64_t *due_us)
{
    int best = -1;

    for (int i = 0; i < entry_count(cfg); i++)
    {
        if (!ap_sched_entry_enabled(st, cfg, i))
        {
            continue;
        }

        if (best < 0 ||
            st->slots[i].due_us < st->slots[best].due_us ||
            (st->slots[i].due_us == st->slots[best].due_us &&
             st->slots[i].last_run_us < st->slots[best].last_run_us))
        {
            best = i;
        }
    }

    if (best >= 0 && due_us != NULL)
    {
        *due_us = st->slots[best].due_us;
    }

    return best;
}

void ap_sched_ran(ap_sched_t *st, const ap_config_t *cfg, int i,
                  int64_t now_us, bool ok)
{
    if (ok)
    {
        st->slots[i].fail_streak = 0;
    }
    else if (st->slots[i].fail_streak < UINT16_MAX)
    {
        st->slots[i].fail_streak++;
    }

    st->slots[i].last_run_us = now_us;

    int64_t period = ap_sched_period_us(st, cfg, i);

    /* period 0 = high-fidelity: due immediately; the last_run_us
       tiebreak in ap_sched_next round-robins several max-rate entries */
    st->slots[i].due_us = now_us + period;
}
