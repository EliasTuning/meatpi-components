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
 * @file data_destinations_abrp.c
 * @brief PURE ABRP (Iternio telemetry API) rules: the autopid snapshot ->
 *        `tlm` name map, the `token=&tlm=` form body, the `APIKEY` header
 *        value and the response verdict. No IDF, no RTOS — host-tested.
 *        Contract: https://documenter.getpostman.com/view/7396339/SWTK5a8w
 *        (GET or POST /1/tlm/send, api_key as a query parameter or an
 *        `Authorization: APIKEY <key>` header, `token` + URL-encoded
 *        `tlm` JSON, response `{"status":"ok"|"error", ...}`).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "data_destinations_private.h"

/** Copy @p from (any name case) into @p tlm as @p to, normalizing
 *  booleans and "on"/"off"/numeric strings to numbers (legacy rule). */
static bool abrp_map(const cJSON *src, cJSON *tlm, const char *from,
                     const char *to)
{
    const cJSON *it = cJSON_GetObjectItem(src, from); /* case-insensitive */

    if (it == NULL || cJSON_GetObjectItemCaseSensitive(tlm, to) != NULL)
    {
        return false;
    }

    if (cJSON_IsNumber(it))
    {
        cJSON_AddNumberToObject(tlm, to, it->valuedouble);
        return true;
    }

    if (cJSON_IsBool(it))
    {
        cJSON_AddNumberToObject(tlm, to, cJSON_IsTrue(it) ? 1 : 0);
        return true;
    }

    if (cJSON_IsString(it) && it->valuestring != NULL)
    {
        const char *s = it->valuestring;

        if (strcasecmp(s, "on") == 0 || strcasecmp(s, "true") == 0)
        {
            cJSON_AddNumberToObject(tlm, to, 1);
        }
        else if (strcasecmp(s, "off") == 0 || strcasecmp(s, "false") == 0)
        {
            cJSON_AddNumberToObject(tlm, to, 0);
        }
        else
        {
            char *end = NULL;
            double v = strtod(s, &end);

            if (end != NULL && end != s && *end == '\0')
            {
                cJSON_AddNumberToObject(tlm, to, v);
            }
            else
            {
                cJSON_AddStringToObject(tlm, to, s);
            }
        }

        return true;
    }

    return false;
}

cJSON *dd_abrp_tlm(const cJSON *snapshot, const char *car_model,
                   int64_t utc_s)
{
    /* legacy autopid name -> ABRP key (v4.51p build_abrp_payload) */
    static const struct
    {
        const char *from;
        const char *to;
    } MAP[] =
    {
        { "SOC", "soc" },               { "HV_W", "power" },
        { "SPEED", "speed" },           { "CHARGING", "is_charging" },
        { "CHARGING_DC", "is_dcfc" },   { "PARK_BRAKE", "is_parked" },
        { "HV_CAPACITY_KWH", "capacity" }, { "HV_CAPACITY_R", "soe" },
        { "SOH", "soh" },               { "TMP_A", "ext_temp" },
        { "BATT_TEMP", "batt_temp" },   { "HV_V", "voltage" },
        { "HV_A", "current" },          { "ODOMETER", "odometer" },
        { "RANGE", "est_battery_range" }, { "T_CAB", "cabin_temp" },
        { "TYRE_P_FL", "tire_pressure_fl" },
        { "TYRE_P_FR", "tire_pressure_fr" },
        { "TYRE_P_RL", "tire_pressure_rl" },
        { "TYRE_P_RR", "tire_pressure_rr" },
        /* the ESPNetLink dongle fix (main's GPS sink names) */
        { "gps_latitude", "lat" },      { "gps_longitude", "lon" },
        { "gps_altitude", "elevation" }, { "gps_heading", "heading" },
        { "gps_speed", "speed" },       /* only when no SPEED above */
        /* pass-through ABRP-native keys a profile may already use */
        { "lat", "lat" },               { "lon", "lon" },
        { "elevation", "elevation" },   { "utc", "utc" },
        { "heading", "heading" },
    };

    cJSON *tlm = cJSON_CreateObject();

    if (tlm == NULL || snapshot == NULL)
    {
        return tlm;
    }

    for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++)
    {
        abrp_map(snapshot, tlm, MAP[i].from, MAP[i].to);
    }

    if (cJSON_GetObjectItemCaseSensitive(tlm, "utc") == NULL)
    {
        cJSON_AddNumberToObject(tlm, "utc", (double)utc_s);
    }

    if (car_model != NULL && car_model[0] != '\0' &&
        cJSON_GetObjectItemCaseSensitive(tlm, "car_model") == NULL)
    {
        cJSON_AddStringToObject(tlm, "car_model", car_model);
    }

    return tlm;
}

bool dd_abrp_form(const char *token, const char *tlm_json, char *out,
                  size_t cap)
{
    if (token == NULL || tlm_json == NULL || out == NULL || cap < 12)
    {
        return false;
    }

    memcpy(out, "token=", 6);

    if (!dd_url_encode(token, out + 6, cap - 6))
    {
        return false;
    }

    size_t len = strlen(out);

    if (len + 5 >= cap)
    {
        return false;
    }

    memcpy(out + len, "&tlm=", 5);
    len += 5;
    return dd_url_encode(tlm_json, out + len, cap - len);
}

bool dd_abrp_auth_value(const char *api_key, char *out, size_t cap)
{
    if (api_key == NULL || api_key[0] == '\0' || out == NULL)
    {
        return false;
    }

    int n = (strncasecmp(api_key, "APIKEY ", 7) == 0)
                ? snprintf(out, cap, "%s", api_key)
                : snprintf(out, cap, "APIKEY %s", api_key);

    return n >= 0 && (size_t)n < cap;
}

bool dd_abrp_response_ok(const char *body, bool http_ok, char *err,
                         size_t err_len)
{
    if (body == NULL || body[0] == '\0')
    {
        if (!http_ok && err != NULL)
        {
            snprintf(err, err_len, "abrp: empty response");
        }

        return http_ok;
    }

    cJSON *root = cJSON_Parse(body);

    if (root == NULL)
    {
        return http_ok; /* not JSON: the HTTP status is the outcome */
    }

    const cJSON *st = cJSON_GetObjectItemCaseSensitive(root, "status");
    bool ok = http_ok;

    if (cJSON_IsString(st) && st->valuestring != NULL)
    {
        ok = strcmp(st->valuestring, "ok") == 0;

        if (!ok && err != NULL)
        {
            const cJSON *detail = cJSON_GetObjectItemCaseSensitive(root,
                                                                   "missing");

            if (detail == NULL)
            {
                detail = cJSON_GetObjectItemCaseSensitive(root, "errors");
            }

            if (detail == NULL)
            {
                detail = cJSON_GetObjectItemCaseSensitive(root, "error");
            }

            char *d = detail != NULL ? cJSON_PrintUnformatted(detail) : NULL;

            snprintf(err, err_len, "abrp: %s%s%s", st->valuestring,
                     d != NULL ? " " : "", d != NULL ? d : "");
            cJSON_free(d);
        }
    }

    cJSON_Delete(root);
    return ok;
}
