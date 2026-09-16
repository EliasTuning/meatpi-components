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
 * @file autopid_cli.c
 * @brief The `autopid` console command (replaces main_cli's pending stub).
 *        Registered from the component's settings apply (Standard §6b).
 */
#include <stdio.h>
#include <string.h>

#include "argtable3/argtable3.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cmdline_manager.h"

#include "autopid.h"
#include "autopid_private.h"

static struct
{
    struct arg_lit *list;
    struct arg_lit *dtc;
    struct arg_lit *dtc_scan;
    struct arg_end *end;
} s_args;

static void print_codes(const char *label,
                        const char codes[][AP_DTC_CODE_LEN], uint8_t n)
{
    cmdline_printf("%s (%u):", label, n);

    for (uint8_t i = 0; i < n; i++)
    {
        cmdline_printf(" %s", codes[i]);
    }

    cmdline_printf("\n");
}

static int cmd_dtc(bool scan)
{
    if (scan)
    {
        esp_err_t err = ap_dtc_scan_start();

        if (err == ESP_ERR_NOT_ALLOWED)
        {
            cmdline_printf("dtc_enabled is off\n");
            return 1;
        }

        if (err != ESP_OK)
        {
            cmdline_printf("scan not started (busy?)\n");
            return 1;
        }

        /* worst case = 4 requests x 5 s (SEARCHING on a cold protocol) */
        int waited = 0;

        while (ap_dtc_busy() && waited < 25000)
        {
            vTaskDelay(pdMS_TO_TICKS(200));
            waited += 200;
        }
    }

    ap_dtc_report_t r;

    (void)ap_dtc_report_get(&r);

    if (!r.valid)
    {
        cmdline_printf("no valid scan yet%s%s\n",
                       (r.error[0] != '\0') ? " - " : "", r.error);
        return scan ? 1 : 0;
    }

    cmdline_printf("MIL: %s (count %u)\n", r.mil ? "ON" : "off",
                   r.mil_count);
    print_codes("stored", r.stored, r.n_stored);
    print_codes("pending", r.pending, r.n_pending);
    print_codes("permanent", r.permanent, r.n_permanent);
    print_codes("new", r.new_codes, r.n_new);

    if (r.error[0] != '\0')
    {
        cmdline_printf("last error: %s\n", r.error);
    }

    cmdline_printf("OK\n");
    return 0;
}

static int cmd_autopid(int argc, char **argv)
{
    int errors = arg_parse(argc, argv, (void **)&s_args);

    if (errors != 0)
    {
        cmdline_printf("Usage: autopid [-l] [-d] [--dtc-scan]\n");
        return 1;
    }

    if (s_args.dtc->count > 0 || s_args.dtc_scan->count > 0)
    {
        return cmd_dtc(s_args.dtc_scan->count > 0);
    }

    autopid_stats_t st;

    autopid_stats(&st);
    cmdline_printf("AutoPID: %s%s%s%s\n",
                   st.running ? "running" : "idle",
                   st.paused_voltage ? " (voltage pause)" : "",
                   st.paused_client ? " (yielding to an OBD app)" : "",
                   st.paused_diag ? " (paused for a diagnostic tool)" : "");
    cmdline_printf("Tables: %lu pids, %lu filters, %lu params, %lu groups\n",
                   (unsigned long)st.pids_loaded,
                   (unsigned long)st.filters_loaded,
                   (unsigned long)st.params_loaded,
                   (unsigned long)st.groups_loaded);
    cmdline_printf("Polls: %lu ok, %lu failed\n",
                   (unsigned long)st.polls_ok,
                   (unsigned long)st.polls_failed);

    if (s_args.list->count > 0)
    {
        const ap_config_t *cfg = ap_core_config();
        int64_t now = esp_timer_get_time();

        for (uint16_t i = 0; i < cfg->n_params; i++)
        {
            double value;
            int64_t ts;

            if (ap_cache_get(i, &value, &ts))
            {
                cmdline_printf("%-24s %10.2f %-8s (%lld ms ago)\n",
                               cfg->params[i].name, value,
                               cfg->params[i].unit,
                               (long long)((now - ts) / 1000));
            }
            else
            {
                cmdline_printf("%-24s        --- %s\n",
                               cfg->params[i].name, cfg->params[i].unit);
            }
        }
    }

    cmdline_printf("OK\n");
    return 0;
}

esp_err_t autopid_register_cli(void)
{
    s_args.list = arg_lit0("l", "list", "list parameters + latest values");
    s_args.dtc = arg_lit0("d", "dtc", "show the last DTC report");
    s_args.dtc_scan = arg_lit0(NULL, "dtc-scan",
                               "run a DTC scan, then show the report");
    s_args.end = arg_end(3);

    static const esp_console_cmd_t CMD =
    {
        .command = "autopid",
        .help = "AutoPID status, values, and DTC report",
        .hint = "Usage: autopid [-l] [-d] [--dtc-scan]",
        .func = cmd_autopid,
        .argtable = &s_args,
    };

    return cmdline_manager_register(&CMD);
}
