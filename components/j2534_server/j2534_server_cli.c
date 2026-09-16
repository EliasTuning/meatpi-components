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
 * @file j2534_server_cli.c
 * @brief `j2534` console command — status snapshot.
 */
#include "cmdline_manager.h"

#include "j2534_server.h"
#include "j2534_server_private.h"

static int cmd_j2534(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    j2534_server_status_t st;

    if (j2534_server_status(&st) != ESP_OK)
    {
        cmdline_printf("status unavailable\n");
        return 1;
    }

    cmdline_printf("J2534 PassThru server (Phase 1: handshake)\n");
    cmdline_printf("  enabled     : %s\n", st.enabled ? "yes" : "no");
    cmdline_printf("  tcp port    : %u\n", st.port);
    cmdline_printf("  listening   : %s\n", st.listening ? "yes" : "no");
    cmdline_printf("  tester      : %s\n",
                   st.client_connected ? "connected" : "-");
    cmdline_printf("  device_open : %s\n", st.device_open ? "yes" : "no");
    cmdline_printf("  exclusive   : %s%s\n", st.exclusive ? "yes" : "no",
                   st.autopid_paused ? " (autopid paused)" : "");
    cmdline_printf("  channels    : %u\n", st.channel_count);
    cmdline_printf("  frames rx/tx: %lu / %lu\n",
                   (unsigned long)st.frames_rx,
                   (unsigned long)st.frames_tx);
    cmdline_printf("OK\n");
    return 0;
}

esp_err_t j2534_server_register_cli(void)
{
    static const esp_console_cmd_t CMD =
    {
        .command = "j2534",
        .help = "SAE J2534 PassThru server status",
        .func = cmd_j2534,
    };

    return cmdline_manager_register(&CMD);
}
