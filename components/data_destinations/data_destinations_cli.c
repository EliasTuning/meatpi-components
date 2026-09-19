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
 * @file data_destinations_cli.c
 * @brief The `destinations [-t <name>]` console command: the table with
 *        its counters, or one synchronous test delivery. Registered by
 *        data_destinations_register_cli() from on_apply, gated by the
 *        `cli` setting (§6b). main wires nothing. The table is borrowed
 *        under the config lock (never copied onto the console stack).
 */
#include <string.h>

#include "argtable3/argtable3.h"
#include "cmdline_manager.h"

#include "data_destinations.h"
#include "data_destinations_private.h"

static struct
{
    struct arg_str *test;
    struct arg_end *end;
} s_args;

static int cmd_destinations(int argc, char **argv)
{
    int errors = arg_parse(argc, argv, (void **)&s_args);

    if (errors != 0)
    {
        arg_print_errors(stdout, s_args.end, argv[0]);
        return 1;
    }

    if (!dd_config_is_configured())
    {
        cmdline_printf("destinations: not configured\n");
        return 0;
    }

    if (s_args.test->count > 0)
    {
        data_destinations_result_t r = { 0 };
        esp_err_t rc = data_destinations_test(s_args.test->sval[0], &r);

        if (rc != ESP_OK)
        {
            cmdline_printf("test %s: %s\n", s_args.test->sval[0],
                           esp_err_to_name(rc));
            return 1;
        }

        cmdline_printf("test %s: %s (status %d, %u ms)%s%s\nOK\n",
                       s_args.test->sval[0], r.ok ? "ok" : "FAILED",
                       r.status, (unsigned)r.elapsed_ms,
                       r.error[0] ? " " : "", r.error);
        return 0;
    }

    dd_config_lock();

    const dd_config_t *cfg = dd_config_peek();

    cmdline_printf("enabled: %s  destinations: %u\n",
                   cfg->enabled ? "yes" : "no", (unsigned)cfg->n);

    for (size_t i = 0; i < cfg->n; i++)
    {
        const dd_dest_t *d = &cfg->dest[i];
        dd_state_t st;

        dd_config_unlock();
        (void)dd_state_get(i, &st);
        dd_config_lock();
        cmdline_printf("  %-15s %-5s %-3s every %us  %s\n", d->name,
                       dd_type_str(d->type), d->enabled ? "on" : "off",
                       (unsigned)d->period_s, d->url);
        cmdline_printf("      auth %s  ok %u  fail %u  offline %u  "
                       "backoff %us  last_status %d\n",
                       dd_auth_str(d->auth), (unsigned)st.success,
                       (unsigned)st.fail, (unsigned)st.skipped_offline,
                       (unsigned)(st.backoff_ms / 1000), st.last_status);

        if (st.last_error[0])
        {
            cmdline_printf("      last_err %s (%s)\n", st.last_error,
                           st.last_error_time);
        }
    }

    dd_config_unlock();
    cmdline_printf("OK\n");
    return 0;
}

esp_err_t data_destinations_register_cli(void)
{
    s_args.test = arg_str0("t", "test", "<name>", "deliver once, now");
    s_args.end = arg_end(2);

    static const esp_console_cmd_t CMD =
    {
        .command = "destinations",
        .help = "Data destinations: table + counters; -t <name> = test once",
        .hint = NULL,
        .func = cmd_destinations,
        .argtable = &s_args,
    };

    return cmdline_manager_register(&CMD);
}
