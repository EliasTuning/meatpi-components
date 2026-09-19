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
 * @file data_destinations_private.h
 * @brief Internals shared between the PURE core (data_destinations_core.c:
 *        config parse, scheduler/backoff, URL/form/ABRP builders — host-
 *        tested), the settings descriptor, the poster task, the
 *        transports, the HTTP routes and the CLI.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "cJSON.h"

#include "data_destinations.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- table bounds (schema maxLength + 1) ---------------------------------- */
#define DD_MAX          DATA_DESTINATIONS_MAX
#define DD_NAME_LEN     16
#define DD_URL_LEN      256
#define DD_SECRET_LEN   256   /* bearer / API key value / ABRP user token   */
#define DD_SMALL_LEN    64    /* header/query name, basic user/pass, model  */
#define DD_QUERY_LEN    256   /* extra "k=v&k2=v2" string                   */
#define DD_CERTSET_LEN  25
#define DD_ERR_LEN      96
#define DD_TS_LEN       32

/* ---- policy knobs ---------------------------------------------------------- */
#define DD_BACKOFF_AFTER      3       /* consecutive failures before backoff  */
#define DD_BACKOFF_FLOOR_MS   10000   /* first backed-off interval at least   */
#define DD_BACKOFF_MAX_MULT   8       /* cap = max(period * 8, 60 s)          */
#define DD_BACKOFF_CAP_MIN_MS 60000
#define DD_BACKOFF_CAP_MAX_MS 600000  /* never wait more than 10 min          */
#define DD_HTTP_TIMEOUT_MS    6000
#define DD_ABRP_DEFAULT_URL   "https://api.iternio.com/1/tlm/send"

typedef enum
{
    DD_TYPE_MQTT = 0,
    DD_TYPE_HTTP,
    DD_TYPE_HTTPS,
    DD_TYPE_ABRP,
} dd_type_t;

typedef enum
{
    DD_AUTH_NONE = 0,
    DD_AUTH_BEARER,
    DD_AUTH_API_KEY_HEADER,
    DD_AUTH_API_KEY_QUERY,
    DD_AUTH_BASIC,
} dd_auth_t;

/** One configured destination (parsed + normalized settings item). */
typedef struct
{
    char      name[DD_NAME_LEN];
    dd_type_t type;
    bool      enabled;
    char      url[DD_URL_LEN];              /* topic (mqtt) or URL          */
    uint32_t  period_s;
    dd_auth_t auth;
    char      auth_token[DD_SECRET_LEN];    /* bearer / api-key value /
                                               ABRP user token              */
    char      auth_name[DD_SMALL_LEN];      /* api-key header / query name  */
    char      basic_username[DD_SMALL_LEN];
    char      basic_password[DD_SMALL_LEN];
    char      api_key[DD_SECRET_LEN];       /* ABRP developer api_key       */
    char      query[DD_QUERY_LEN];          /* extra query params, raw      */
    char      cert_set[DD_CERTSET_LEN];     /* cert_manager set; "" bundle  */
    char      car_model[DD_SMALL_LEN];      /* ABRP car_model               */
    bool      retain;                       /* mqtt                         */
    bool      full_first;                   /* http(s): config+status once  */
} dd_dest_t;

/** The applied table. */
typedef struct
{
    bool      enabled;
    size_t    n;
    dd_dest_t dest[DD_MAX];
} dd_config_t;

/** Per-destination runtime state (RAM only, never persisted). */
typedef struct
{
    uint32_t success;
    uint32_t fail;
    uint32_t skipped_offline;   /* due laps skipped: link/broker down     */
    uint32_t consec_failures;
    uint32_t backoff_ms;        /* 0 = none                               */
    int64_t  next_due_us;       /* 0 = due now                            */
    bool     settings_sent;     /* http(s): the config+status push landed */
    bool     was_ok;            /* last attempt outcome (log transitions) */
    bool     ever_tried;
    int      last_status;       /* HTTP status of the last attempt, or 0  */
    char     last_error[DD_ERR_LEN];
    char     last_error_time[DD_TS_LEN];
    char     last_ok_time[DD_TS_LEN];
} dd_state_t;

/* ---- PURE core (data_destinations_core.c) --------------------------------- */

const char *dd_type_str(dd_type_t t);
const char *dd_auth_str(dd_auth_t a);
bool dd_type_parse(const char *s, dd_type_t *out);
bool dd_auth_parse(const char *s, dd_auth_t *out);

/**
 * Parse + normalize ONE settings array item into @p out. Fills defaults
 * for missing keys, prepends the scheme the type implies, applies the
 * ABRP default URL, and rejects an ENABLED entry that cannot work
 * (missing URL, https type with an http:// URL, ABRP without a user
 * token, auth modes without their credential). @p err receives the
 * reason (prefixed with the entry name).
 */
esp_err_t dd_parse_dest(const cJSON *item, dd_dest_t *out, char *err,
                        size_t err_len);

/** Parse the whole settings document (`enabled` + `destinations[]`).
 *  Duplicate names are rejected. */
esp_err_t dd_config_parse(const cJSON *settings, dd_config_t *out,
                          char *err, size_t err_len);

/** True when the destination is due at @p now_us (next_due_us 0 = now). */
bool dd_sched_due(const dd_state_t *st, int64_t now_us);

/** The next backed-off interval: doubles from the period, floor
 *  DD_BACKOFF_FLOOR_MS, cap max(period*DD_BACKOFF_MAX_MULT, 60 s) ≤ 10 min. */
uint32_t dd_backoff_next(uint32_t prev_ms, uint32_t period_ms);

/** Book an attempt's outcome: counters, backoff, next due time. */
void dd_sched_after(dd_state_t *st, uint32_t period_s, bool ok,
                    int64_t now_us);

/** Book a lap skipped because the link/broker was down (no backoff). */
void dd_sched_skip(dd_state_t *st, uint32_t period_s, int64_t now_us);

/** RFC 3986 percent-encoding (unreserved chars pass). false when @p out
 *  is too small. */
bool dd_url_encode(const char *in, char *out, size_t cap);

/**
 * @p base + the user's extra query string (raw, leading ?/& tolerated)
 * + an optional `key=value` pair (value percent-encoded). Handles an
 * existing `?` in @p base. false when @p out is too small.
 */
bool dd_url_compose(const char *base, const char *extra, const char *key,
                    const char *value, char *out, size_t cap);

/** True when the URL's host is a dotted IPv4 literal (TLS CN skip). */
bool dd_url_host_is_ip(const char *url);

/** `~/x` -> `<prefix>/x`; anything else copied verbatim. */
bool dd_topic_expand(const char *topic, const char *prefix, char *out,
                     size_t cap);

/**
 * Build ABRP's `tlm` object from the flat autopid snapshot: the legacy
 * name map (SOC->soc, HV_W->power, SPEED->speed, CHARGING->is_charging,
 * ...), the dongle GPS fix (gps_latitude->lat, gps_longitude->lon,
 * gps_altitude->elevation, gps_heading->heading, gps_speed->speed when
 * no SPEED), pass-through lat/lon/elevation, `utc` (@p utc_s unless the
 * snapshot carries one) and `car_model` when given. Booleans and "on"/
 * "off" strings become 0/1. Caller frees.
 */
cJSON *dd_abrp_tlm(const cJSON *snapshot, const char *car_model,
                   int64_t utc_s);

/** `token=<enc>&tlm=<enc>` — the form body ABRP expects. */
bool dd_abrp_form(const char *token, const char *tlm_json, char *out,
                  size_t cap);

/** `APIKEY <key>` (prefix added unless already present). */
bool dd_abrp_auth_value(const char *api_key, char *out, size_t cap);

/**
 * Interpret an ABRP response: a JSON body with `"status"` other than
 * `"ok"` is a failure even on HTTP 200 (@p err gets the status + any
 * `missing`/`errors` detail); a non-JSON or empty body defers to the
 * HTTP status (@p http_ok).
 */
bool dd_abrp_response_ok(const char *body, bool http_ok, char *err,
                         size_t err_len);

/* ---- settings (data_destinations_settings.c) ------------------------------ */

esp_err_t dd_settings_register(void);

/* ---- config / state caches (data_destinations.c) -------------------------- */

/** Store the boot-applied table (called from on_apply only). */
void dd_config_store(const dd_config_t *cfg);

/** True once on_apply stored a table. */
bool dd_config_is_configured(void);

/** The applied table is ~10 KB: NEVER copy it onto a caller's stack
 *  (the first build overflowed the main task in _start). Borrow it under
 *  the lock instead, or read one destination while holding it. */
void dd_config_lock(void);
void dd_config_unlock(void);
const dd_config_t *dd_config_peek(void);   /* valid while locked only */

/** Number of configured destinations. */
size_t dd_config_count(void);

/** Index of the destination named @p name, -1 when none. */
int dd_config_find(const char *name);

/** Copy destination @p i's runtime state (~250 B). */
bool dd_state_get(size_t i, dd_state_t *out);

/** UTC ISO8601 into a DD_TS_LEN buffer. */
void dd_format_utc(char out[DD_TS_LEN]);

/* ---- transports (data_destinations_post.c) -------------------------------- */

/** The flat snapshot + `timestamp` as a JSON string. Caller frees. */
char *dd_build_data_json(void);

/**
 * Deliver @p d once: builds the payload, does the MQTT publish / HTTP
 * POST / ABRP form POST. @p st->settings_sent decides the HTTP body
 * shape and is set on success. Fills @p out (status/error/elapsed).
 * Returns @p out->ok.
 */
bool dd_post_one(const dd_dest_t *d, dd_state_t *st,
                 data_destinations_result_t *out);

#ifdef __cplusplus
}
#endif
