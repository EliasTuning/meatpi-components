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
 * @file wifi_manager_private.h
 * @brief Internals shared between wifi_manager.c, wifi_manager_settings.c and
 *        the pure candidate-selection module (wifi_manager_select.c — keep
 *        that one free of IDF deps so the host tests compile it directly).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WM_SSID_LEN       33 /* 32 chars + NUL */
#define WM_PASS_LEN       65 /* 64 chars + NUL */
#define WM_HOSTNAME_LEN   33
#define WM_MAX_FALLBACKS  5
#define WM_MAX_CANDIDATES (1 + WM_MAX_FALLBACKS)

/* Failure memory (meatpi 2026-09-06, replaces the timed ban list): an
 * entry whose last WM_AUTH_FAIL_THRESHOLD connection attempts failed —
 * for ANY reason except "network not found" (a wrong password, an AP
 * that never finishes the handshake, a full AP, a refused association)
 * — is only DEPRIORITISED: every other visible network on the list is
 * tried first, and when it is the only network around it keeps being
 * tried at the normal reconnect cadence, because it is our best chance.
 * Nothing is ever blocked for a period (the old 10-minute ban parked a
 * device for minutes after three AP hiccups). The memory is per config
 * ENTRY, not per SSID name: two entries may carry the same SSID with
 * different passwords (home vs elsewhere) and a failure with one
 * password must not stop the other from being tried. It fades after
 * WM_FAIL_MEMORY_MS without a new failure, so an entry regains its
 * place in the priority order (roam-to-preferred may try it again, and
 * a failed roam trial costs one strike, not three). */
#define WM_AUTH_FAIL_THRESHOLD 3
#define WM_FAIL_MEMORY_MS      (2u * 60u * 1000u) /* 2 minutes */

typedef enum
{
    WM_MODE_OFF = 0,
    WM_MODE_STA,
    WM_MODE_AP,
    WM_MODE_APSTA,
} wm_mode_t;

typedef enum
{
    WM_PS_NONE = 0,
    WM_PS_MIN,
    WM_PS_MAX,
} wm_ps_t;

/* WiFi memory profile (v5): trades internal RAM for WiFi throughput by
 * sizing the driver buffers at esp_wifi_init. */
typedef enum
{
    WM_RAM_FULL = 0, /* IDF defaults — max throughput          */
    WM_RAM_LEAN,     /* smaller buffers, ~15 KB internal freed */
    WM_RAM_CUSTOM,   /* the wifi_static_rx/tx + cache_tx knobs */
} wm_ram_profile_t;

/* AP security selection (v4). AUTO keeps the historic behaviour: WPA2-PSK
 * when a password is set, OPEN when it is blank. */
typedef enum
{
    WM_AP_AUTH_AUTO = 0,
    WM_AP_AUTH_OPEN,
    WM_AP_AUTH_WPA2,
    WM_AP_AUTH_WPA2WPA3,
    WM_AP_AUTH_WPA3,
} wm_ap_auth_t;

typedef struct
{
    char ssid[WM_SSID_LEN];
    char password[WM_PASS_LEN];
} wm_network_t;

/* v6: one network's STA addressing (host-order; is_static=false = DHCP) */
typedef struct
{
    bool     is_static;
    uint32_t ip;      /* valid when is_static             */
    uint32_t netmask;
    uint32_t gw;      /* 0 = none (isolated LAN is legal) */
} wm_addr_t;

/** Boot-applied configuration (from the settings descriptor). */
typedef struct
{
    wm_mode_t    mode;
    wm_network_t sta[WM_MAX_CANDIDATES]; /* [0] primary, then fallbacks     */
    size_t       sta_count;              /* 0 => no STA network configured  */
    char         hostname[WM_HOSTNAME_LEN];
    bool         sta_auto_reconnect;
    int32_t      sta_max_retry;          /* -1 => infinite                  */
    wm_network_t ap;
    uint8_t      ap_channel;
    uint8_t      ap_max_connections;
    bool         ap_auto_disable;        /* drop AP once STA has an IP      */
    uint32_t     ap_ip;                  /* v4: AP gateway IP (host order);
                                            /24 assumed. 0 => legacy default
                                            192.168.0.10                    */
    bool         ap_hidden;              /* v4: don't beacon the SSID       */
    bool         ap_ht40;                /* v4: 40 MHz bandwidth (else 20)  */
    wm_ap_auth_t ap_auth;                /* v4: security mode (see enum)    */
    wm_ps_t      power_save;
    bool         trusted[WM_MAX_CANDIDATES]; /* v3: untrusted nets
                                            refuse inbound admin
                                            arriving via STA        */
    uint32_t     sta_roam_interval_s;    /* while connected to a FALLBACK,
                                            re-check for a higher-priority
                                            network this often; 0 = never
                                            (v2, TASK: home > car hotspot) */
    /* v5: WiFi memory profile — buffer counts fed to esp_wifi_init.
       0 in a count = "use the profile/IDF default". */
    wm_ram_profile_t ram_profile;
    uint16_t     wifi_static_rx;         /* custom: static RX buffers  */
    uint16_t     wifi_static_tx;         /* custom: static TX buffers  */
    uint16_t     wifi_cache_tx;          /* custom: cache TX buffers   */
    /* v6: PER-NETWORK STA addressing — static (user-fixed) or DHCP
       (default). Each candidate lives on its own LAN, so its addressing
       travels with it through the empty-slot collapse (like trusted[]).
       Applied per CONNECT ATTEMPT (apply_sta_network). Host-order.
       sta_dns = GLOBAL custom DNS override for BOTH modes (0 =
       automatic: DHCP-provided, or that network's gateway when static). */
    wm_addr_t    sta_addr[WM_MAX_CANDIDATES];
    uint32_t     sta_dns;
} wm_config_t;

/** Parse a dotted-quad IPv4 into host-order uint32 (any address). */
bool wm_parse_ipv4(const char *s, uint32_t *out);

/** wm_parse_ipv4 + reject a host octet of 0/255 (network/broadcast) —
 *  for the device's own / gateway addresses. Shared with the settings
 *  validator (wifi_manager_settings.c) and the host tests. */
bool wm_parse_ap_ipv4(const char *s, uint32_t *out);

/** True when @p mask (host order) is a contiguous netmask (/1../31). */
bool wm_netmask_valid(uint32_t mask);

/** The DHCP/DNS host name to give the STA netif: @p configured (the
 *  `hostname` setting) when non-empty, else `wican_<device_id>` - the
 *  same name mdns_manager advertises as `<host>.local`, so a router's
 *  client list and the .local name agree (lwIP's own default is
 *  "espressif", 2026-09-08). false = nothing to set (no id, or the
 *  result would not fit @p cap); @p out is "" then. Pure, host-tested. */
bool wm_default_hostname(const char *configured, const char *device_id,
                         char *out, size_t cap);

/* ---- pure candidate selection + failure memory (wifi_manager_select.c) --- */

typedef struct
{
    uint8_t  fail_count;   /* consecutive failed connection attempts     */
    uint32_t last_fail_ms; /* tick of the newest one (memory fade)       */
} wm_fail_entry_t;

typedef struct
{
    wm_fail_entry_t fails[WM_MAX_CANDIDATES]; /* per candidate INDEX     */
    int             seq_cursor; /* rotation state, no-scan path          */
    int             dep_cursor; /* rotation among all-failed picks       */
} wm_select_state_t;

/** Reset selection state; makes the first sequential pick the primary. */
void wm_select_init(wm_select_state_t *st);

/**
 * Pick a candidate given scan results: the highest-priority VISIBLE entry
 * with a clean record (fewer than WM_AUTH_FAIL_THRESHOLD recent failed
 * attempts). When every visible entry has failed, one of them is still
 * returned — round-robin, so two entries sharing an SSID (different
 * passwords) alternate — because an entry that fails authentication is
 * still our best chance when nothing else is there. @p present is an
 * array of SSIDs seen in the scan. Returns -1 only when nothing
 * configured is visible.
 */
int wm_select_from_scan(wm_select_state_t *st, const wm_network_t *cand,
                        size_t cand_count,
                        const char (*present)[WM_SSID_LEN],
                        size_t present_count, uint32_t now_ms);

/**
 * No-scan/blind path (single network, scan failed, or nothing visible
 * matched — hidden SSIDs and scan-truncation land here): rotate through
 * the candidates, skipping ones that failed lately; when all have, rotate
 * through all of them. Returns -1 only for an empty list.
 */
int wm_select_sequential(wm_select_state_t *st, const wm_network_t *cand,
                         size_t cand_count, uint32_t now_ms);

/**
 * Roam-to-preferred check (call while CONNECTED to candidate
 * @p current_idx > 0): returns the highest-priority candidate index
 * < current_idx that is visible in @p present, has a clean record and
 * carries a DIFFERENT SSID from the working connection (the same name
 * is the same AP with another password on file — nothing to gain), or
 * -1 (stay). The "home > car hotspot" migration decision.
 */
int wm_select_better(wm_select_state_t *st, const wm_network_t *cand,
                     size_t cand_count, int current_idx,
                     const char (*present)[WM_SSID_LEN],
                     size_t present_count, uint32_t now_ms);

/** Record a failed connection attempt on candidate @p idx (a faded
 *  memory starts counting again from one). */
void wm_select_on_attempt_fail(wm_select_state_t *st, int idx,
                               uint32_t now_ms);

/** Deprioritise @p idx at once (a failed roam trial: we LEFT a working
 *  network to try it, one strike is proof enough). */
void wm_select_deprioritise(wm_select_state_t *st, int idx,
                            uint32_t now_ms);

/** Successful connect on @p idx: forget its failures. */
void wm_select_on_success(wm_select_state_t *st, int idx);

/** Recent consecutive failed attempts on @p idx (0 once the memory faded). */
uint8_t wm_select_fail_count(const wm_select_state_t *st, int idx,
                             uint32_t now_ms);

/** True while @p idx is tried only after every other visible network. */
bool wm_select_is_deprioritised(const wm_select_state_t *st, int idx,
                                uint32_t now_ms);

/* Escalating reconnect backoff (2026-07-10): how many 5 s reconnect
 * loops to SKIP after real attempt number @p retry_count failed.
 * 0 for the first two attempts (fast candidate walk / short outages),
 * then 1/3/5 (10/20/30 s cadence — cap per meatpi) so a parked device
 * doesn't scan every 5 s forever. Resets with sta_retry_count on
 * got-ip. */
#define WM_BACKOFF_MAX_SKIP_LOOPS 5 /* x 5 s = 30 s cadence cap */
int wm_backoff_skip_loops(int32_t retry_count);

/* AP-client pause policy (2026-08-31): while a client sits on our AP a
 * STA (re)connect may hop the radio's channel and knock it off, so the
 * reconnect loop defers. Two escapes, or the device never gets its
 * uplink: the FIRST association of a boot always goes ahead (a user who
 * is on our AP precisely to configure the device — the ESPNetLink
 * pairing story — would otherwise wait forever: field-hit on a fresh
 * WiCAN Pro), and the pause is BOUNDED (max_pauses x the pause period)
 * so an outage cannot last as long as a client stays parked on the AP.
 * @return true = defer this loop. */
#define WM_AP_CLIENT_MAX_PAUSES 6 /* x 10 s = 60 s, then one attempt */
bool wm_sta_pause_for_ap_clients(uint16_t ap_clients, bool ever_connected,
                                 uint32_t pauses_so_far,
                                 uint32_t max_pauses);

/* ---- runtime interface suspension (wifi_manager_suspend.c) -----------------
 * EPHEMERAL per-interface suspension driven by interface_manager's policy
 * (BLE/WiFi arbitration). Settings untouched; a reboot resets. */

/** True while the policy holds STA / AP suspended (gates the reconnect
 *  task and the AP-restore paths in wifi_manager.c). */
bool wm_suspend_sta_active(void);
bool wm_suspend_ap_active(void);

/** wifi_manager.c internals the suspend layer needs. */
bool wm_core_started(void);

/* ---- settings glue (wifi_manager_settings.c) ------------------------------ */

/** Register the "wifi_manager" descriptor with settings_manager. */
esp_err_t wm_settings_register(void);

/** Boot-applied config; valid once wm_settings_is_configured(). */
/* The factory access point password. It has to work on a fresh device (the
 * first connection uses it) but it is public knowledge: after the boot pass
 * any settings write that would leave it in place is refused (on_validate),
 * /api/wifi/status reports `ap_default_password`, and the web UI stops the
 * Submit until a new one is typed (meatpi 2026-09-07). */
#define WM_AP_PASSWORD_DEFAULT "@meatpi#"

const wm_config_t *wm_settings_config(void);

/* wifi_manager_events.c: the wifi.sta event + ${wifi.ssid} value */
void wm_events_register(void);
void wm_events_sta(bool connected, const char *ssid);
bool wm_settings_is_configured(void);

#ifdef __cplusplus
}
#endif
