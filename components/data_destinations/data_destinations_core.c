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
 * @file data_destinations_core.c
 * @brief PURE policy: settings-item parse/normalize, the per-destination
 *        scheduler with exponential backoff, percent-encoding, URL
 *        composition and `~/` topic expansion (the ABRP rules live in
 *        data_destinations_abrp.c). No IDF, no RTOS — host-tested.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "data_destinations_private.h"

/* ---- enums ----------------------------------------------------------------- */

static const char *const TYPE_NAMES[] = { "mqtt", "http", "https", "abrp" };
static const char *const AUTH_NAMES[] =
{ "none", "bearer", "api_key_header", "api_key_query", "basic" };

const char *dd_type_str(dd_type_t t)
{
    return ((unsigned)t < 4) ? TYPE_NAMES[t] : "?";
}

const char *dd_auth_str(dd_auth_t a)
{
    return ((unsigned)a < 5) ? AUTH_NAMES[a] : "?";
}

bool dd_type_parse(const char *s, dd_type_t *out)
{
    for (unsigned i = 0; s != NULL && i < 4; i++)
    {
        if (strcmp(s, TYPE_NAMES[i]) == 0)
        {
            *out = (dd_type_t)i;
            return true;
        }
    }

    return false;
}

bool dd_auth_parse(const char *s, dd_auth_t *out)
{
    for (unsigned i = 0; s != NULL && i < 5; i++)
    {
        if (strcmp(s, AUTH_NAMES[i]) == 0)
        {
            *out = (dd_auth_t)i;
            return true;
        }
    }

    return false;
}

/* ---- parse ------------------------------------------------------------------ */

static void item_str(const cJSON *o, const char *key, char *dst, size_t cap,
                     const char *dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    const char *s = (cJSON_IsString(v) && v->valuestring != NULL)
                        ? v->valuestring : dflt;

    snprintf(dst, cap, "%s", s != NULL ? s : "");
}

static bool item_bool(const cJSON *o, const char *key, bool dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : dflt;
}

static bool has_scheme(const char *url)
{
    return strncasecmp(url, "http://", 7) == 0 ||
           strncasecmp(url, "https://", 8) == 0;
}

/** In-place `<prefix><url>`; false when the result would not fit. */
static bool prepend(char *url, size_t cap, const char *prefix)
{
    size_t plen = strlen(prefix);
    size_t ulen = strlen(url);

    if (plen + ulen >= cap)
    {
        return false;
    }

    memmove(url + plen, url, ulen + 1);
    memcpy(url, prefix, plen);
    return true;
}

static esp_err_t fail(char *err, size_t len, const char *name,
                      const char *what)
{
    if (err != NULL && len > 0)
    {
        snprintf(err, len, "%s: %s", name, what);
    }

    return ESP_ERR_INVALID_ARG;
}

esp_err_t dd_parse_dest(const cJSON *item, dd_dest_t *out, char *err,
                        size_t err_len)
{
    if (!cJSON_IsObject(item) || out == NULL)
    {
        return fail(err, err_len, "destination", "not an object");
    }

    memset(out, 0, sizeof(*out));
    item_str(item, "name", out->name, sizeof(out->name), "");

    if (out->name[0] == '\0')
    {
        return fail(err, err_len, "destination", "name required");
    }

    char buf[DD_SMALL_LEN];

    item_str(item, "type", buf, sizeof(buf), "mqtt");

    if (!dd_type_parse(buf, &out->type))
    {
        return fail(err, err_len, out->name, "unknown type");
    }

    item_str(item, "auth", buf, sizeof(buf), "none");

    if (!dd_auth_parse(buf, &out->auth))
    {
        return fail(err, err_len, out->name, "unknown auth");
    }

    out->enabled = item_bool(item, "enabled", true);
    out->retain = item_bool(item, "retain", true);
    out->full_first = item_bool(item, "full_first", true);

    const cJSON *p = cJSON_GetObjectItemCaseSensitive(item, "period_s");

    out->period_s = cJSON_IsNumber(p) ? (uint32_t)p->valueint : 5;

    if (out->period_s < 1)
    {
        out->period_s = 1;
    }

    item_str(item, "url", out->url, sizeof(out->url), "");
    item_str(item, "auth_token", out->auth_token, sizeof(out->auth_token),
             "");
    item_str(item, "auth_name", out->auth_name, sizeof(out->auth_name), "");
    item_str(item, "basic_username", out->basic_username,
             sizeof(out->basic_username), "");
    item_str(item, "basic_password", out->basic_password,
             sizeof(out->basic_password), "");
    item_str(item, "api_key", out->api_key, sizeof(out->api_key), "");
    item_str(item, "query", out->query, sizeof(out->query), "");
    item_str(item, "cert_set", out->cert_set, sizeof(out->cert_set), "");
    item_str(item, "car_model", out->car_model, sizeof(out->car_model), "");

    /* normalize per type (legacy semantics: a bare host gets the scheme
       the type implies; ABRP defaults to Iternio's endpoint) */
    switch (out->type)
    {
    case DD_TYPE_MQTT:
        if (out->url[0] == '\0')
        {
            snprintf(out->url, sizeof(out->url), "~/autopid");
        }
        break;

    case DD_TYPE_HTTP:
    case DD_TYPE_HTTPS:
        if (out->url[0] != '\0' && !has_scheme(out->url) &&
            !prepend(out->url, sizeof(out->url),
                     out->type == DD_TYPE_HTTPS ? "https://" : "http://"))
        {
            return fail(err, err_len, out->name, "url too long");
        }

        if (out->type == DD_TYPE_HTTPS &&
            strncasecmp(out->url, "http://", 7) == 0)
        {
            return fail(err, err_len, out->name,
                        "https destination with an http:// URL");
        }
        break;

    case DD_TYPE_ABRP:
        if (out->url[0] == '\0')
        {
            snprintf(out->url, sizeof(out->url), "%s", DD_ABRP_DEFAULT_URL);
        }
        else if (!has_scheme(out->url) &&
                 !prepend(out->url, sizeof(out->url), "https://"))
        {
            return fail(err, err_len, out->name, "url too long");
        }
        break;
    }

    if (out->auth == DD_AUTH_API_KEY_HEADER && out->auth_name[0] == '\0')
    {
        snprintf(out->auth_name, sizeof(out->auth_name), "%s",
                 out->type == DD_TYPE_ABRP ? "Authorization" : "x-api-key");
    }

    if (out->auth == DD_AUTH_API_KEY_QUERY && out->auth_name[0] == '\0')
    {
        snprintf(out->auth_name, sizeof(out->auth_name), "api_key");
    }

    if (!out->enabled)
    {
        return ESP_OK; /* a disabled entry may be half-filled */
    }

    if (out->url[0] == '\0')
    {
        return fail(err, err_len, out->name, "url required");
    }

    if (out->type == DD_TYPE_ABRP && out->auth_token[0] == '\0')
    {
        return fail(err, err_len, out->name,
                    "ABRP user token (auth_token) required");
    }

    if (out->type != DD_TYPE_ABRP)
    {
        if ((out->auth == DD_AUTH_BEARER ||
             out->auth == DD_AUTH_API_KEY_HEADER ||
             out->auth == DD_AUTH_API_KEY_QUERY) &&
            out->auth_token[0] == '\0')
        {
            return fail(err, err_len, out->name,
                        "auth_token required for this auth mode");
        }

        if (out->auth == DD_AUTH_BASIC && out->basic_username[0] == '\0')
        {
            return fail(err, err_len, out->name,
                        "basic_username required for basic auth");
        }
    }

    return ESP_OK;
}

esp_err_t dd_config_parse(const cJSON *settings, dd_config_t *out,
                          char *err, size_t err_len)
{
    if (!cJSON_IsObject(settings) || out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->enabled = item_bool(settings, "enabled", true);

    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(settings,
                                                        "destinations");
    const cJSON *it = NULL;

    cJSON_ArrayForEach(it, arr)
    {
        if (out->n >= DD_MAX)
        {
            return fail(err, err_len, "destinations", "too many entries");
        }

        esp_err_t rc = dd_parse_dest(it, &out->dest[out->n], err, err_len);

        if (rc != ESP_OK)
        {
            return rc;
        }

        for (size_t i = 0; i < out->n; i++)
        {
            if (strcmp(out->dest[i].name, out->dest[out->n].name) == 0)
            {
                return fail(err, err_len, out->dest[out->n].name,
                            "duplicate name");
            }
        }

        out->n++;
    }

    return ESP_OK;
}

/* ---- scheduler / backoff -------------------------------------------------- */

bool dd_sched_due(const dd_state_t *st, int64_t now_us)
{
    return st->next_due_us == 0 || now_us >= st->next_due_us;
}

uint32_t dd_backoff_next(uint32_t prev_ms, uint32_t period_ms)
{
    uint64_t next = prev_ms != 0 ? (uint64_t)prev_ms * 2
                                 : (uint64_t)period_ms * 2;
    uint64_t cap = (uint64_t)period_ms * DD_BACKOFF_MAX_MULT;

    if (next < DD_BACKOFF_FLOOR_MS)
    {
        next = DD_BACKOFF_FLOOR_MS;
    }

    if (cap < DD_BACKOFF_CAP_MIN_MS)
    {
        cap = DD_BACKOFF_CAP_MIN_MS;
    }

    if (cap > DD_BACKOFF_CAP_MAX_MS)
    {
        cap = DD_BACKOFF_CAP_MAX_MS;
    }

    if (next > cap)
    {
        next = cap;
    }

    return (uint32_t)next;
}

void dd_sched_after(dd_state_t *st, uint32_t period_s, bool ok,
                    int64_t now_us)
{
    uint32_t period_ms = period_s * 1000u;
    uint32_t wait_ms = period_ms;

    st->ever_tried = true;
    st->was_ok = ok;

    if (ok)
    {
        st->success++;
        st->consec_failures = 0;
        st->backoff_ms = 0;
    }
    else
    {
        st->fail++;
        st->consec_failures++;

        if (st->consec_failures >= DD_BACKOFF_AFTER)
        {
            st->backoff_ms = dd_backoff_next(st->backoff_ms, period_ms);
        }

        if (st->backoff_ms > wait_ms)
        {
            wait_ms = st->backoff_ms;
        }
    }

    st->next_due_us = now_us + (int64_t)wait_ms * 1000;
}

void dd_sched_skip(dd_state_t *st, uint32_t period_s, int64_t now_us)
{
    st->skipped_offline++;
    st->next_due_us = now_us + (int64_t)period_s * 1000000;
}

/* ---- URL helpers ------------------------------------------------------------ */

static bool unreserved(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
           c == '~';
}

bool dd_url_encode(const char *in, char *out, size_t cap)
{
    static const char HEX[] = "0123456789ABCDEF";
    size_t o = 0;

    if (in == NULL || out == NULL || cap == 0)
    {
        return false;
    }

    for (; *in != '\0'; in++)
    {
        unsigned char c = (unsigned char)*in;

        if (unreserved(c))
        {
            if (o + 1 >= cap)
            {
                out[0] = '\0';
                return false;
            }

            out[o++] = (char)c;
        }
        else
        {
            if (o + 3 >= cap)
            {
                out[0] = '\0';
                return false;
            }

            out[o++] = '%';
            out[o++] = HEX[c >> 4];
            out[o++] = HEX[c & 0x0F];
        }
    }

    out[o] = '\0';
    return true;
}

bool dd_url_compose(const char *base, const char *extra, const char *key,
                    const char *value, char *out, size_t cap)
{
    if (base == NULL || out == NULL || cap == 0)
    {
        return false;
    }

    int n = snprintf(out, cap, "%s", base);

    if (n < 0 || (size_t)n >= cap)
    {
        return false;
    }

    size_t len = (size_t)n;
    bool has_q = strchr(base, '?') != NULL;
    const char *tail = strchr(base, '?');
    bool needs_amp = has_q && tail[1] != '\0' && base[len - 1] != '&';

    if (extra != NULL)
    {
        while (*extra == '?' || *extra == '&')
        {
            extra++;
        }
    }

    if (extra != NULL && *extra != '\0')
    {
        n = snprintf(out + len, cap - len, "%s%s",
                     has_q ? (needs_amp ? "&" : "") : "?", extra);

        if (n < 0 || (size_t)n >= cap - len)
        {
            return false;
        }

        len += (size_t)n;
        has_q = true;
        needs_amp = out[len - 1] != '&';
    }

    if (key != NULL && key[0] != '\0' && value != NULL)
    {
        char enc[DD_SECRET_LEN * 3];

        if (!dd_url_encode(value, enc, sizeof(enc)))
        {
            return false;
        }

        n = snprintf(out + len, cap - len, "%s%s=%s",
                     has_q ? (needs_amp ? "&" : "") : "?", key, enc);

        if (n < 0 || (size_t)n >= cap - len)
        {
            return false;
        }
    }

    return true;
}

bool dd_url_host_is_ip(const char *url)
{
    if (url == NULL)
    {
        return false;
    }

    const char *h = strstr(url, "://");

    h = (h != NULL) ? h + 3 : url;

    if (*h == '\0' || *h == '/' || *h == ':')
    {
        return false;
    }

    for (; *h != '\0' && *h != '/' && *h != ':'; h++)
    {
        if ((*h < '0' || *h > '9') && *h != '.')
        {
            return false;
        }
    }

    return true;
}

bool dd_topic_expand(const char *topic, const char *prefix, char *out,
                     size_t cap)
{
    if (topic == NULL || out == NULL || cap == 0)
    {
        return false;
    }

    int n;

    if (topic[0] == '~' && topic[1] == '/' && prefix != NULL)
    {
        n = snprintf(out, cap, "%s/%s", prefix, topic + 2);
    }
    else
    {
        n = snprintf(out, cap, "%s", topic);
    }

    return n >= 0 && (size_t)n < cap;
}
