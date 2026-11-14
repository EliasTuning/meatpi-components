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
 * @file ha_webhooks_private.h
 * @brief Internals shared between ha_webhooks.c (settings/lifecycle),
 *        ha_webhooks_http.c (the /api/webhook endpoint), ha_webhooks_post.c
 *        (the poster task), ha_webhooks_reg.c (the registration object, the
 *        revision and the reply rules: pure, host tested) and
 *        ha_webhooks_cli.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HW_URL_LEN       192   /* legacy cap */
#define HW_CERTSET_LEN   32
#define HW_NETS_LEN      200   /* "active on" list (2026-11-08) */
#define HW_STATUS_LEN    16
#define HW_ERR_LEN       160
#define HW_TS_LEN        32
#define HW_REV_LEN       17    /* a revision: 16 characters + NUL          */
#define HW_REV_SUM_LEN   9     /* its fingerprint: 8 hex characters + NUL  */
#define HW_REPLY_MAX     2048  /* a longer reply body is ignored           */

/** Boot-applied + live config (settings-backed, mutable by the URL push). */
typedef struct
{
    bool     enabled;
    char     url[HW_URL_LEN];
    char     url2[HW_URL_LEN];       /* failover; "" = none               */
    uint32_t interval_s;             /* 1..3600                           */
    bool     data_mode_full;         /* true = full, false = changed/diff */
    bool     gzip;                   /* Content-Encoding: gzip the push
                                        (either data_mode; needs HA
                                        integration >= 2026-07-10)        */
    bool     manual_override;        /* pin URL against HA auto-push       */
    char     cert_set[HW_CERTSET_LEN]; /* cert_manager set; "" = bundle   */
    char     networks[HW_NETS_LEN];  /* "active on" (2026-11-08): "" =
                                        every network, else the WiFi
                                        networks' names, comma-separated  */
    char     rev[HW_REV_LEN];        /* the revision Home Assistant gave
                                        the webhook settings (2026-11-14);
                                        "" = none                         */
    char     rev_sum[HW_REV_SUM_LEN]; /* the fingerprint of the settings
                                        the revision was stored with      */
} hw_config_t;

/** Runtime stats: RAM only, never persisted (poster has no flash). */
typedef struct
{
    char     status[HW_STATUS_LEN];  /* "ok" / "failed" / "disabled" /
                                       "removed" (a 410, 2026-11-15) /
                                        "rejected" (HA 403 pause)         */
    char     last_post[HW_TS_LEN];   /* UTC ISO8601 of last success       */
    uint32_t success_count;
    uint32_t fail_count;
    uint32_t retries;
    char     last_error[HW_ERR_LEN];
    char     last_error_time[HW_TS_LEN];
    uint32_t reply_applied;          /* settings taken from a push reply  */
    uint32_t reply_ignored;          /* reply bodies refused              */
    char     reply_last[HW_STATUS_LEN]; /* hw_reply_str() of the last
                                        reply that carried a body         */
} hw_stats_t;

/* ---- the registration object, the revision, the reply (ha_webhooks_reg.c) -
 * Pure: strings and cJSON only, no IDF glue (host tested). */

/** Why a registration object was refused. */
typedef enum
{
    HW_REG_OK = 0,
    HW_REG_URL,          /* url missing, or not http:// or https://        */
    HW_REG_URL_LONG,     /* (strict) an address over HW_URL_LEN - 1        */
    HW_REG_URLS_MANY,    /* more than two urls                             */
    HW_REG_URLS_FIRST,   /* urls[0] is not url                             */
    HW_REG_URLS_ENTRY,   /* urls[1] not http:// or https://                */
    HW_REG_INTERVAL,     /* (strict) interval missing or outside 1..3600   */
    HW_REG_ENABLED,      /* (strict) enabled present and not a boolean     */
    HW_REG_REV,          /* rev not a revision, or missing where required  */
} hw_reg_err_t;

/** What a push's reply said about the webhook settings. */
typedef enum
{
    HW_REPLY_NONE = 0,   /* no body, or a JSON object without `webhook`:
                            a plain "delivered"                            */
    HW_REPLY_APPLY,      /* the settings are to be stored                  */
    HW_REPLY_SAME,       /* the revision is the one in use                 */
    HW_REPLY_PINNED,     /* the user pinned the webhook on the device      */
    HW_REPLY_UNTRUSTED,  /* not from where a registration would be taken   */
    HW_REPLY_TOO_LONG,   /* the body went on past HW_REPLY_MAX             */
    HW_REPLY_NOT_JSON,   /* the body is not a JSON object                  */
    HW_REPLY_INVALID,    /* the webhook object failed validation           */
    HW_REPLY_FAILED,     /* valid, the device could not store it           */
    HW_REPLY_REMOVED,    /* a 410: Home Assistant removed this device      */
} hw_reply_t;

/** True for a revision: 1 to 16 characters of `A-Z a-z 0-9 _ -`. */
bool hw_rev_valid(const char *rev);

/** The fingerprint of the webhook settings a revision stands for (enabled,
 *  url, url2, interval_s), 8 hex characters. */
void hw_rev_sum(const hw_config_t *c, char out[HW_REV_SUM_LEN]);

/** Give @p c the revision @p rev (NULL or "" = none) with the fingerprint
 *  of its settings as they are now. */
void hw_rev_stamp(hw_config_t *c, const char *rev);

/** The revision to report for @p c: the stored one while the settings are
 *  still the ones it was stored with, "" otherwise. Whatever route changed
 *  them without a revision (the web interface, a restore, a factory reset)
 *  is caught here, with no hook in that route. */
const char *hw_rev_reported(const hw_config_t *c);

/**
 * Read a registration object (the body of `POST /api/webhook`, or the
 * `webhook` object of a push reply) into @p out, starting from @p cur.
 *
 * Without a `rev` the legacy rules hold (a missing or out of range interval
 * keeps the current one, a long address is cut) and @p out carries no
 * revision. With a `rev` every field has to be right, or the object is
 * refused: a stored revision must stand for exactly the settings Home
 * Assistant sent. @p need_rev refuses an object without one (a reply).
 * `manual_override` is the caller's business.
 */
hw_reg_err_t hw_reg_parse(const cJSON *obj, const hw_config_t *cur,
                          bool need_rev, hw_config_t *out);

/** The text of a refusal (the 400 body of `POST /api/webhook`). */
const char *hw_reg_err_str(hw_reg_err_t e);

/**
 * May the reply to a push change the webhook settings? Only where the
 * server is known to be Home Assistant, or where a registration would be
 * taken: @p https_named = the push went over https to a host name (chain
 * and name verified); else the peer has to be a private address
 * (@p peer_known, @p peer_private) and the push must not have left through
 * the station address of a network marked untrusted (@p via_untrusted_sta).
 */
bool hw_reply_trusted(bool https_named, bool peer_known, bool peer_private,
                      bool via_untrusted_sta);

/**
 * Decide what the body of a 200 reply means. HW_REPLY_APPLY fills @p out
 * (the settings to store, revision stamped); HW_REPLY_INVALID fills
 * @p err. @p truncated = the body went on past the read limit.
 */
hw_reply_t hw_reply_decide(const char *body, size_t len, bool truncated,
                           const hw_config_t *cur, bool trusted,
                           hw_config_t *out, hw_reg_err_t *err);

/**
 * Decide what a 410 Gone means (2026-11-15). Home Assistant keeps the
 * webhook of a device it removed answering 410 with
 * `{"webhook": {"removed": true}}`. HW_REPLY_REMOVED = clear the webhook.
 * HW_REPLY_PINNED / HW_REPLY_UNTRUSTED = the marker is there and the device
 * keeps its settings (the user pinned them here; the server is not one a
 * registration would be taken from). HW_REPLY_NONE = a 410 without the
 * marker, some other server's: a failed push like any other.
 */
hw_reply_t hw_gone_decide(const char *body, size_t len, bool truncated,
                          const hw_config_t *cur, bool trusted);

/** Take the webhook out of @p c: no address, off, no revision. The user's
 *  own settings stay (the interval, the mode, the networks, the pin). What
 *  `DELETE /api/webhook` and a 410 from Home Assistant leave behind. */
void hw_config_unregister(hw_config_t *c);

/** A word for `GET /api/webhook` and the log: "" for HW_REPLY_NONE. */
const char *hw_reply_str(hw_reply_t r);

/* ---- settings (ha_webhooks_settings.c) ------------------------------------ */

/** Register the "ha_webhooks" descriptor with settings_manager. */
esp_err_t hw_settings_register(void);

/* ---- config accessors (ha_webhooks.c) ------------------------------------- */

/** Copy the live config (mutex-guarded). false before configured. */
bool hw_config_get(hw_config_t *out);

/** The revision the pushes report, into @p out: hw_rev_reported() of the
 *  live config, read under the lock without a copy of the config (the
 *  payload builder sits deep on the poster's stack). False while there is
 *  none to report: the webhook is pinned (manual_override) or the component
 *  is unconfigured. */
bool hw_rev_live(char out[HW_REV_LEN]);

/** Store @p cfg as the live config (mutex-guarded) and mark the component
 *  configured. The write path for BOTH the boot on_apply and the URL push. */
void hw_config_store(const hw_config_t *cfg);

/** Apply a URL-push LIVE (update the cache the poster reads) AND persist
 *  it to settings so it survives reboot. Used by POST/DELETE /api/webhook
 *  (@p resync true: the next push is a full snapshot for a new Home
 *  Assistant) and by a push reply (@p resync false: the same Home
 *  Assistant, the diff goes on). Safe from a PSRAM stack: the Settings
 *  Manager's storage runs through flash_jobs. */
esp_err_t hw_config_apply_live(const hw_config_t *cfg, bool resync);

/* ---- stats accessors (ha_webhooks.c) -------------------------------------- */

void hw_stats_get(hw_stats_t *out);
void hw_stats_set(const hw_stats_t *in);

/* ---- payload (ha_webhooks_payload.c) -------------------------------------- */

/** The push body {schema, status, autopid_data, config, gps} as a heap
 *  string (caller frees); @p full_mode = every section whole each time
 *  (the configured `full` mode), else the keys that changed since the
 *  previous build, the first build after a registration whole. NULL on
 *  allocation failure. Built from RAM sources only: safe from the
 *  poster's PSRAM stack. */
char *hw_payload_build(bool full_mode);

/** Make the NEXT build a full snapshot (drop the diff caches). */
void  hw_payload_resync(void);

/* ---- poster (ha_webhooks_post.c) ------------------------------------------ */

esp_err_t hw_poster_start(void);
void      hw_poster_stop(void);

/** Force the NEXT post to be a full snapshot (drop the diff caches) and
 *  end a rejected pause: a newly-registered HA needs the whole picture,
 *  not a diff against stale state. Called on a URL push. */
void      hw_poster_resync(void);

/** UTC ISO8601 into a HW_TS_LEN buffer. */
void hw_format_utc(char out[HW_TS_LEN]);

#ifdef __cplusplus
}
#endif
