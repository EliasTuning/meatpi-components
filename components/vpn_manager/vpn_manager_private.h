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
 * @file vpn_manager_private.h
 * @brief Internals shared between the state machine, the esp_wireguard
 *        glue, the pure config checks, and the HTTP/CLI/events glue.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#include "vpn_manager.h"

/* boot-applied configuration (strings sized to the settings schema) */
typedef struct
{
    bool enabled;
    bool tailscale;            /* type == "tailscale"                */
    char private_key[64];
    char peer_public_key[64];
    char preshared_key[64];
    char address[32];          /* local tunnel IP, optional /cidr    */
    char routes[256];          /* "a.b.c.d/n, ..." through the tunnel
                                  (v2, issue #975); "" = nothing but
                                  default_route                      */
    char endpoint[64];
    int  port;
    int  keepalive_s;
    bool default_route;
    char dns[16];              /* optional override, "" = off        */
    /* tailscale (microlink) */
    char ts_auth_key[96];      /* SECRET (auth_key redaction suffix) */
    char ts_device_name[48];   /* "" = wican_<device_id>             */
    char ts_control_url[64];   /* "" = tailscale.com; host only      */
} vpn_config_t;

/* ---- vpn_manager_check.c: PURE (host-tested) ---------------------------- */

/* a WireGuard key is 32 bytes base64: exactly 44 chars, valid alphabet,
 * '=' terminated */
bool vpn_check_wg_key(const char *key_b64);
/* endpoint hostname/IP sanity (len 1..63, [A-Za-z0-9.-:_]) */
bool vpn_check_endpoint(const char *host);
/* full enabled-config validation; returns NULL when OK, else a static
 * human-readable reason (the settings on_validate message) */
const char *vpn_check_config(const vpn_config_t *cfg);

/* the routes list (issue #975, 2026-10-11): at most VPN_MAX_ROUTES networks
 * in CIDR form; false with a readable reason in err (the offending token) */
#define VPN_MAX_ROUTES 8
bool vpn_check_routes(const char *routes, char *err, size_t err_len);

/* The internal-RAM floor Tailscale needs at the moment it starts (issue
 * 1024, 2026-10-31): the rule is the bring-up's lowest-point cost plus a
 * 10 KB working margin (WiFi bursts, the SD card's DMA, TLS), rounded up.
 * Re-derived 2026-11-01 with the state task and the client's WireGuard
 * manager on PSRAM stacks (their NVS work on the flash worker): the start
 * still dips 19 KB below the pre-start level (the connect's 9 KB burst,
 * the client's 4 KB of queues and keys, the flash worker's 4.4 KB that the
 * first key read creates for 5 s), while the steady cost is about 10 KB
 * (it was 22 KB with the stacks internal; 25.7 KB at the dip). 19 + 10,
 * rounded: the floor stays at 30. */
#define VPN_TS_INTERNAL_FLOOR_KB 30u

/* Pure, host-tested: the hold reason shown on /api/vpn and the console
 * while Tailscale waits for internal RAM: what the device can change,
 * from its own settings (Bluetooth on, the WiFi memory profile not Lean),
 * else WireGuard. Returns @p out. The Bluetooth memory profile of
 * ble_manager v5 is not named here on purpose: its Lean frees about 3 KB
 * on the bench unit (2026-10-31), no help against a 30 KB floor. */
const char *vpn_ts_hold_reason(unsigned free_kb, unsigned need_kb, bool ble_on,
                               bool wifi_full, char *out, size_t len);

/* settings v1 -> v2: the network v1 actually routed, as ONE route
 * ("a.b.c.d/n"): the tunnel `address` under allowed_ip_mask (v1 gave the
 * netif the address with that netmask, so lwIP routed address & mask; the
 * AllowedIPs address itself only contributed the mask, issue #988). Without
 * a usable address the pair's own network is taken. False for the 0.0.0.0
 * idiom, a non-contiguous mask or a malformed pair */
bool vpn_check_legacy_route(const char *address, const char *ip,
                            const char *mask, char *out, size_t len);

/* ---- vpn_manager_wg.c: esp_wireguard glue (state task ONLY) ------------- */

esp_err_t vpn_wg_up(const vpn_config_t *cfg);   /* init + connect      */
void      vpn_wg_down(void);                    /* disconnect + reset  */
bool      vpn_wg_peer_up(void);
esp_err_t vpn_wg_set_default_route(void);

/* ---- vpn_manager_ts.c: tailscale/microlink glue (state task ONLY) ------- */

esp_err_t vpn_ts_up(const vpn_config_t *cfg);   /* init + start        */
void      vpn_ts_down(void);                    /* stop + destroy      */
bool      vpn_ts_connected(void);
bool      vpn_ts_failed(void);
void      vpn_ts_status(char *ip, size_t ip_len, int *peers, bool *derp);

/* Per-peer snapshot for /api/vpn + the `vpn` CLI ("peer online +
 * direct-vs-relay"). Returns the number of entries written (0 when the
 * tunnel is down or type != tailscale). */
typedef struct
{
    char hostname[64];
    char ip[16];
    bool online;
    bool direct_path; /* true = direct UDP path, false = DERP relay */
} vpn_ts_peer_t;

int vpn_ts_get_peers(vpn_ts_peer_t *out, int max);

/* The subnet routes learned from the tailnet (2026-11-01): a network a
 * peer advertises as a subnet router and the control server approved,
 * installed into the tunnel (`/api/vpn` `routes` for the tailscale type,
 * the `vpn` command). Returns the number of entries written (0 when the
 * tunnel is down or type != tailscale). */
#define VPN_TS_MAX_ROUTES 8
typedef struct
{
    char cidr[24];   /* "a.b.c.d/n" */
    char via[16];    /* the subnet router's tailnet address */
} vpn_ts_route_t;

int vpn_ts_get_routes(vpn_ts_route_t *out, int max);

/* ---- settings (vpn_manager_settings.c) ------------------------------------ */

/** Register the "vpn_manager" descriptor with settings_manager. */
esp_err_t vpn_settings_register(void);

/** Boot-applied config; valid once vpn_settings_is_configured(). */
const vpn_config_t *vpn_settings_config(void);
bool vpn_settings_is_configured(void); /* boot apply ran (standard §4.3) */

/** Display endpoint ("host:port" or the control server), derived on
 *  the settings apply for status/log strings. */
const char *vpn_settings_endpoint(void);

/* ---- glue ----------------------------------------------------------------- */

void vpn_events_register(void);
/** Publish `vpn.state {connected, paused, network}` (issue #976: paused
 *  = the station is on a network marked "Pause VPN"). */
void vpn_events_state(bool connected, bool paused, const char *network);
