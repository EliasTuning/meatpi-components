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
 * @file bridge_manager_config.c
 * @brief Pure config parsing + validation (host-tested): item parse with
 *        code defaults, the translator-name check, uniqueness, and the
 *        single-consumer rule. Endpoint names are not checked against
 *        the registry (a jack may register at the next boot).
 */
#include <stdio.h>
#include <string.h>

#include "bridge_manager.h"
#include "bridge_manager_private.h"

static bool copy_name(char *dst, size_t dst_len, const cJSON *v)
{
    if (!cJSON_IsString(v) || v->valuestring[0] == '\0' ||
        strlen(v->valuestring) >= dst_len)
    {
        return false;
    }

    strcpy(dst, v->valuestring);
    return true;
}

esp_err_t bm_parse_bridge(const cJSON *item, bm_bridge_cfg_t *out)
{
    memset(out, 0, sizeof(*out));

    if (!copy_name(out->name, sizeof(out->name),
                   cJSON_GetObjectItemCaseSensitive(item, "name")) ||
        !copy_name(out->a, sizeof(out->a),
                   cJSON_GetObjectItemCaseSensitive(item, "a")) ||
        !copy_name(out->b, sizeof(out->b),
                   cJSON_GetObjectItemCaseSensitive(item, "b")))
    {
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *tr = cJSON_GetObjectItemCaseSensitive(item, "translator");

    if (tr == NULL)
    {
        strcpy(out->translator, "raw"); /* code default */
    }
    else if (!copy_name(out->translator, sizeof(out->translator), tr))
    {
        return ESP_ERR_INVALID_ARG;
    }

    out->enabled = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(item, "enabled"));
    return ESP_OK;
}

static bool name_in_list(const char *name, const char *const *list, int count)
{
    for (int i = 0; i < count; i++)
    {
        if (strcmp(name, list[i]) == 0)
        {
            return true;
        }
    }

    return false;
}

/** True when @p name is a registered endpoint whose provider fans RX out
 *  to every subscriber (multi_consumer): exempt from single-consumer. */
static bool name_is_multi(const char *name, const char *const *names,
                          const bool *multi, int count)
{
    for (int i = 0; multi != NULL && i < count; i++)
    {
        if (strcmp(name, names[i]) == 0)
        {
            return multi[i];
        }
    }

    return false;
}

bool bm_bridges_use_endpoint(const cJSON *bridges, const char *endpoint)
{
    const cJSON *item = NULL;

    cJSON_ArrayForEach(item, bridges)
    {
        const cJSON *a = cJSON_GetObjectItemCaseSensitive(item, "a");
        const cJSON *b = cJSON_GetObjectItemCaseSensitive(item, "b");

        if ((cJSON_IsString(a) && strcmp(a->valuestring, endpoint) == 0) ||
            (cJSON_IsString(b) && strcmp(b->valuestring, endpoint) == 0))
        {
            return true;
        }
    }

    return false;
}

bool bm_bridges_add_ble_default(cJSON *bridges)
{
    if (!cJSON_IsArray(bridges) || bm_bridges_use_endpoint(bridges, "ble") ||
        cJSON_GetArraySize(bridges) >= BRIDGE_MANAGER_MAX_BRIDGES)
    {
        return false;
    }

    cJSON *row = cJSON_CreateObject();

    if (row == NULL)
    {
        return false;
    }

    cJSON_AddStringToObject(row, "name", "br_ble_obd");
    cJSON_AddStringToObject(row, "a", "obd");
    cJSON_AddStringToObject(row, "b", "ble");
    cJSON_AddStringToObject(row, "translator", "raw");
    cJSON_AddBoolToObject(row, "enabled", true);
    cJSON_AddItemToArray(bridges, row);
    return true;
}

esp_err_t bm_validate_bridges(const cJSON *bridges,
                              const char *const *endpoint_names,
                              const bool *endpoint_multi, int ep_count,
                              const char *const *translator_names,
                              int tr_count, char *err, size_t err_len)
{
    bm_bridge_cfg_t cfg[BRIDGE_MANAGER_MAX_BRIDGES];
    int count = 0;
    const cJSON *item = NULL;

    cJSON_ArrayForEach(item, bridges)
    {
        if (count >= BRIDGE_MANAGER_MAX_BRIDGES)
        {
            snprintf(err, err_len, "more than %d bridges",
                     BRIDGE_MANAGER_MAX_BRIDGES);
            return ESP_ERR_INVALID_ARG;
        }

        bm_bridge_cfg_t *c = &cfg[count];

        if (bm_parse_bridge(item, c) != ESP_OK)
        {
            snprintf(err, err_len, "bridges[%d]: bad name/a/b", count);
            return ESP_ERR_INVALID_ARG;
        }

        if (strcmp(c->a, c->b) == 0)
        {
            snprintf(err, err_len, "%s: a and b are the same endpoint",
                     c->name);
            return ESP_ERR_INVALID_ARG;
        }

        /* Endpoint names are NOT required to be registered (2026-10-29,
           the field report in internal/unable-to-save-config): the jacks
           of socket servers and WebSocket channels register at the NEXT
           boot under their configured names, so the Connections card
           creates a server and its bridge in ONE submit, and a row left
           behind by a server renamed or removed must not refuse every
           later save of the list (two such rows, udp3333 and udp23,
           locked a device out of all its settings pages). A name the
           composition never resolves degrades that bridge alone at start
           (one W line, /api/bridges says why). The registry still feeds
           the single-consumer rule below. Translators are compile-time
           registrations, complete once the registry is final (tr_count
           >= 0 = a runtime PUT): a codec name nobody registered is a
           typo and is refused. */
        if (tr_count >= 0 && strcmp(c->translator, "raw") != 0 &&
            !name_in_list(c->translator, translator_names, tr_count))
        {
            snprintf(err, err_len, "%s: unknown translator '%s'", c->name,
                     c->translator);
            return ESP_ERR_INVALID_ARG;
        }

        for (int i = 0; i < count; i++)
        {
            if (strcmp(cfg[i].name, c->name) == 0)
            {
                snprintf(err, err_len, "duplicate bridge name '%s'", c->name);
                return ESP_ERR_INVALID_ARG;
            }

            /* single-consumer rule: an endpoint's RX stream belongs to at
               most one ENABLED bridge, except fan-out providers
               (multi_consumer), whose every subscriber gets a full copy.
               A name not in the registry counts as single-stream (every
               dynamic jack is). ep_count < 0 (BOOT apply) skips the rule
               entirely: capabilities are unknowable before the jacks
               register, and a genuinely single-stream provider refuses
               the second subscribe at build time (degrades that bridge
               alone). */
            if (ep_count >= 0 && cfg[i].enabled && c->enabled)
            {
                const char *pairs[4] = { cfg[i].a, cfg[i].b, c->a, c->b };

                for (int x = 0; x < 2; x++)
                {
                    for (int y = 2; y < 4; y++)
                    {
                        if (strcmp(pairs[x], pairs[y]) == 0 &&
                            !name_is_multi(pairs[x], endpoint_names,
                                           endpoint_multi, ep_count))
                        {
                            snprintf(err, err_len,
                                     "endpoint '%s' used by two enabled "
                                     "bridges", pairs[x]);
                            return ESP_ERR_INVALID_ARG;
                        }
                    }
                }
            }
        }

        count++;
    }

    return ESP_OK;
}
