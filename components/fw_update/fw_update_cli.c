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
 * @file fw_update_cli.c
 * @brief The `fwupdate` console command: the summary, `check`, `install
 *        [<version>] [--wipe] [--dongle]`, `abort`, `notes [--dongle]` (the
 *        notes URL of the offered release: the browser fetches it, never the device),
 *        `confirm` and `forget` (the bench's knobs). Registered from the
 *        settings on_apply behind `cli` (standard §6b); handlers run on
 *        internal stacks, so the NVS erase of `forget` is legal here.
 */
#include "fw_update_private.h"

#include <string.h>

#include "argtable3/argtable3.h"
#include "esp_attr.h"

#include "cmdline_manager.h"

static struct
{
    struct arg_str *action;
    struct arg_str *version;
    struct arg_lit *wipe;
    struct arg_lit *dongle;
    struct arg_end *end;
} s_args;

static void print_summary(void)
{
    static fw_update_status_t st EXT_RAM_BSS_ATTR; /* 1 KB: off the stack */

    if (fw_update_status(&st) != ESP_OK)
    {
        cmdline_printf("fwupdate: not running\n");
        return;
    }

    cmdline_printf("channel: %s  scheduled check: %s (every %s)  downloads: WiFi only\n",
                   st.channel, st.enabled ? "on" : "off",
                   st.next_in_s >= 0 ? "due" : "never");

    if (st.next_in_s >= 0)
    {
        cmdline_printf("next check in %ld s\n", (long)st.next_in_s);
    }

    cmdline_printf("uplink: %s%s\n", st.uplink, st.is_cellular ? " (mobile)" : "");
    cmdline_printf("installed: %s  build: %s  slot: %s  state: %s%s\n",
                   st.installed, st.build, st.running, st.ota_state,
                   st.pending_verify ? " (not yet verified)" : "");
    cmdline_printf("layout: %s%s  bootloader: v%lu%s\n", st.flashed_md5,
                   strcmp(st.flashed_md5, st.build_md5) == 0 ? "" : " (differs from the build's)",
                   (unsigned long)st.bootloader_version,
                   st.bootloader_rollback ? " (rollback)" : "");

    if (!st.checked)
    {
        cmdline_printf("last check: none yet\n");
    }
    else
    {
        cmdline_printf("last check: %s %lu s ago%s%s%s\n", st.check_ok ? "ok" : "FAILED",
                       (unsigned long)st.check_age_s, st.etag_hit ? " (unchanged)" : "",
                       st.check_error[0] ? " " : "", st.check_error);
    }

    if (st.have_latest)
    {
        cmdline_printf("latest: %s (%s) %s\n", st.latest_version, st.latest_date,
                       st.latest_summary);
    }

    if (st.available)
    {
        cmdline_printf("target: %s%s  layout: %s%s%s  %s\n", st.target_version,
                       st.step ? " (a step, more follow)" : "", st.verdict,
                       st.wipes[0] ? " wipes " : "", st.wipes,
                       st.installable ? "installable" : st.reason);
    }
    else
    {
        cmdline_printf("target: none (%s)%s%s\n", st.reason[0] ? st.reason : "nothing",
                       st.installed_yanked ? "  INSTALLED RELEASE WITHDRAWN: " : "",
                       st.installed_yanked ? st.yank_reason : "");
    }

    if (st.job_active || st.job_state != FW_JOB_IDLE)
    {
        cmdline_printf("job %lu: %s %s %lu/%lu attempt %u%s%s%s%s\n",
                       (unsigned long)st.job_id, st.job_version,
                       fwu_job_state_str(st.job_state), (unsigned long)st.job_received,
                       (unsigned long)st.job_total, (unsigned)st.job_attempt,
                       st.job_error[0] ? " error " : "", st.job_error,
                       st.job_detail[0] ? ": " : "", st.job_detail);
    }

    if (st.have_last)
    {
        cmdline_printf("last result: job %lu %s -> %s %s%s%s\n",
                       (unsigned long)st.last_id, st.last_from, st.last_to,
                       fwu_job_state_str(st.last_state),
                       st.last_by[0] ? " by " : "", st.last_by);
    }

    if (st.dongle_wired)
    {
        cmdline_printf("dongle: %s%s%s  fw %s  api %d  %s%s%s\n",
                       st.dongle_present ? "present" : "none",
                       st.dongle_reachable ? " via " : "",
                       st.dongle_reachable ? st.dongle_transport : "",
                       st.dongle_installed[0] ? st.dongle_installed : "?",
                       st.dongle_api_level, st.dongle_verdict,
                       st.dongle_target_version[0] ? " -> " : "",
                       st.dongle_target_version);

        if (st.dongle_have_last)
        {
            cmdline_printf("dongle last: job %lu %s -> %s %s%s%s (push %lu s)\n",
                           (unsigned long)st.dongle_last_id, st.dongle_last_from,
                           st.dongle_last_to, fwu_job_state_str(st.dongle_last_state),
                           st.dongle_last_error[0] ? " " : "", st.dongle_last_error,
                           (unsigned long)st.dongle_last_push_s);
        }
    }
}

static int cmd_fwupdate(int argc, char **argv)
{
    int errors = arg_parse(argc, argv, (void **)&s_args);

    if (errors != 0)
    {
        arg_print_errors(stdout, s_args.end, argv[0]);
        return 1;
    }

    if (s_args.action->count == 0)
    {
        print_summary();
        cmdline_printf("OK\n");
        return 0;
    }

    const char *action = s_args.action->sval[0];
    const char *version = (s_args.version->count > 0) ? s_args.version->sval[0] : NULL;
    esp_err_t err;

    if (strcmp(action, "check") == 0)
    {
        uint32_t id = 0;

        err = fw_update_check_now(&id);
        cmdline_printf("check: %s (job %lu)\n", esp_err_to_name(err), (unsigned long)id);
    }
    else if (strcmp(action, "install") == 0)
    {
        uint32_t id = 0;
        uint32_t flags = 0;
        char why[FW_UPDATE_CODE_LEN] = "";
        bool already = false;

        if (s_args.wipe->count > 0)
        {
            flags |= FW_UPDATE_F_CONFIRM_WIPE;
        }

        fw_update_target_t tg = (s_args.dongle->count > 0) ? FW_UPDATE_TARGET_DONGLE
                                                           : FW_UPDATE_TARGET_SELF;

        err = fw_update_install(tg, version, flags, &id, why, &already);

        if (already)
        {
            cmdline_printf("install: already running that version\n");
        }
        else
        {
            cmdline_printf("install: %s%s%s (job %lu)\n", esp_err_to_name(err),
                           why[0] ? " " : "", why, (unsigned long)id);
        }
    }
    else if (strcmp(action, "abort") == 0)
    {
        err = fw_update_abort();
        cmdline_printf("abort: %s\n", esp_err_to_name(err));
    }
    else if (strcmp(action, "notes") == 0)
    {
        /* the device never fetches the notes (2026-11-02): say where they are */
        static fw_update_status_t st EXT_RAM_BSS_ATTR;
        const char *url;

        err = fw_update_status(&st);
        url = (s_args.dongle->count > 0) ? st.dongle_latest_notes_url : st.latest_notes_url;
        cmdline_printf("notes: %s\n", (err == ESP_OK && url[0]) ? url
                       : "none offered yet (run `fwupdate check`); the web page fetches them");
    }
    else if (strcmp(action, "confirm") == 0)
    {
        err = fw_update_confirm();
        cmdline_printf("confirm: %s\n", esp_err_to_name(err));
    }
    else if (strcmp(action, "forget") == 0)
    {
        err = fw_update_forget();
        cmdline_printf("forget: %s\n", esp_err_to_name(err));
    }
    else
    {
        cmdline_printf("fwupdate: unknown action '%s'\n", action);
        return 1;
    }

    cmdline_printf(err == ESP_OK ? "OK\n" : "ERROR\n");
    return err == ESP_OK ? 0 : 1;
}

esp_err_t fw_update_register_cli(void)
{
    s_args.action = arg_str0(NULL, NULL, "<check|install|abort|notes|confirm|forget>",
                             "bare = the summary");
    s_args.version = arg_str0(NULL, NULL, "<version>", "install: a release");
    s_args.wipe = arg_lit0(NULL, "wipe", "install: accept a layout that formats");
    s_args.dongle = arg_lit0(NULL, "dongle", "install/notes: the ESPNetLink dongle");
    s_args.end = arg_end(4);

    static const esp_console_cmd_t CMD =
    {
        .command = "fwupdate",
        .help = "Firmware updates from the manifest: summary; check; install "
                "[<version>] [--wipe] [--dongle]; abort; notes; confirm; forget",
        .hint = NULL,
        .func = cmd_fwupdate,
        .argtable = &s_args,
    };

    return cmdline_manager_register(&CMD);
}
