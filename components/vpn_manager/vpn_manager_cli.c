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
 * @file vpn_manager_cli.c
 * @brief The `vpn` CLI command: registered by
 *        vpn_manager_register_cli() on the settings apply (§6b).
 */
#include "esp_attr.h"
#include "cmdline_manager.h"

#include "vpn_manager.h"
#include "vpn_manager_private.h"

/* the public words (one table, issue #985); the console spells the wait out */
static const char *state_str(vpn_state_t st)
{
    return st == VPN_STATE_WAITING ? "waiting for network/time"
                                   : vpn_manager_state_str(st);
}

static int cmd_vpn(int argc, char **argv)
{
    vpn_manager_status_t st;

    (void)argc;
    (void)argv;
    (void)vpn_manager_status(&st);

    if (st.state == VPN_STATE_PAUSED)
    {
        /* issue #976: the station is on a network marked Pause VPN */
        cmdline_printf("vpn: %s, paused on %s (marked Pause VPN; %s)\n",
                       st.tailscale ? "tailscale" : "wireguard",
                       st.network[0] ? st.network : "the station's network",
                       st.endpoint[0] ? st.endpoint : "no endpoint");
    }
    else
    {
        cmdline_printf("vpn: %s, %s (%s)%s%s\n",
                       st.tailscale ? "tailscale" : "wireguard",
                       state_str(st.state),
                       st.endpoint[0] ? st.endpoint : "no endpoint",
                       st.network[0] ? ", station on " : "",
                       st.network[0] ? st.network : "");
    }

    if (!st.tailscale)
    {
        const char *routes = vpn_settings_config()->routes;

        cmdline_printf("  routes: %s\n",
                       routes[0] ? routes : "(none; default_route only)");
    }

    if (st.reason[0])
    {
        cmdline_printf("  held: %s\n", st.reason);
    }

    if (st.tailscale && st.ts_ip[0])
    {
        cmdline_printf("  tailnet ip %s, %d peer(s), relay %s\n", st.ts_ip,
                       st.ts_peers, st.ts_derp ? "up" : "down");

        static vpn_ts_peer_t peers[16] EXT_RAM_BSS_ATTR; /* console task only; PSRAM (1.3 KB) */
        int n = vpn_ts_get_peers(peers, 16);

        for (int i = 0; i < n; i++)
        {
            cmdline_printf("  peer %-16s %-15s %s %s\n",
                           peers[i].hostname, peers[i].ip,
                           peers[i].online ? "online " : "offline",
                           peers[i].direct_path ? "direct" : "relay");
        }

        /* the subnet routes learned from the tailnet (2026-11-01) */
        static vpn_ts_route_t routes[VPN_TS_MAX_ROUTES] EXT_RAM_BSS_ATTR; /* PSRAM */
        int nr = vpn_ts_get_routes(routes, VPN_TS_MAX_ROUTES);

        for (int i = 0; i < nr; i++)
        {
            cmdline_printf("  route %-18s via %s\n", routes[i].cidr, routes[i].via);
        }
    }

    cmdline_printf("  connects %lu, failures %lu, uptime %lu s\n",
                   (unsigned long)st.connects,
                   (unsigned long)st.failures,
                   (unsigned long)st.uptime_s);
    return 0;
}

esp_err_t vpn_manager_register_cli(void)
{
    static const esp_console_cmd_t CMD =
    {
        .command = "vpn",
        .help = "VPN (WireGuard) status",
        .func = cmd_vpn,
    };

    return cmdline_manager_register(&CMD);
}
