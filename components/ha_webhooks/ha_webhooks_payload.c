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
 * @file ha_webhooks_payload.c
 * @brief The push body: {schema, status, autopid_data, config, gps}
 *        (device-contract v2) built for the poster task, whole or as the
 *        keys that changed since the previous build (`changed` mode, the
 *        prev caches live here). status.device_id/fw_version/hw_version
 *        are guaranteed in EVERY push. Split out of ha_webhooks_post.c
 *        (the 700-line rule) on 2026-10-11, when `sta_ip` joined the
 *        status section (issue #977).
 *
 * Built from RAM only, so the poster's PSRAM stack is safe here:
 *   - status        -> dev_status_manager / wifi_manager / vpn_manager /
 *                      battery_monitor / mdns_manager getters (RAM)
 *   - autopid_data  -> autopid_snapshot()          (RAM cache)
 *   - config        -> autopid_config_json_dup()    (PSRAM cache, no flash)
 *   - gps           -> usb_acm_cli_gps_get()        (RAM fix cache)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"

#include "cJSON.h"

#include "autopid.h"
#include "battery_monitor.h"
#include "dev_status_manager.h"
#include "ha_webhooks.h" /* ha_webhooks_fw_provider_t (2026-10-18) */
#include "mdns_manager.h"
#include "obd_chip.h"
#include "sdkconfig.h"
#include "sleep_manager.h" /* status.sleep_mode (2026-11-15) */
#include "usb_acm_cli.h" /* usb_acm_cli_gps_get: the dongle fix */
#include "vpn_manager.h"
#include "wifi_manager.h" /* wifi_manager_get_sta_ip: status.sta_ip */

#include "ha_webhooks_private.h"

/* device-contract v2: the webhook payload shape version (bump only on
 * breaking changes; HA treats a missing key as 1) */
#define HW_PAYLOAD_SCHEMA 1

/* previous snapshots for "changed" diffing (PSRAM strings) */
static char *s_prev_status EXT_RAM_BSS_ATTR;
static char *s_prev_autopid EXT_RAM_BSS_ATTR;
static char *s_prev_config EXT_RAM_BSS_ATTR;
static char *s_prev_gps EXT_RAM_BSS_ATTR;
static volatile bool s_resync; /* next build = full snapshot */

/* the firmware update keys' provider (the composition root's, 2026-10-18) */
static ha_webhooks_fw_provider_t s_fw_provider;

esp_err_t ha_webhooks_set_fw_provider(ha_webhooks_fw_provider_t fn)
{
    s_fw_provider = fn;
    return ESP_OK;
}

void hw_payload_resync(void)
{
    s_resync = true;
}

static void drop_prev_caches(void)
{
    free(s_prev_status);
    free(s_prev_autopid);
    free(s_prev_config);
    free(s_prev_gps);
    s_prev_status = NULL;
    s_prev_autopid = NULL;
    s_prev_config = NULL;
    s_prev_gps = NULL;
}

/* ---- status section (mirror of /api/status + device_id) ------------------- */

/** The four keys of the device catalog's diagnostic sensors (2026-11-15).
 *  The integration's catalog gives every WiCAN the sensors Sleep mode, CAN
 *  protocol, ECU PID count and AutoPID loop, read from `sleep_mode`,
 *  `can_protocol`, `ecu_pids_num` and `loop_status`. No firmware ever sent
 *  them (not this one, not the 4.x line): four sensors with no value on
 *  every device, seen on a first-time setup with Home Assistant 2026.11.1.
 *  All four are RAM reads (the poster's stack is in PSRAM). */
static void status_catalog_keys(cJSON *s)
{
    sleep_manager_status_t sleep;

    /* Power Saving as it is set: the device that sleeps does not push */
    if (sleep_manager_status(&sleep) == ESP_OK)
    {
        cJSON_AddStringToObject(s, "sleep_mode", sleep.enabled ? "on" : "off");
    }

    /* the vehicle protocol in effect, in words: the pinned one, else the
       current car's; with neither the chip's own search picks */
    char proto[48];

    if (autopid_protocol_text(proto, sizeof(proto)) == 0)
    {
        snprintf(proto, sizeof(proto), "Automatic");
    }

    cJSON_AddStringToObject(s, "can_protocol", proto);

    autopid_stats_t ap;

    if (autopid_stats(&ap) == ESP_OK)
    {
        bool paused = ap.paused_voltage || ap.paused_client ||
                      ap.paused_diag || ap.paused_bus;

        cJSON_AddNumberToObject(s, "ecu_pids_num", (double)ap.pids_loaded);
        cJSON_AddStringToObject(s, "loop_status",
                                ap.running ? "Running"
                                : paused   ? "Paused"
                                           : "Stopped");
    }
}

static cJSON *build_status(void)
{
    cJSON *s = cJSON_CreateObject();

    if (s == NULL)
    {
        return NULL;
    }

    /* device_id: the HA integration binds/validates on this (coordinator
       _validate_device_identity), REQUIRED */
    cJSON_AddStringToObject(s, "device_id",
                            dev_status_manager_device_id());

    /* identity for the device registry, device-type inference and the
       update entity (contract v2 guarantees these in every push: the
       build_payload overlay keeps them alive through diff mode) */
    cJSON_AddStringToObject(s, "fw_version",
                            dev_status_manager_app_version());
    cJSON_AddStringToObject(s, "hw_version", CONFIG_WICAN_HW_VERSION);
    cJSON_AddStringToObject(s, "device_type", CONFIG_WICAN_DEVICE_TYPE);

    /* the revision of the webhook settings in use (2026-11-14): Home
       Assistant compares it with the one it wants and hands its settings
       over in the reply to this push while they differ. "" = none. The key
       says "this firmware reads replies", so it is left out while the user
       pinned the webhook here (manual_override): nothing is taken from a
       reply then, as nothing is taken from a registration. */
    char rev[HW_REV_LEN];

    if (hw_rev_live(rev))
    {
        cJSON_AddStringToObject(s, "webhook_rev", rev);
    }

    /* the manifest updater's word (2026-10-18): the integration's update
       entity reads these instead of the GitHub release list */
    if (s_fw_provider != NULL)
    {
        bool available = false;
        char latest[32] = "";

        if (s_fw_provider(&available, latest, sizeof(latest)))
        {
            cJSON_AddBoolToObject(s, "fw_update_available", available);
            cJSON_AddStringToObject(s, "fw_latest_version", latest);
            cJSON_AddStringToObject(s, "fw_build",
                                    dev_status_manager_app_build());
        }
    }

    const char *host = mdns_manager_hostname();

    if (host != NULL && host[0] != '\0')
    {
        char url[48];

        snprintf(url, sizeof(url), "http://%s", host);
        cJSON_AddStringToObject(s, "mdns", url); /* HA configuration_url */
    }

    cJSON *bits = cJSON_AddObjectToObject(s, "bits");

    for (int i = 0; i < 24; i++)
    {
        EventBits_t bit = (EventBits_t)1u << i;
        const char *name = dev_status_manager_bit_name(bit);

        if (strcmp(name, "unknown") != 0)
        {
            cJSON_AddBoolToObject(bits, name,
                                  dev_status_manager_is_set(bit));
        }
    }

    cJSON_AddBoolToObject(s, "network_connected",
                          dev_status_manager_any_set(
                              DEV_STATUS_NETWORK_CONNECTED_MASK));

    char uptime[HW_TS_LEN] = "";

    dev_status_manager_format_uptime(uptime, sizeof(uptime));
    cJSON_AddStringToObject(s, "uptime", uptime);
    cJSON_AddStringToObject(s, "version",
                            dev_status_manager_app_version());

    float temp_c = 0;

    if (dev_status_manager_temperature(&temp_c) == ESP_OK)
    {
        cJSON_AddNumberToObject(s, "temp_c",
                                (double)((int)(temp_c * 10)) / 10.0);
    }

    /* the integration's fixed sensor entities (attributes.py) */
    bool sta = dev_status_manager_is_set(DEV_STATUS_BIT_STA_ENABLED);
    bool ap = dev_status_manager_is_set(DEV_STATUS_BIT_AP_ENABLED);

    cJSON_AddStringToObject(s, "wifi_mode",
                            (sta && ap) ? "AP+Station"
                            : sta       ? "Station"
                            : ap        ? "AP"
                                        : "Off");

    /* the station's IPv4 (issue #977): the integration's `sta_ip`
       diagnostic sensor and the device card's configuration_url read it
       from the push. The legacy /check_status carried it for the polling
       integration; no webhook ever did, so on a push-based install the
       sensor stayed empty. Present while the station holds an address,
       omitted otherwise: in `changed` mode HA keeps the last one until
       the station joins again, and a new address is a change that goes
       out at once. */
    char sta_ip[16] = "";

    if (wifi_manager_get_sta_ip(sta_ip, sizeof(sta_ip)) == ESP_OK &&
        sta_ip[0] != '\0')
    {
        cJSON_AddStringToObject(s, "sta_ip", sta_ip);
    }

    cJSON_AddStringToObject(
        s, "ble_status",
        dev_status_manager_is_set(DEV_STATUS_BIT_BLE_ENABLED)
            ? "enable" : "disable");

    float volts = 0;

    if (battery_monitor_voltage(&volts) == ESP_OK)
    {
        cJSON_AddNumberToObject(s, "batt_voltage",
                                (double)((int)(volts * 100)) / 100.0);
    }

    /* device-contract v2 ECU/chip health (the last missing status keys):
       ecu_status = is the VEHICLE answering polls; obd_chip_status = the
       MIC3624's READY pin. */
    cJSON_AddStringToObject(s, "ecu_status",
                            autopid_ecu_online() ? "online" : "offline");
    cJSON_AddStringToObject(s, "obd_chip_status",
                            obd_chip_status_ok() ? "ready" : "offline");

    status_catalog_keys(s);

    /* vpn_ip is HA's away-from-home control endpoint (it registers the
       tunnel address as the backup for /api/webhook + buttons).
       vpn_status takes vpn_manager's own words (issue #985: a 4-entry
       table here read past its end once PAUSED joined the enum, so
       `connected` pushed garbage and `paused` read "connecting"). */
    vpn_manager_status_t vs;

    if (vpn_manager_status(&vs) == ESP_OK)
    {
        cJSON_AddStringToObject(s, "vpn_status",
                                vpn_manager_state_str(vs.state));

        char tip[32];

        if (vpn_manager_tunnel_ip(tip, sizeof(tip)) == ESP_OK)
        {
            cJSON_AddStringToObject(s, "vpn_ip", tip);
        }
    }

    return s;
}

/* ---- gps section (device-contract §5.4: HA's device_tracker) -------------- */

/** The contract's `gps` block - the SAME shape as GET /api/gps minus
 *  `valid`/`age_ms`: {latitude, longitude, accuracy (m), altitude (m),
 *  speed (m/s), heading, satellites}. HA's Location tracker reads exactly
 *  this (device_tracker.py); the fix ALSO rides autopid_data as the
 *  gps_* sensors (main's GPS sink). NULL without a LIVE fix - a cached
 *  AGNSS position is never presented as current - so the section drops
 *  out and HA keeps the last known location. Source =
 *  usb_acm_cli_gps_get(): the dongle console's fix, else the
 *  espnetlink_link HTTP-polled fix (main wires that fallback). Bench
 *  2026-09-09: before this block existed the WiCAN Pro tracker in HA
 *  never left "unavailable". */
static cJSON *build_gps(void)
{
    usb_acm_gps_t g;

    if (usb_acm_cli_gps_get(&g) != ESP_OK || !g.valid)
    {
        return NULL;
    }

    cJSON *o = cJSON_CreateObject();

    if (o == NULL)
    {
        return NULL;
    }

    cJSON_AddNumberToObject(o, "latitude", g.latitude);
    cJSON_AddNumberToObject(o, "longitude", g.longitude);
    cJSON_AddNumberToObject(o, "accuracy", g.accuracy_m);
    cJSON_AddNumberToObject(o, "altitude", g.altitude_m);
    cJSON_AddNumberToObject(o, "speed", g.speed_kmph / 3.6); /* m/s */
    cJSON_AddNumberToObject(o, "heading", g.heading_deg);
    cJSON_AddNumberToObject(o, "satellites", g.satellites);
    return o;
}

/** Add the gps block WHOLE (never a key diff: a block without both
 *  latitude and longitude means nothing to the tracker) when it differs
 *  from the previous post, or always in full mode. Takes @p gps. */
static void add_gps_section(cJSON *root, cJSON *gps, bool full)
{
    char *now = cJSON_PrintUnformatted(gps);
    bool changed = full || now == NULL || s_prev_gps == NULL ||
                   strcmp(now, s_prev_gps) != 0;

    if (changed)
    {
        cJSON_AddItemToObject(root, "gps", gps);
    }
    else
    {
        cJSON_Delete(gps);
    }

    if (now != NULL)
    {
        free(s_prev_gps);
        s_prev_gps = now; /* PSRAM heap via cJSON's default alloc */
    }
}

/* ---- diff: keys in curr that are new or changed vs prev ------------------- */

static cJSON *diff_object(const cJSON *curr, const char *prev_json)
{
    cJSON *out = cJSON_CreateObject();

    if (out == NULL)
    {
        return NULL;
    }

    cJSON *prev = prev_json != NULL ? cJSON_Parse(prev_json) : NULL;
    const cJSON *it = NULL;

    cJSON_ArrayForEach(it, curr)
    {
        const char *key = it->string;

        if (key == NULL)
        {
            continue;
        }

        bool changed = true;
        const cJSON *p = prev != NULL
                             ? cJSON_GetObjectItemCaseSensitive(prev, key)
                             : NULL;

        if (p != NULL)
        {
            changed = !cJSON_Compare(it, p, false);
        }

        if (changed)
        {
            cJSON_AddItemToObject(out, key, cJSON_Duplicate(it, true));
        }
    }

    if (prev != NULL)
    {
        cJSON_Delete(prev);
    }

    return out;
}

/** Add @p curr to @p root under @p name as full (dup) or diff; update the
 *  matching prev-cache (@p prev_cache NULL = the configured `full` mode,
 *  which keeps none). Empty sections are omitted (legacy behaviour). */
static void add_section(cJSON *root, const char *name, cJSON *curr,
                        bool full, char **prev_cache)
{
    if (curr == NULL)
    {
        return;
    }

    cJSON *section;

    if (full)
    {
        section = cJSON_Duplicate(curr, true);
    }
    else
    {
        section = diff_object(curr, prev_cache != NULL ? *prev_cache : NULL);
    }

    /* refresh the prev cache from the full current object, after a full
       build too: the resync's full push primes the diff, so the push after
       a registration is a real diff. Until 2026-10-11 the cache stayed
       empty through the resync and the next push went out whole once more
       (the sta_ip bench caught it). */
    if (prev_cache != NULL)
    {
        char *now = cJSON_PrintUnformatted(curr);

        if (now != NULL)
        {
            free(*prev_cache);
            *prev_cache = now; /* PSRAM heap via cJSON's default alloc */
        }
    }

    if (section != NULL && cJSON_GetArraySize(section) > 0)
    {
        cJSON_AddItemToObject(root, name, section);
    }
    else if (section != NULL)
    {
        cJSON_Delete(section); /* empty -> omit */
    }
}

char *hw_payload_build(bool full_mode)
{
    cJSON *root = cJSON_CreateObject();

    if (root == NULL)
    {
        return NULL;
    }

    /* the configured `full` mode keeps no caches; `changed` diffs against
       the previous build */
    bool full = full_mode;
    char **status_cache = full_mode ? NULL : &s_prev_status;
    char **autopid_cache = full_mode ? NULL : &s_prev_autopid;
    char **config_cache = full_mode ? NULL : &s_prev_config;

    /* a fresh registration wants the complete state, not a diff against a
       previous HA's baseline */
    if (s_resync)
    {
        s_resync = false;
        drop_prev_caches();
        full = true;
    }

    cJSON_AddNumberToObject(root, "schema", HW_PAYLOAD_SCHEMA);

    /* status */
    cJSON *status = build_status();

    add_section(root, "status", status, full, status_cache);

    /* contract v2: status.device_id / fw_version / hw_version are
       guaranteed in EVERY push (identity check + update entity), and so is
       webhook_rev where the status has it (Home Assistant decides on this
       push alone whether its reply carries settings). Diff mode drops
       unchanged keys, so overlay them from the fresh build. */
    cJSON *sent = cJSON_GetObjectItemCaseSensitive(root, "status");

    if (sent == NULL)
    {
        sent = cJSON_AddObjectToObject(root, "status");
    }

    if (sent != NULL && status != NULL)
    {
        static const char *IDENTITY_KEYS[] =
        { "device_id", "fw_version", "hw_version", "fw_update_available",
          "webhook_rev" };

        for (size_t i = 0;
             i < sizeof(IDENTITY_KEYS) / sizeof(IDENTITY_KEYS[0]); i++)
        {
            if (cJSON_GetObjectItemCaseSensitive(sent,
                                                 IDENTITY_KEYS[i]) != NULL)
            {
                continue;
            }

            const cJSON *v = cJSON_GetObjectItemCaseSensitive(
                status, IDENTITY_KEYS[i]);

            if (v != NULL)
            {
                cJSON_AddItemToObject(sent, IDENTITY_KEYS[i],
                                      cJSON_Duplicate(v, true));
            }
        }
    }

    cJSON_Delete(status);

    /* autopid_data */
    cJSON *data = NULL;

    if (autopid_snapshot(&data) == ESP_OK && data != NULL)
    {
        add_section(root, "autopid_data", data, full, autopid_cache);
        cJSON_Delete(data);
    }

    /* config (from the PSRAM cache: no flash) */
    char *cfg_json = NULL;

    if (autopid_config_json_dup(&cfg_json) == ESP_OK && cfg_json != NULL)
    {
        cJSON *cfg = cJSON_Parse(cfg_json);

        free(cfg_json);

        if (cfg != NULL)
        {
            add_section(root, "config", cfg, full, config_cache);
            cJSON_Delete(cfg);
        }
    }

    /* gps (contract §5.4 -> HA's device_tracker); omitted without a
       live fix */
    cJSON *gps = build_gps();

    if (gps != NULL)
    {
        add_gps_section(root, gps, full);
    }

    char *body = cJSON_PrintUnformatted(root);

    cJSON_Delete(root);
    return body;
}
