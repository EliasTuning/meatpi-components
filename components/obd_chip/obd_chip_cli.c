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
 * @file obd_chip_cli.c
 * @brief The `obd_chip` console command (standard §6b, registered from the
 *        settings apply behind `cli`, 2026-10-28): the chip's state and
 *        firmware version, the packaged image and its policy, a forced
 *        update as a background job, and the wire tools (probe, scan, the
 *        hardware baud detection, listen) for a chip that will not come up.
 *        Issue #1023: a stuck MIC3624 could neither be diagnosed nor
 *        reflashed from the device. The tools run through the one runner
 *        in obd_chip_probe.c that the HTTP route shares; this file prints.
 */
#include "obd_chip.h"

#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "cmdline_manager.h"

#include "obd_chip_private.h"

/* the runner's result: ~3 KB, PSRAM (§2); the console runs one tool at a
 * time (the dispatcher serializes commands) */
static obd_probe_result_t s_result EXT_RAM_BSS_ATTR;

static void print_status(void)
{
    obd_chip_stats_t st;
    obd_chip_fw_status_t fw;
    char ver[64];

    obd_chip_get_stats(&st);
    obd_chip_fw_status(&fw);

    cmdline_printf("chip:    ready=%d claim=%s baud=%d rx_buffered=%lu\n",
                   st.ready ? 1 : 0, st.claim, obd_chip_active_baud(),
                   (unsigned long)st.rx_buffered);

    if (fw.running)
    {
        cmdline_printf("version: - (update running)\n");
    }
    else if (st.ready &&
             obd_chip_get_version(ver, sizeof(ver), pdMS_TO_TICKS(3000)) ==
                 ESP_OK)
    {
        cmdline_printf("version: %s\n", ver);

        if (obd_fw_version_is_outdated(ver, obd_chip_builtin_fw_version()))
        {
            cmdline_printf("warning: the chip's firmware is older than the "
                           "packaged %s: obd_chip update --force rewrites it "
                           "(or System > Maintenance)\n",
                           obd_chip_builtin_fw_version());
        }
    }
    else
    {
        cmdline_printf("version: - (%s)\n",
                       st.ready ? "no answer" : "chip not up; see: obd_chip probe");
    }

    if (!st.ready)
    {
        obd_wire_note_t wire[2];
        size_t n = obd_chip_bringup_wire(wire, 2);

        for (size_t i = 0; i < n; i++)
        {
            if (wire[i].bytes == 0)
            {
                cmdline_printf("bring-up: at %d baud nothing received\n",
                               wire[i].baud);
            }
            else
            {
                cmdline_printf("bring-up: at %d baud %lu byte(s), no prompt "
                               "(%s)\n", wire[i].baud,
                               (unsigned long)wire[i].bytes, wire[i].hex);
            }
        }
    }

    cmdline_printf("builtin: %s (auto_update %s; current: %s)\n",
                   obd_chip_builtin_fw_version(),
                   obd_settings_config()->auto_update ? "on" : "off",
                   obd_fw_current_versions());
    cmdline_printf("eeprom:  guard rewrites %lu, blocked %lu, protocol saves "
                   "%lu, ATM0 sent %lu, app resets %lu\n",
                   (unsigned long)st.guard_rewrites,
                   (unsigned long)st.guard_blocked,
                   (unsigned long)st.protocol_saves,
                   (unsigned long)st.memory_off,
                   (unsigned long)st.app_resets);

    if (fw.running)
    {
        cmdline_printf("update:  running %lu/%lu records (%s)\n",
                       (unsigned long)fw.lines, (unsigned long)fw.total,
                       fw.source);
    }
    else if (fw.done_once)
    {
        cmdline_printf("update:  idle; last %s: %s\n", fw.source,
                       esp_err_to_name(fw.last_err));
    }
    else
    {
        cmdline_printf("update:  idle\n");
    }
}

static int do_update(int argc, char **argv)
{
    bool force = false;
    const char *path = NULL;

    for (int i = 2; i < argc; i++)
    {
        if (strcmp(argv[i], "--force") == 0)
        {
            force = true;
        }
        else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc)
        {
            path = argv[++i];
        }
        else
        {
            cmdline_printf("usage: obd_chip update [--force] [--file <path>]\n");
            return 1;
        }
    }

    esp_err_t err = obd_chip_firmware_update_start(path, force);

    if (err == ESP_ERR_INVALID_STATE)
    {
        cmdline_printf("an update is already running\n");
        return 1;
    }

    if (err != ESP_OK)
    {
        cmdline_printf("could not start the update (%s)\n",
                       esp_err_to_name(err));
        return 1;
    }

    cmdline_printf("update started from %s%s: about 20 s, the chip is busy "
                   "meanwhile; `obd_chip` shows the progress\n",
                   (path != NULL) ? path : "the packaged image",
                   force ? " (forced)" : "");
    return 0;
}

/* ---- the wire tools: run, then print the result ------------------------------ */

static void print_lines(const obd_probe_result_t *r)
{
    for (size_t i = 0; i < r->n_lines; i++)
    {
        const obd_probe_line_t *ln = &r->lines[i];

        if (ln->bytes == 0)
        {
            cmdline_printf("  %7d %-7s silent\n", ln->baud, ln->cmd);
            continue;
        }

        cmdline_printf("  %7d %-7s prompt=%d %3lu B '%s'%s %s\n", ln->baud,
                       ln->cmd, ln->prompt ? 1 : 0, (unsigned long)ln->bytes,
                       ln->text, (ln->bytes > strlen(ln->text)) ? " ..." : "",
                       ln->hex);
    }
}

static void print_raw(const obd_probe_result_t *r)
{
    if (r->raw_total == 0)
    {
        cmdline_printf("  nothing received at %d baud\n", r->raw_baud);
        return;
    }

    for (size_t off = 0; off < r->raw_len; off += OBD_PROBE_TEXT)
    {
        size_t n = ((r->raw_len - off) < OBD_PROBE_TEXT) ? (r->raw_len - off)
                                                          : OBD_PROBE_TEXT;
        char text[OBD_PROBE_TEXT + 1];
        char hex[3 * OBD_PROBE_TEXT + 1];
        size_t h = 0;

        for (size_t i = 0; i < n; i++)
        {
            uint8_t c = r->raw[off + i];

            text[i] = (c >= 32 && c < 127) ? (char)c : '.';
            h += (size_t)snprintf(hex + h, sizeof(hex) - h, "%s%02X",
                                  (i > 0) ? " " : "", c);
        }

        text[n] = '\0';
        cmdline_printf("  '%s' %s\n", text, hex);
    }

    cmdline_printf("  %lu byte(s) at %d baud%s%s\n", (unsigned long)r->raw_total,
                   r->raw_baud, r->raw_prompt ? ", ended at a prompt" : "",
                   (r->raw_total > r->raw_len) ? " (first 512 shown)" : "");
}

static int run_tool(obd_probe_kind_t kind, int baud, int seconds,
                    const char *cmd)
{
    esp_err_t err = obd_chip_probe_run(kind, baud, seconds, cmd, &s_result);

    if (err == ESP_ERR_INVALID_STATE)
    {
        cmdline_printf("the chip is busy (a firmware update or a tool "
                       "running, or the bring-up)\n");
        return 1;
    }

    if (err != ESP_OK && err != ESP_ERR_NO_MEM)
    {
        cmdline_printf("  %s\n", esp_err_to_name(err));
    }

    return (err == ESP_OK) ? 0 : 1;
}

#define PROBE_USAGE "obd_chip probe [baud] [scan|auto [seconds]] [command]"

/* A number is a baud (above 60) or, after auto, the window in seconds
 * (1..60); any other word is the stimulus sent instead of ATI (VTDLMIC3422
 * asks a chip whose application is gone for its bootloader, 2026-10-28). */
static int do_probe(int argc, char **argv)
{
    obd_probe_kind_t kind = OBD_PROBE_PROBE;
    int baud = 0;
    int seconds = 10;
    const char *cmd = NULL;

    for (int i = 2; i < argc; i++)
    {
        if (strcmp(argv[i], "scan") == 0)
        {
            kind = OBD_PROBE_SCAN;
        }
        else if (strcmp(argv[i], "auto") == 0)
        {
            kind = OBD_PROBE_AUTO;
        }
        else if (argv[i][0] >= '0' && argv[i][0] <= '9')
        {
            int n = atoi(argv[i]);

            if (kind == OBD_PROBE_AUTO && n >= 1 && n <= 60)
            {
                seconds = n;
            }
            else if (n > 60)
            {
                baud = n;
            }
            else
            {
                cmdline_printf("usage: %s\n", PROBE_USAGE);
                return 1;
            }
        }
        else if (cmd == NULL && strlen(argv[i]) < OBD_PROBE_CMD)
        {
            cmd = argv[i];
        }
        else
        {
            cmdline_printf("usage: %s\n", PROBE_USAGE);
            return 1;
        }
    }

    const char *stim = (cmd != NULL) ? cmd : "ATI";

    if (kind == OBD_PROBE_SCAN)
    {
        size_t n;

        (void)obd_chip_probe_bauds(&n);
        cmdline_printf("baud scan with %s (about %u s, the chip is busy):\n",
                       stim, (unsigned)n);
        int rc = run_tool(OBD_PROBE_SCAN, 0, 0, cmd);

        print_lines(&s_result);

        if (s_result.detected != 0)
        {
            cmdline_printf("prompt at %d baud\n", s_result.detected);
        }
        else
        {
            cmdline_printf("no prompt at any baud\n");
        }

        return rc;
    }

    if (kind == OBD_PROBE_AUTO)
    {
        cmdline_printf("baud detection: CR + %s at %d baud, then %d s of "
                       "measuring the RX edges:\n", stim,
                       (baud > 0) ? baud : obd_uart_get_baud(), seconds);
        int rc = run_tool(OBD_PROBE_AUTO, baud, seconds, cmd);

        if (s_result.edges == 0)
        {
            cmdline_printf("  no edges: the chip sent nothing in %d s\n", seconds);
            return rc;
        }

        cmdline_printf("  %lu edges, bit time says %d baud\n",
                       (unsigned long)s_result.edges, s_result.measured);

        if (s_result.detected == 0)
        {
            cmdline_printf("  no standard baud within 8 %% of %d\n",
                           s_result.measured);
            return rc;
        }

        cmdline_printf("  nearest standard baud: %d; listening there 6 s:\n",
                       s_result.detected);
        print_raw(&s_result);
        return rc;
    }

    cmdline_printf("bare-wire probe with %s%s (the chip is busy for a few "
                   "seconds):\n", stim, (cmd == NULL) ? " and VTVERS" : "");
    int rc = run_tool(OBD_PROBE_PROBE, baud, 0, cmd);

    print_lines(&s_result);
    return rc;
}

static int do_listen(int argc, char **argv)
{
    int seconds = (argc > 2) ? atoi(argv[2]) : 5;
    int baud = (argc > 3) ? atoi(argv[3]) : 0;

    if (seconds <= 0 || seconds > 60 || baud < 0)
    {
        cmdline_printf("usage: obd_chip listen <seconds 1..60> [baud]\n");
        return 1;
    }

    cmdline_printf("listening %d s at %d baud, sending nothing:\n", seconds,
                   (baud > 0) ? baud : obd_uart_get_baud());
    int rc = run_tool(OBD_PROBE_LISTEN, baud, seconds, NULL);

    print_raw(&s_result);
    return rc;
}

/** `obd_chip report`: the chip configuration block for a bug report. */
static int do_report(void)
{
    static obd_chip_report_t s_report EXT_RAM_BSS_ATTR;
    esp_err_t err = obd_chip_report_run(&s_report);

    if (err == ESP_ERR_INVALID_STATE)
    {
        cmdline_printf("the chip is not up or an update runs: see obd_chip probe\n");
        return 1;
    }

    if (err != ESP_OK)
    {
        cmdline_printf("could not read the chip (%s)\n", esp_err_to_name(err));
        return 1;
    }

    cmdline_printf("--- OBD chip configuration ---\n");

    for (size_t i = 0; i < s_report.n; i++)
    {
        const obd_chip_report_item_t *it = &s_report.items[i];

        if (it->err != ESP_OK)
        {
            cmdline_printf("%-8s %s: %s\n", it->cmd, it->what,
                           esp_err_to_name(it->err));
        }
        else if (strchr(it->resp, '\n') == NULL)
        {
            cmdline_printf("%-8s %s: %s\n", it->cmd, it->what, it->resp);
        }
        else
        {
            cmdline_printf("%-8s %s:\n", it->cmd, it->what);

            const char *p = it->resp;

            while (*p != '\0')
            {
                const char *e = strchr(p, '\n');
                size_t len = (e != NULL) ? (size_t)(e - p) : strlen(p);

                cmdline_printf("         %.*s\n", (int)len, p);
                p += len + ((e != NULL) ? 1 : 0);
            }
        }
    }

    cmdline_printf("--- end ---\n");
    return 0;
}

static int cmd_obd_chip(int argc, char **argv)
{
    if (argc < 2)
    {
        print_status();
        return 0;
    }

    if (strcmp(argv[1], "update") == 0)
    {
        return do_update(argc, argv);
    }

    if (strcmp(argv[1], "report") == 0)
    {
        return do_report();
    }

    if (strcmp(argv[1], "probe") == 0)
    {
        return do_probe(argc, argv);
    }

    if (strcmp(argv[1], "listen") == 0)
    {
        return do_listen(argc, argv);
    }

    cmdline_printf("usage: obd_chip | obd_chip report | obd_chip update "
                   "[--force] [--file <path>] | obd_chip probe [baud] "
                   "[scan|auto [seconds]] [command] | obd_chip listen "
                   "<seconds> [baud]\n");
    return 1;
}

esp_err_t obd_chip_register_cli(void)
{
    static const esp_console_cmd_t CMD =
    {
        .command = "obd_chip",
        .help = "OBD chip (MIC3624): state, configuration report, firmware "
                "update, probe, listen",
        .hint = "Usage: obd_chip | obd_chip report | obd_chip update [--force] "
                "[--file <path>] | obd_chip probe [baud] [scan|auto [seconds]] [command] | "
                "obd_chip listen <seconds> [baud]",
        .func = cmd_obd_chip,
    };

    return cmdline_manager_register(&CMD);
}
