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
 * @file vpn_manager_check.c
 * @brief Pure config validation (host-tested; no esp deps beyond
 *        esp_err.h). The settings on_validate speaks through
 *        vpn_check_config so the UI gets a reason, not just a 400.
 */
#include <stdio.h>
#include <string.h>

#include "ip4_routes.h"

#include "vpn_manager_private.h"

#define WG_KEY_B64_LEN 44 /* base64(32 bytes) incl. the trailing '=' */

static bool b64_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '+' || c == '/';
}

bool vpn_check_wg_key(const char *key_b64)
{
    if (key_b64 == NULL || strlen(key_b64) != WG_KEY_B64_LEN)
    {
        return false;
    }

    for (int i = 0; i < WG_KEY_B64_LEN - 1; i++)
    {
        if (!b64_char(key_b64[i]))
        {
            return false;
        }
    }

    return key_b64[WG_KEY_B64_LEN - 1] == '=';
}

bool vpn_check_endpoint(const char *host)
{
    if (host == NULL)
    {
        return false;
    }

    size_t len = strlen(host);

    if (len < 1 || len > 63)
    {
        return false;
    }

    for (size_t i = 0; i < len; i++)
    {
        char c = host[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '-' ||
                  c == ':' || c == '_';

        if (!ok)
        {
            return false;
        }
    }

    return true;
}

const char *vpn_check_config(const vpn_config_t *cfg)
{
    if (!cfg->enabled)
    {
        return NULL; /* disabled config is always storable */
    }

    if (cfg->tailscale)
    {
        size_t len = strlen(cfg->ts_auth_key);

        /* tskey-auth-... (Tailscale) or a headscale preauth key (hex);
         * shape-check only: the coordinator is the judge */
        if (len < 16)
        {
            return "ts_auth_key is required for tailscale (from your "
                   "coordinator's admin)";
        }

        if (cfg->ts_control_url[0] != '\0' &&
            !vpn_check_endpoint(cfg->ts_control_url))
        {
            return "ts_control_url must be a hostname or IP (no "
                   "scheme)";
        }

        return NULL; /* wireguard fields not required for tailscale */
    }

    if (!vpn_check_wg_key(cfg->private_key))
    {
        return "private_key must be a 44-char base64 WireGuard key "
               "(use the keygen)";
    }

    if (!vpn_check_wg_key(cfg->peer_public_key))
    {
        return "peer_public_key must be a 44-char base64 WireGuard key";
    }

    if (cfg->preshared_key[0] != '\0' &&
        !vpn_check_wg_key(cfg->preshared_key))
    {
        return "preshared_key must be empty or a 44-char base64 key";
    }

    if (!vpn_check_endpoint(cfg->endpoint))
    {
        return "endpoint must be a hostname or IP";
    }

    if (cfg->port < 1 || cfg->port > 65535)
    {
        return "port must be 1..65535";
    }

    if (cfg->address[0] == '\0')
    {
        return "address (local tunnel IP) is required";
    }

    return NULL;
}

bool vpn_check_routes(const char *routes, char *err, size_t err_len)
{
    return ip4_routes_parse_list(routes, NULL, VPN_MAX_ROUTES, err, err_len)
           >= 0;
}

const char *vpn_manager_state_str(vpn_state_t state)
{
    /* a switch, not a table: nothing to index past (issue #985) */
    switch (state)
    {
        case VPN_STATE_WAITING:    return "waiting";
        case VPN_STATE_PAUSED:     return "paused";
        case VPN_STATE_CONNECTING: return "connecting";
        case VPN_STATE_CONNECTED:  return "connected";
        case VPN_STATE_DISABLED:
        default:                   return "disabled";
    }
}

/* the tunnel address as a /32 entry, any "/n" suffix ignored (v1 stripped
 * it the same way before handing the address to the netif) */
static bool parse_bare_address(const char *address, ip4_routes_entry_t *out)
{
    char bare[32];

    if (address == NULL)
    {
        return false;
    }

    snprintf(bare, sizeof(bare), "%s", address);

    char *slash = strchr(bare, '/');

    if (slash != NULL)
    {
        *slash = '\0';
    }

    return ip4_routes_parse_cidr(bare, out) && out->mask == 0xFFFFFFFFu &&
           out->network != 0;
}

bool vpn_check_legacy_route(const char *address, const char *ip,
                            const char *mask, char *out, size_t len)
{
    ip4_routes_entry_t a;
    ip4_routes_entry_t m;

    if (ip == NULL || mask == NULL || out == NULL || len == 0)
    {
        return false;
    }

    /* a bare address parses as /32: `network` then holds the value */
    if (!ip4_routes_parse_cidr(ip, &a) || !ip4_routes_parse_cidr(mask, &m))
    {
        return false;
    }

    uint32_t mv = m.network;
    uint32_t inv = ~mv;

    if (a.network == 0 || mv == 0 || (inv & (inv + 1)) != 0)
    {
        return false; /* the 0.0.0.0 idiom, or a mask with holes */
    }

    /* v1 routed the netif's own network: the tunnel ADDRESS under the
       mask (issue #988). The AllowedIPs address only ever contributed
       the mask; it is the fallback when the stored address is unusable. */
    ip4_routes_entry_t host;
    uint32_t           base = parse_bare_address(address, &host) ? host.network
                                                                : a.network;
    ip4_routes_entry_t e = { .network = base & mv, .mask = mv };

    ip4_routes_format(&e, out, len);
    return true;
}

/* ---- the Tailscale hold reason (issue 1024, 2026-10-31) ------------------- */

static void append(char *out, size_t len, size_t *pos, const char *text)
{
    if (*pos >= len)
    {
        return;
    }

    int n = snprintf(out + *pos, len - *pos, "%s", text);

    if (n < 0)
    {
        return;
    }

    *pos = (*pos + (size_t)n < len) ? *pos + (size_t)n : len - 1;
}

const char *vpn_ts_hold_reason(unsigned free_kb, unsigned need_kb, bool ble_on,
                               bool wifi_full, char *out, size_t len)
{
    if (out == NULL || len == 0)
    {
        return "";
    }

    char head[80];
    size_t pos = 0;
    const char *sep = ": ";

    snprintf(head, sizeof(head),
             "not started: %u KB of internal RAM free, Tailscale needs %u KB",
             free_kb, need_kb);
    out[0] = '\0';
    append(out, len, &pos, head);

    if (ble_on)
    {
        append(out, len, &pos, sep);
        /* measured 2026-10-31 on the build with the controller's code in
           flash: Bluetooth costs 31.5 KB on Lean and 34.6 KB on Full */
        append(out, len, &pos, "turn Bluetooth off (about 32 KB)");
        sep = ", ";
    }

    if (wifi_full)
    {
        append(out, len, &pos, sep);
        append(out, len, &pos, "set the WiFi memory profile to Lean (about 14 KB)");
        sep = ", ";
    }

    append(out, len, &pos, sep);
    append(out, len, &pos, (ble_on || wifi_full) ? "or use WireGuard" : "use WireGuard");
    return out;
}
