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
 *        (the poster task) and ha_webhooks_cli.c.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HW_URL_LEN       192   /* legacy cap */
#define HW_CERTSET_LEN   32
#define HW_NETS_LEN      200   /* "active on" list (2026-11-08) */
#define HW_STATUS_LEN    16
#define HW_ERR_LEN       160
#define HW_TS_LEN        32

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
} hw_config_t;

/** Runtime stats: RAM only, never persisted (poster has no flash). */
typedef struct
{
    char     status[HW_STATUS_LEN];  /* "ok" / "failed" / "disabled" /
                                        "rejected" (HA 403 pause)         */
    char     last_post[HW_TS_LEN];   /* UTC ISO8601 of last success       */
    uint32_t success_count;
    uint32_t fail_count;
    uint32_t retries;
    char     last_error[HW_ERR_LEN];
    char     last_error_time[HW_TS_LEN];
} hw_stats_t;

/* ---- settings (ha_webhooks_settings.c) ------------------------------------ */

/** Register the "ha_webhooks" descriptor with settings_manager. */
esp_err_t hw_settings_register(void);

/* ---- config accessors (ha_webhooks.c) ------------------------------------- */

/** Copy the live config (mutex-guarded). false before configured. */
bool hw_config_get(hw_config_t *out);

/** Store @p cfg as the live config (mutex-guarded) and mark the component
 *  configured. The write path for BOTH the boot on_apply and the URL push. */
void hw_config_store(const hw_config_t *cfg);

/** Apply a URL-push LIVE (update the cache the poster reads) AND persist
 *  it to settings so it survives reboot. Used by POST/DELETE /api/webhook. */
esp_err_t hw_config_apply_live(const hw_config_t *cfg);

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
