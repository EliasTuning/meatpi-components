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
 * @file vpn_manager.h
 * @brief WiCAN VPN client (feature component): WireGuard over the
 *        vendored `esp_wireguard` port (rewrite of legacy vpn_manager,
 *        TASK_vpn_manager.md).
 *
 * Model: settings describe ONE WireGuard peer (reboot-to-apply like
 * everything). start() arms a small state task gated on
 * DEV_STATUS_NETWORK_CONNECTED_MASK *and* a valid clock (WireGuard
 * handshakes carry timestamps: rtc_manager restores time at boot; a
 * fresh board waits and retries): it connects when the uplink is up,
 * polls peer liveness, tears down on network loss and reconnects with
 * backoff. Connected state = DEV_STATUS_BIT_VPN_ENABLED + the
 * `vpn.state {connected}` event.
 *
 * Routing: the networks listed in `routes` (CIDRs, settings v2, issue
 * #975: handed to ip4_routes' table once the tunnel is up, so lwIP's
 * route lookup sends only those destinations into the netif) unless
 * `default_route` is set (default OFF, meatpi 2026-07-07: then the
 * netif is the default one). Optional DNS override (saved/restored
 * around the connection, the legacy behavior).
 *
 * Keys: `POST /api/vpn/keygen` generates a fresh Curve25519 pair ON
 * the device and stores the private key straight into pending
 * settings, the response carries ONLY the public key (register it at
 * the server). Settings GETs redact `private_key`/`preshared_key`
 * (api_http secret suffixes); "" on PUT keeps the stored value.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    VPN_STATE_DISABLED = 0,  /**< disabled in settings                */
    VPN_STATE_WAITING,       /**< enabled; waiting for network/time   */
    VPN_STATE_PAUSED,        /**< the station is on a network marked
                                  "Pause VPN" (issue #976): the tunnel
                                  stays down until it leaves           */
    VPN_STATE_CONNECTING,    /**< tunnel up, no verified handshake yet */
    VPN_STATE_CONNECTED,     /**< peer handshake verified             */
} vpn_state_t;

typedef struct
{
    vpn_state_t state;
    bool        tailscale;        /**< type: tailscale vs wireguard     */
    bool        paused;           /**< state == VPN_STATE_PAUSED        */
    char        network[33];      /**< the station's network ("" none)  */
    char        endpoint[64];     /**< peer host:port / control server  */
    char        ts_ip[16];        /**< tailnet 100.x address ("" none)  */
    int         ts_peers;         /**< tailnet peers visible            */
    bool        ts_derp;          /**< the DERP relay connection is up  */
    uint16_t    ts_derp_region;   /**< the relay region the home connection
                                       uses (2026-11-03; 0 for wireguard)  */
    char        reason[176];      /**< why the tunnel is held in waiting
                                       ("" none): the internal-RAM floor */
    uint32_t    connects;         /**< successful peer-up transitions   */
    uint32_t    failures;         /**< connect attempts that never came up */
    uint32_t    uptime_s;         /**< seconds since the last peer-up   */
} vpn_manager_status_t;

/** Register descriptors (settings/log/events). No network access. */
esp_err_t vpn_manager_init(void);

/** Create the state task when enabled (network-gated internally). */
esp_err_t vpn_manager_start(void);

/** Tear the tunnel down; the state task parks. */
esp_err_t vpn_manager_stop(void);

esp_err_t vpn_manager_status(vpn_manager_status_t *out);

/** The wire word of a state: "disabled", "waiting", "paused",
 *  "connecting" or "connected", what `GET /api/vpn` `state` and the HA
 *  push `vpn_status` carry. ONE table for every surface (issue #985: the
 *  HA push kept a 4-entry copy that PAUSED outgrew). Never NULL; an
 *  unknown value reads "disabled". Pure (vpn_manager_check.c). */
const char *vpn_manager_state_str(vpn_state_t state);

/** The device's OWN tunnel address while CONNECTED: WireGuard: the
 *  configured `address` (any /cidr stripped); Tailscale: the live
 *  tailnet 100.x IP. Consumers: the HA webhook push (`vpn_ip`, HA
 *  stores it as the away-from-home control endpoint). "" +
 *  ESP_ERR_INVALID_STATE when not connected. */
esp_err_t vpn_manager_tunnel_ip(char *buf, size_t len);

/** Generate a device-side WireGuard keypair. The private key is
 *  written into this component's pending settings (reboot applies);
 *  only the base64 PUBLIC key is returned. */
esp_err_t vpn_manager_keygen(char *public_key_b64, size_t len);

/** Optional /api/vpn routes (§9.1; main wires in HTTP compositions). */
esp_err_t vpn_manager_register_http(void);

/** `vpn` CLI command; registered internally on the settings apply. */
esp_err_t vpn_manager_register_cli(void);

#ifdef __cplusplus
}
#endif
