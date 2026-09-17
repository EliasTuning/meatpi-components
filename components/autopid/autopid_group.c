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
 * @file autopid_group.c
 * @brief Runtime group control (EPHEMERAL, the §5b context switch): the
 *        public autopid_group_set() / autopid_group_restore() and the group
 *        state JSON for /api/autopid. The decisions are the pure
 *        ap_sched_group_set()/ap_sched_group_restore() (host-tested); this
 *        file adds the name lookup under the core lock and the poller
 *        wake-up. Split out of autopid.c on 2026-09-17 (standard §1: ≤ 700
 *        lines per file).
 */
#include "autopid.h"

#include <string.h>

#include "esp_log.h"

#include "autopid_private.h"

static const char *TAG = "autopid";

/** Find @p group by name under the lock; -1 when unknown. Caller holds
 *  the lock (the index is only valid while it does). */
static int group_index(const char *group)
{
    const ap_config_t *cfg = ap_core_config();

    for (int g = 0; g < cfg->n_groups; g++)
    {
        if (strcmp(cfg->groups[g].name, group) == 0)
        {
            return g;
        }
    }

    return -1;
}

esp_err_t autopid_group_set(const char *group, bool enabled,
                            int32_t period_override_ms)
{
    if (group == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    ap_core_lock();

    int g = group_index(group);

    if (g >= 0)
    {
        ap_sched_group_set(ap_core_sched(), g, enabled, period_override_ms);
    }

    ap_core_unlock();

    if (g < 0)
    {
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "group '%s': %s%s", group, enabled ? "enabled" : "disabled",
             (period_override_ms >= 0) ? " (period override)" : "");
    ap_core_wake();
    return ESP_OK;
}

esp_err_t autopid_group_restore(const char *group)
{
    if (group == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    ap_core_lock();

    int g = group_index(group);

    if (g >= 0)
    {
        ap_sched_group_restore(ap_core_sched(), ap_core_config(), g);
    }

    ap_core_unlock();

    if (g < 0)
    {
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "group '%s': back to its configured state", group);
    ap_core_wake();
    return ESP_OK;
}

esp_err_t ap_core_group_json(cJSON *arr)
{
    ap_core_lock();

    const ap_config_t *cfg = ap_core_config();
    const ap_sched_t *st = ap_core_sched();

    for (int g = 0; g < cfg->n_groups; g++)
    {
        cJSON *o = cJSON_CreateObject();

        if (o == NULL)
        {
            break;
        }

        cJSON_AddStringToObject(o, "name", cfg->groups[g].name);
        cJSON_AddBoolToObject(o, "enabled", st->group_enabled[g]);
        cJSON_AddNumberToObject(o, "period_ms",
                                (st->group_period_override[g] >= 0)
                                    ? st->group_period_override[g]
                                    : (double)cfg->groups[g].period_ms);
        cJSON_AddItemToArray(arr, o);
    }

    ap_core_unlock();
    return ESP_OK;
}
