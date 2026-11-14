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
 * @file ha_webhooks_reg.c
 * @brief The registration object (the body of `POST /api/webhook` and the
 *        `webhook` object of a push reply), the revision Home Assistant
 *        gives a set of webhook settings, and the rules for a reply
 *        (2026-11-14, settings in the push reply). Pure: strings and cJSON
 *        only, so the host test compiles this file alone.
 *
 * Home Assistant can hand the webhook settings to the device in its reply
 * to the device's own push, for a car it cannot reach (asleep at the
 * moment of the registration, or on LTE without a VPN). It names each set
 * of settings with a short revision; the device stores the revision with
 * the settings and reports it in every push (`status.webhook_rev`), and
 * Home Assistant repeats its settings until the device reports the
 * revision it wants. The device never computes a revision. It keeps the
 * stored one honest with a fingerprint of the settings it was stored
 * with: settings changed by any other route report "".
 */
#include "ha_webhooks_private.h"

#include <stdio.h>
#include <string.h>

static bool url_ok(const char *u)
{
    return u != NULL && (strncmp(u, "http://", 7) == 0 ||
                         strncmp(u, "https://", 8) == 0);
}

/* ---- the revision --------------------------------------------------------- */

bool hw_rev_valid(const char *rev)
{
    if (rev == NULL || rev[0] == '\0')
    {
        return false;
    }

    size_t n = 0;

    for (; rev[n] != '\0'; n++)
    {
        char ch = rev[n];
        bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                  (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';

        if (!ok || n >= HW_REV_LEN - 1)
        {
            return false;
        }
    }

    return true;
}

/* FNV-1a, 32 bit: a fingerprint, not a checksum of anything stored */
static uint32_t fnv1a(uint32_t h, const void *data, size_t len)
{
    const uint8_t *p = data;

    for (size_t i = 0; i < len; i++)
    {
        h ^= p[i];
        h *= 16777619u;
    }

    return h;
}

void hw_rev_sum(const hw_config_t *c, char out[HW_REV_SUM_LEN])
{
    uint8_t enabled = c->enabled ? 1 : 0;
    uint8_t iv[4] = {
        (uint8_t)c->interval_s, (uint8_t)(c->interval_s >> 8),
        (uint8_t)(c->interval_s >> 16), (uint8_t)(c->interval_s >> 24),
    };
    uint32_t h = 2166136261u;

    /* the terminators are part of it: ("ab", "c") is not ("a", "bc") */
    h = fnv1a(h, &enabled, 1);
    h = fnv1a(h, c->url, strlen(c->url) + 1);
    h = fnv1a(h, c->url2, strlen(c->url2) + 1);
    h = fnv1a(h, iv, sizeof(iv));
    snprintf(out, HW_REV_SUM_LEN, "%08x", (unsigned)h);
}

void hw_rev_stamp(hw_config_t *c, const char *rev)
{
    c->rev[0] = '\0';
    c->rev_sum[0] = '\0';

    if (hw_rev_valid(rev))
    {
        strlcpy(c->rev, rev, sizeof(c->rev));
        hw_rev_sum(c, c->rev_sum);
    }
}

const char *hw_rev_reported(const hw_config_t *c)
{
    char sum[HW_REV_SUM_LEN];

    if (!hw_rev_valid(c->rev))
    {
        return "";
    }

    hw_rev_sum(c, sum);
    return strcmp(sum, c->rev_sum) == 0 ? c->rev : "";
}

/* ---- the registration object ---------------------------------------------- */

hw_reg_err_t hw_reg_parse(const cJSON *obj, const hw_config_t *cur,
                          bool need_rev, hw_config_t *out)
{
    const cJSON *url = cJSON_GetObjectItemCaseSensitive(obj, "url");
    const cJSON *urls = cJSON_GetObjectItemCaseSensitive(obj, "urls");
    const cJSON *en = cJSON_GetObjectItemCaseSensitive(obj, "enabled");
    const cJSON *iv = cJSON_GetObjectItemCaseSensitive(obj, "interval");
    const cJSON *rev = cJSON_GetObjectItemCaseSensitive(obj, "rev");
    bool strict = rev != NULL;

    if (strict ? (!cJSON_IsString(rev) || !hw_rev_valid(rev->valuestring))
               : need_rev)
    {
        return HW_REG_REV;
    }

    if (!cJSON_IsString(url) || !url_ok(url->valuestring))
    {
        return HW_REG_URL;
    }

    if (strict && strlen(url->valuestring) >= HW_URL_LEN)
    {
        return HW_REG_URL_LONG;
    }

    *out = *cur;
    strlcpy(out->url, url->valuestring, sizeof(out->url));
    out->url2[0] = '\0';

    if (cJSON_IsArray(urls))
    {
        if (cJSON_GetArraySize(urls) > 2)
        {
            return HW_REG_URLS_MANY;
        }

        const cJSON *u0 = cJSON_GetArrayItem(urls, 0);

        if (!cJSON_IsString(u0) || strcmp(u0->valuestring, out->url) != 0)
        {
            return HW_REG_URLS_FIRST;
        }

        const cJSON *u1 = cJSON_GetArrayItem(urls, 1);

        if (cJSON_IsString(u1))
        {
            if (!url_ok(u1->valuestring))
            {
                return HW_REG_URLS_ENTRY;
            }

            if (strict && strlen(u1->valuestring) >= HW_URL_LEN)
            {
                return HW_REG_URL_LONG;
            }

            strlcpy(out->url2, u1->valuestring, sizeof(out->url2));
        }
        else if (strict && u1 != NULL)
        {
            return HW_REG_URLS_ENTRY;
        }
    }
    else if (strict && urls != NULL)
    {
        return HW_REG_URLS_ENTRY;
    }

    if (cJSON_IsBool(en))
    {
        out->enabled = cJSON_IsTrue(en);
    }
    else if (strict && en != NULL)
    {
        return HW_REG_ENABLED;
    }
    else
    {
        out->enabled = true;
    }

    /* contract: HA's options dialog offers 1..3600 s */
    if (cJSON_IsNumber(iv) && iv->valueint >= 1 && iv->valueint <= 3600 &&
        (!strict || iv->valuedouble == (double)iv->valueint))
    {
        out->interval_s = (uint32_t)iv->valueint;
    }
    else if (strict)
    {
        return HW_REG_INTERVAL;
    }

    hw_rev_stamp(out, strict ? rev->valuestring : NULL);
    return HW_REG_OK;
}

const char *hw_reg_err_str(hw_reg_err_t e)
{
    switch (e)
    {
        case HW_REG_URL:        return "url";
        case HW_REG_URL_LONG:   return "url too long";
        case HW_REG_URLS_MANY:  return "too many urls";
        case HW_REG_URLS_FIRST: return "url must match urls[0]";
        case HW_REG_URLS_ENTRY: return "url entry";
        case HW_REG_INTERVAL:   return "interval";
        case HW_REG_ENABLED:    return "enabled";
        case HW_REG_REV:        return "rev";
        case HW_REG_OK:
        default:                return "";
    }
}

/* ---- the reply ------------------------------------------------------------ */

bool hw_reply_trusted(bool https_named, bool peer_known, bool peer_private,
                      bool via_untrusted_sta)
{
    return https_named || (peer_known && peer_private && !via_untrusted_sta);
}

hw_reply_t hw_reply_decide(const char *body, size_t len, bool truncated,
                           const hw_config_t *cur, bool trusted,
                           hw_config_t *out, hw_reg_err_t *err)
{
    *err = HW_REG_OK;

    if (body == NULL || len == 0)
    {
        return HW_REPLY_NONE;
    }

    if (truncated || len > HW_REPLY_MAX)
    {
        return HW_REPLY_TOO_LONG;
    }

    cJSON *root = cJSON_ParseWithLength(body, len);

    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        return HW_REPLY_NOT_JSON;
    }

    const cJSON *wh = cJSON_GetObjectItemCaseSensitive(root, "webhook");
    hw_reply_t r;

    if (wh == NULL)
    {
        r = HW_REPLY_NONE; /* `{}`, or keys of a later firmware: delivered */
    }
    else if (!cJSON_IsObject(wh))
    {
        *err = HW_REG_URL;
        r = HW_REPLY_INVALID;
    }
    else if (cur->manual_override)
    {
        r = HW_REPLY_PINNED;
    }
    else
    {
        const cJSON *rev = cJSON_GetObjectItemCaseSensitive(wh, "rev");
        const char *have = hw_rev_reported(cur);

        if (cJSON_IsString(rev) && have[0] != '\0' &&
            strcmp(rev->valuestring, have) == 0)
        {
            r = HW_REPLY_SAME;
        }
        else if (!trusted)
        {
            r = HW_REPLY_UNTRUSTED;
        }
        else
        {
            *err = hw_reg_parse(wh, cur, true, out);
            r = (*err == HW_REG_OK) ? HW_REPLY_APPLY : HW_REPLY_INVALID;
        }
    }

    cJSON_Delete(root);
    return r;
}

hw_reply_t hw_gone_decide(const char *body, size_t len, bool truncated,
                          const hw_config_t *cur, bool trusted)
{
    /* any server can answer 410: only Home Assistant's marker counts */
    if (body == NULL || len == 0 || truncated || len > HW_REPLY_MAX)
    {
        return HW_REPLY_NONE;
    }

    cJSON *root = cJSON_ParseWithLength(body, len);
    bool marked = false;

    if (cJSON_IsObject(root))
    {
        const cJSON *wh = cJSON_GetObjectItemCaseSensitive(root, "webhook");

        marked = cJSON_IsObject(wh) &&
                 cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(wh, "removed"));
    }

    cJSON_Delete(root);

    if (!marked)
    {
        return HW_REPLY_NONE;
    }

    if (cur->manual_override)
    {
        return HW_REPLY_PINNED;
    }

    return trusted ? HW_REPLY_REMOVED : HW_REPLY_UNTRUSTED;
}

void hw_config_unregister(hw_config_t *c)
{
    c->url[0] = '\0';
    c->url2[0] = '\0';
    c->enabled = false;
    hw_rev_stamp(c, NULL); /* nothing left for a revision to name */
}

const char *hw_reply_str(hw_reply_t r)
{
    switch (r)
    {
        case HW_REPLY_APPLY:     return "applied";
        case HW_REPLY_SAME:      return "same";
        case HW_REPLY_PINNED:    return "pinned";
        case HW_REPLY_UNTRUSTED: return "untrusted";
        case HW_REPLY_TOO_LONG:  return "too_long";
        case HW_REPLY_NOT_JSON:  return "not_json";
        case HW_REPLY_INVALID:   return "invalid";
        case HW_REPLY_FAILED:    return "failed";
        case HW_REPLY_REMOVED:   return "removed";
        case HW_REPLY_NONE:
        default:                 return "";
    }
}
