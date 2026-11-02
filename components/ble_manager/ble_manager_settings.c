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
 * @file ble_manager_settings.c
 * @brief settings_manager descriptor for ble_manager: field-table schema
 *        (source of truth for shape/ranges/defaults), on_apply (parses into
 *        the boot-applied config) and the v2/v3 migration hook.
 */
#include <stdio.h>

#include "esp_attr.h"

#include "settings_manager.h"

#include "ble_manager_private.h"

/* enabled=false by default: BLE is opt-in (it shares the radio with WiFi;
 * coexistence policy is the composition root's business: README). */
static const settings_field_t FIELDS[] =
{
    SETTINGS_BOOL("enabled", false),
    SETTINGS_INT("passkey", 0, 999999, 123456),
    SETTINGS_INT("tx_power_dbm", -12, 9, 9),
    SETTINGS_BOOL("pairing_at_boot", true),
    /* v6 (2026-11-02, Ali: behave as firmware 4.5x did). `address` = the
       address the device advertises with: `public` = the fixed hardware
       address (the station MAC + 2, what 4.x advertised: Home Assistant
       presence by MAC, an app that remembers the address), `private` = a
       resolvable private address that changes at every advertising restart
       and every 15 minutes (the v6 betas before this). A bond holds the
       public identity either way, so the switch keeps paired phones.
       `security_request` = the device asks the phone to pair right at the
       connect (4.x: esp_ble_set_encryption on connect while pairing was
       allowed), so an app's first write lands on a secured link; off for a
       Windows app with its own pairing ceremony (the 2026-09-21 finding). */
    SETTINGS_STR_ENUM("address", "public,private", "public"),
    SETTINGS_BOOL("security_request", true),
    /* v2: bonding = keep long-term keys so a paired phone reconnects
       without re-entering the passkey. Disable for a stricter posture:
       the link is still encrypted+MITM-protected per session, but no
       long-term key is exchanged/kept, so every reconnect re-pairs and
       nothing reusable is left on the device (meatpi 2026-07-08). */
    SETTINGS_BOOL("bonding", true),
    /* v3: sc_only = require LE Secure Connections. Default true refuses a
       peer that tries to downgrade to weak LEGACY pairing (whose static-
       passkey exchange is brute-forceable): closes the downgrade attack
       at a negligible interop cost (every phone since ~2014 does SC).
       Disable only for a genuinely pre-4.2 accessory (meatpi 2026-07-09). */
    SETTINGS_BOOL("sc_only", true),
    /* the connection window the device REQUESTS on connect: iOS-friendly
       legacy default, or short-interval max performance (user's choice) */
    SETTINGS_STR_ENUM("conn_profile", "ios,android_fast", "ios"),
    /* v4 (2026-09-21): BLE 5 as a user choice. `phy` = the PHY the device
       prefers on a link (requested after connect; the central decides, a
       4.2 phone keeps 1M): 1m = today's behaviour, 2m = double the on-air
       bit rate, coded = long range, auto = 1M or 2M. `advertising` = which
       advertising set(s) run: legacy = the 4.2-visible set every scanner
       sees, extended = the 5.0 set only (invisible to 4.2 phones), both. */
    SETTINGS_STR_ENUM("phy", "1m,2m,coded,auto", "1m"),
    SETTINGS_STR_ENUM("advertising", "legacy,extended,both", "legacy"),
    /* v5 (2026-10-31): the Bluetooth memory profile. The ESP32-S3
       controller's sizing is read once at esp_bt_controller_init, so a
       setting trades internal RAM for Bluetooth features at the next boot
       (wifi_ram_profile's model). `full` = the build's Kconfig values
       (today's behaviour, the BLE 5 options), `lean` = one phone at a time,
       legacy advertising on the 1M PHY, no scanning (the product never
       scans), `custom` = the three knobs below (only read then; their
       ranges are the controller's, so no combination is invalid except a
       scan cache of 1..9, which on_validate names). The controller's BLE 5
       feature set is not a knob: the NimBLE host is built with it and a
       controller without it crashed the device at the bring-up, three
       times, into the park brake (bench, 2026-10-31). Lean from day one:
       Bluetooth on the Full profile took about 38 KB of internal RAM, which
       left a default-mode device one burst from failed saves and no room
       for Tailscale (issue 1024). A device with a stored BLE 5 choice
       migrates to Full. */
    SETTINGS_STR_ENUM("ble_ram_profile", "full,lean,custom", "lean"),
    SETTINGS_INT("ble_max_act", 2, 10, 4),
    SETTINGS_INT("ble_acl_tx_bufs", 0, 12, 12),
    SETTINGS_INT("ble_scan_cache", 0, 1000, 100),
};

static blm_config_t s_cfg EXT_RAM_BSS_ATTR;
static bool s_configured;

static int get_int(const cJSON *obj, const char *key, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);

    return cJSON_IsNumber(v) ? (int)v->valuedouble : def;
}

static const char *get_str(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);

    return cJSON_IsString(v) ? v->valuestring : NULL;
}

const blm_config_t *blm_core_config(void)
{
    return &s_cfg;
}

bool blm_settings_is_configured(void)
{
    return s_configured;
}

static esp_err_t on_apply(const cJSON *settings)
{
    const cJSON *v;

    s_cfg.enabled =
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(settings, "enabled"));
    s_cfg.pairing_at_boot = !cJSON_IsFalse(
        cJSON_GetObjectItemCaseSensitive(settings, "pairing_at_boot"));
    s_cfg.bonding = !cJSON_IsFalse(
        cJSON_GetObjectItemCaseSensitive(settings, "bonding"));
    s_cfg.sc_only = !cJSON_IsFalse(
        cJSON_GetObjectItemCaseSensitive(settings, "sc_only"));
    /* v6: the advertised address and the pairing request at connect */
    s_cfg.address_private = blm_ident_address_private(get_str(settings, "address"));
    s_cfg.security_request = !cJSON_IsFalse(
        cJSON_GetObjectItemCaseSensitive(settings, "security_request"));
    v = cJSON_GetObjectItemCaseSensitive(settings, "passkey");
    s_cfg.passkey = cJSON_IsNumber(v) ? (uint32_t)v->valuedouble : 123456;
    v = cJSON_GetObjectItemCaseSensitive(settings, "tx_power_dbm");
    s_cfg.tx_power_dbm =
        blm_ident_clamp_tx_power(cJSON_IsNumber(v) ? (int)v->valuedouble : 9);
    v = cJSON_GetObjectItemCaseSensitive(settings, "conn_profile");
    blm_ident_conn_window(cJSON_IsString(v) ? v->valuestring : NULL,
                          &s_cfg.conn_min_units, &s_cfg.conn_max_units);
    v = cJSON_GetObjectItemCaseSensitive(settings, "phy");
    s_cfg.phy_mask = blm_ident_phy_mask(cJSON_IsString(v) ? v->valuestring : NULL);
    v = cJSON_GetObjectItemCaseSensitive(settings, "advertising");
    s_cfg.adv_mode = blm_ident_adv_mode(cJSON_IsString(v) ? v->valuestring : NULL);

    /* v5: the memory profile and the custom knobs (the backend maps them
       onto the controller config at the stack bring-up) */
    s_cfg.ram_profile = blm_ident_ram_profile(get_str(settings, "ble_ram_profile"));
    s_cfg.ram_custom.max_act = (uint8_t)get_int(settings, "ble_max_act", 4);
    s_cfg.ram_custom.acl_tx_bufs = (uint8_t)get_int(settings, "ble_acl_tx_bufs", 12);
    s_cfg.ram_custom.scan_cache = (uint16_t)get_int(settings, "ble_scan_cache", 100);
    s_cfg.ram_custom.ble5 = true; /* the mapping keeps the build's; see the header */

    if (s_cfg.ram_profile == BLM_RAM_LEAN)
    {
        /* Lean runs the legacy set on 1M: on_validate refuses the other
           words, and a stored object from before the rule is brought in
           line here rather than failing its boot */
        s_cfg.adv_mode = BLM_ADV_LEGACY;
        s_cfg.phy_mask = BLM_PHY_1M;
    }

    s_configured = true;
    return ESP_OK;
}

/* The one rule the schema cannot state: the Lean profile and the BLE 5
   words (extended advertising, a PHY other than 1M) do not go together,
   and a custom scan cache is 0 or a usable size. The reason reaches the
   UI and the API (standard §4.1). */
static esp_err_t on_validate(const cJSON *settings, char *err, size_t err_len)
{
    const char *why = blm_ident_ram_check(
        blm_ident_ram_profile(get_str(settings, "ble_ram_profile")),
        blm_ident_adv_mode(get_str(settings, "advertising")),
        blm_ident_phy_mask(get_str(settings, "phy")),
        get_int(settings, "ble_scan_cache", 100));

    if (why != NULL)
    {
        snprintf(err, err_len, "%s", why);
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

/* v1 -> v2 added the `bonding` flag, v3 `sc_only`, v4 `phy` +
   `advertising`; every one is covered by its fill-missing default (the
   historic behaviour). v4 -> v5 (2026-10-31) added the memory profile:
   a device that chose a BLE 5 option keeps it on the Full profile, the
   others take the new default (Lean) through the fill-missing rule.
   v5 -> v6 (2026-11-02) added `address` and `security_request`: every
   stored object takes the defaults (public, on), the 4.x behaviour, through
   the fill-missing rule; a device that ran the v6 betas with the private
   address keeps its bonds (they hold the public identity). The hook must
   exist in any case: without it a version bump discards ALL stored
   settings and resets to defaults (settings_manager_boot §2). */
static esp_err_t on_migrate(uint32_t from_version, cJSON *settings)
{
    if (from_version < 5 && settings != NULL &&
        cJSON_GetObjectItemCaseSensitive(settings, "ble_ram_profile") == NULL)
    {
        const char *word = blm_ident_ram_migrated(
            blm_ident_adv_mode(get_str(settings, "advertising")),
            blm_ident_phy_mask(get_str(settings, "phy")));

        if (word != NULL)
        {
            cJSON_AddStringToObject(settings, "ble_ram_profile", word);
        }
    }

    return ESP_OK;
}

esp_err_t blm_settings_register(void)
{
    static const settings_descriptor_t DESC =
    {
        .name = "ble_manager",
        .version = 6, /* v6: +address, +security_request (4.x parity, fill-
                         missing defaults); v5: +ble_ram_profile and its
                         custom knobs (a stored BLE 5 choice migrates to
                         Full); v4: +phy, +advertising (BLE 5 as a choice,
                         4.2 defaults); v3: +sc_only; v2: +bonding */
        .fields = FIELDS,
        .field_count = sizeof(FIELDS) / sizeof(FIELDS[0]),
        .on_apply = on_apply,
        .on_validate = on_validate,
        .on_migrate = on_migrate,
    };

    return settings_manager_register(&DESC);
}
