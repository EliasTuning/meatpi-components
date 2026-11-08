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
 * @file wifi_manager.h
 * @brief WiCAN WiFi manager (feature component).
 *
 * Owns the WiFi radio: STA with prioritized fallback networks and an
 * auth-failure ban list, AP with static IP + DHCP server, APSTA with
 * AP-auto-disable and AP-channel-follows-STA, and a supervised reconnect
 * task.
 *
 * Configuration is a settings_manager descriptor (name "wifi_manager") and
 * applies ONLY at boot (Coding Standard §4.2 reboot-to-apply): there is no
 * runtime set-mode / set-credentials API. Changing WiFi settings = PUT via
 * the settings transport, then reboot. The runtime surface below is status,
 * scanning, and event callbacks only.
 *
 * Lifecycle (composition in main):
 *   wifi_manager_init():       allocate state, register the settings
 *                              descriptor. Before settings_manager_start().
 *   settings_manager_start(): runs on_apply (stores config; touches no HW)
 *   wifi_manager_start():      netifs + esp_wifi up per config; refuses with
 *                              ESP_ERR_INVALID_STATE if unconfigured (§4.3).
 *   wifi_manager_stop():       stops the radio and the reconnect task.
 *
 * Requires nvs_flash_init() before start (esp_wifi dependency); main owns
 * that, per §3 ("a component assumes its dependencies are already init'd").
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Event-group bits for external status monitoring (wifi_manager_get_event_group). */
#define WIFI_MANAGER_BIT_STA_CONNECTED   BIT0  /* associated + got IP        */
#define WIFI_MANAGER_BIT_STA_DISCONNECTED BIT1
#define WIFI_MANAGER_BIT_AP_STARTED      BIT2
#define WIFI_MANAGER_BIT_ENABLED         BIT3  /* radio started              */
#define WIFI_MANAGER_BIT_STA_AP_OVERLAP  BIT4  /* STA and AP subnets overlap */

/** Event callbacks. All run in the default event loop task: keep them short,
 *  never block, never call back into wifi_manager stop/start from them. */
typedef struct
{
    void (*sta_connected)(void);                              /* got IP      */
    void (*sta_disconnected)(void);
    void (*ap_station_connected)(const uint8_t *mac, uint8_t aid);
    void (*ap_station_disconnected)(const uint8_t *mac, uint8_t aid);
} wifi_manager_callbacks_t;

/** Allocate state and register the settings descriptor ("wifi_manager"). */
esp_err_t wifi_manager_init(void);

/** Bring the radio up per the boot-applied settings. ESP_ERR_INVALID_STATE
 *  if the component was left unconfigured by the settings boot pass, ESP_OK
 *  (radio off) when the configured mode is "off". */
esp_err_t wifi_manager_start(void);

/** Stop the reconnect task and the radio. */
esp_err_t wifi_manager_stop(void);

/** Button config mode (2026-07-19): force the AP up with the
 *  boot-applied AP config regardless of the running mode (STA-only
 *  included; an existing STA link stays up). The ONE sanctioned runtime
 *  mode change: there is no reverse; config mode ends with a reboot
 *  back into the configured mode. No-op if the AP is already serving. */
esp_err_t wifi_manager_config_ap(void);

/* ---- status ------------------------------------------------------------- */

bool wifi_manager_is_enabled(void);
bool wifi_manager_is_sta_connected(void);
bool wifi_manager_is_ap_started(void);

/** Copy the current STA IPv4 address ("" when not connected). */
esp_err_t wifi_manager_get_sta_ip(char *buf, size_t buf_len);

/** Copy the name of the network the station is connected to ("" and
 *  ESP_ERR_INVALID_STATE when it is not connected). */
esp_err_t wifi_manager_get_sta_ssid(char *buf, size_t buf_len);

/** "Pause VPN" (issue #976, 2026-10-11): true while the station is
 *  connected to a network whose `sta_vpn_pause` / `fallbackN_vpn_pause`
 *  flag is set. vpn_manager polls it on its one-second lap and keeps the
 *  tunnel down meanwhile; false without a station session. */
bool wifi_manager_sta_vpn_pause(void);

/** "Active on these networks" (2026-11-08: the data destinations and the
 *  Home Assistant webhook): @p list = network names separated by commas
 *  as the sender's `networks` setting stores them. An empty or NULL list
 *  means no restriction: true. Otherwise true only while the station is
 *  connected to one of the named networks (compared whole, blanks around
 *  the commas ignored); false without a station session, so a device on
 *  the ESPNetLink or USB Ethernet alone does not count as "on" a WiFi
 *  network. Cheap: a status flag and one string compare per name. */
bool wifi_manager_sta_on_network(const char *list);

/** Number of stations currently associated to our AP. */
uint16_t wifi_manager_get_ap_station_count(void);

/** The station's last connect attempt, for the status API / UI (2026-09-06):
 *  which entry, why it dropped, and whether it is now tried only after
 *  the other networks on the list. */
typedef struct
{
    char    ssid[33];      /**< network of the last attempt ("" = none yet) */
    uint8_t last_reason;   /**< WIFI_REASON_* of the last disconnect, 0 = none */
    uint8_t fail_count;    /**< recent consecutive authentication failures  */
    bool    deprioritised; /**< true = other networks are tried first        */
} wifi_manager_sta_attempt_t;

void wifi_manager_get_sta_attempt(wifi_manager_sta_attempt_t *out);

/** The connection TRIAL (2026-10-06, Quick Setup: "test it before storing
 *  and rebooting"): join @p ssid with @p password without saving either,
 *  wait for the association, the handshake and an address (20 s in all),
 *  report, let go. Needs a station interface (mode apsta or sta, not
 *  suspended). The reconnect task stands back meanwhile and re-joins the
 *  configured network afterwards; a station that was connected drops for
 *  the trial. A runtime knob, nothing is written. @p channel (1..13) is
 *  the network's channel from the scan, scanned first and alone so the
 *  access point's clients see no long silence (2026-10-08); 0 = all.
 *  @return ESP_OK started (poll wifi_manager_sta_try_status),
 *          ESP_ERR_INVALID_ARG ssid 1..32 / password 8..63 or empty,
 *          ESP_ERR_INVALID_STATE a trial is already running,
 *          ESP_ERR_NOT_SUPPORTED no station interface to try with. */
esp_err_t wifi_manager_sta_try(const char *ssid, const char *password,
                               uint8_t channel);

typedef struct
{
    char     state[8];   /**< idle | running | done                          */
    char     result[10]; /**< none | connected | password | not_found |
                              refused | no_ip | timeout                       */
    char     ssid[33];
    char     ip[16];     /**< the address received (connected)              */
    uint8_t  reason;     /**< the WIFI_REASON_* that ended it, 0 = none      */
    int8_t   rssi;       /**< dBm at the moment of the address (connected)   */
    uint8_t  channel;    /**< the network's channel (from the association)   */
    uint32_t took_ms;    /**< from our connect attempt to the result         */
    uint32_t age_s;      /**< seconds since the result (done)                */
} wifi_manager_try_t;

void wifi_manager_sta_try_status(wifi_manager_try_t *out);

/** Copy the AP's current gateway IP (dotted quad) into @p buf.
 *  ESP_ERR_INVALID_STATE when the AP interface has no address. */
esp_err_t wifi_manager_get_ap_ip(char *buf, size_t buf_len);

/** IPv4 DNS servers as strings; "N/A" written when unavailable. */
esp_err_t wifi_manager_get_sta_dns(char *dns_main, size_t dns_main_len,
                                   char *dns_backup, size_t dns_backup_len);

/** Event group carrying the WIFI_MANAGER_BIT_* bits, for waiters. */
EventGroupHandle_t wifi_manager_get_event_group(void);

/* ---- runtime operations --------------------------------------------------- */

/**
 * Blocking scan; returns a malloc'd JSON string
 * {"networks":[{"ssid","rssi","channel","auth_mode","bssid"},...]} the
 * caller frees, or NULL on error. Temporarily switches AP-only mode to
 * APSTA for the scan and restores it after.
 */
char *wifi_manager_scan_networks(void);

/** Register (or clear, with NULL) event callbacks. */
esp_err_t wifi_manager_set_callbacks(const wifi_manager_callbacks_t *cbs);

/* ---- runtime interface suspension (interface_manager's actuators) ----------
 * EPHEMERAL: settings untouched, a reboot restores the configured mode.
 * Suspending STA also parks the reconnect task; resuming reconnects.
 * ESP_ERR_INVALID_STATE when not started or the configured mode doesn't
 * include the interface. */
esp_err_t wifi_manager_suspend_sta(void);
esp_err_t wifi_manager_resume_sta(void);
esp_err_t wifi_manager_suspend_ap(void);
esp_err_t wifi_manager_resume_ap(void);

/**
 * Drop the current STA association NOW; the normal selection path
 * reconnects (same candidate rules, no settings touched). For a consumer
 * that KNOWS the association is dead while the driver still reports it
 * connected: an AP that rebooted and came back inside the beacon-loss
 * window forgets its clients, so every socket times out on a "connected"
 * STA (espnetlink_link, bench 2026-08-24: the dongle's AP after a VBUS
 * cycle). ESP_ERR_INVALID_STATE when STA is not connected.
 */
esp_err_t wifi_manager_sta_reconnect(void);

/** Whether the BOOT-CONFIGURED mode includes STA / AP (regardless of any
 *  runtime suspension): policy inputs for interface_manager. */
bool wifi_manager_mode_has_sta(void);

/** True while the APPLIED AP password is still the factory default. The
 *  settings on_validate gate (2026-09-07) refuses any save that keeps it,
 *  so consumers that write wifi_manager themselves (espnetlink_link's
 *  zero-touch pairing store) hold off and surface a warning instead of
 *  failing opaquely (field-hit 2026-09-07: a fresh device reported
 *  "Not an ESPNetLink / gave up"). Changes only via reboot-to-apply. */
bool wifi_manager_ap_password_is_factory(void);
bool wifi_manager_mode_has_ap(void);

/**
 * Register the component's /api/wifi/status + /api/wifi/scan routes with
 * http_server_manager (HTTP_API.md §7). Called by main ONLY in compositions
 * that include the HTTP server, after http_server_manager_init(); the rest
 * of the component never touches HTTP.
 */
/**
 * Network-trust admission gate for http_server_manager (wired by main):
 * false when the current STA network is marked untrusted AND the request
 * arrived via the STA address. Own-AP/USB requests always pass; outbound
 * clients are unaffected (meatpi 2026-07-08, shared networks must not
 * expose configuration).
 */
bool wifi_manager_http_request_allowed(int sockfd);

esp_err_t wifi_manager_register_http(void);

/** Register the `wifi` CLI command with cmdline_manager. Called
 *  INTERNALLY on the settings boot apply when the `cli` setting is true
 *  (default): main no longer wires it. */
esp_err_t wifi_manager_register_cli(void);

#ifdef __cplusplus
}
#endif
