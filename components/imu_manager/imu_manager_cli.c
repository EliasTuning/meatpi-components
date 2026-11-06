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
 * @file imu_manager_cli.c
 * @brief The component's CLI command (`imu`): registered into
 *        cmdline_manager by imu_manager_register_cli() (main wires it
 *        in CLI compositions only). Legacy option interface preserved
 *        (-i/--id byte-alike); -r/--read and the bare summary are v6.
 */
#include "argtable3/argtable3.h"

#include "cmdline_manager.h"

#include "imu_manager.h"

static struct
{
    struct arg_lit *id;
    struct arg_lit *read;
    struct arg_lit *regs;
    struct arg_end *end;
} s_args;

/* -x/--regs: the detector registers as the chip holds them (2026-11-06:
 * the imu.bump flood of a Nissan Ariya owner: the WoM thresholds never
 * reached the chip, this dump is what showed it) */
static int imu_regs(void)
{
    imu_manager_regs_t r;
    esp_err_t err = imu_manager_read_regs(&r);

    if (err != ESP_OK)
    {
        cmdline_printf("Error: register read failed: %s\n",
                       esp_err_to_name(err));
        return 1;
    }

    cmdline_printf("WHO_AM_I 0x%02X  MCLK_RDY 0x%02X (%s)\n", r.who_am_i,
                   r.mclk_rdy, (r.mclk_rdy & 0x08) ? "running" : "off");
    cmdline_printf("PWR_MGMT0 0x%02X (accel %u gyro %u idle %u lp_clk %s)\n",
                   r.pwr_mgmt0, (unsigned)(r.pwr_mgmt0 & 0x03),
                   (unsigned)((r.pwr_mgmt0 >> 2) & 0x03),
                   (unsigned)((r.pwr_mgmt0 >> 4) & 0x01),
                   (r.pwr_mgmt0 & 0x80) ? "rc" : "wuosc");
    cmdline_printf("ACCEL_CONFIG0 0x%02X  INT_CONFIG 0x%02X  INT_SOURCE1 0x%02X\n",
                   r.accel_config0, r.int_config, r.int_source1);
    cmdline_printf("APEX_CONFIG0 0x%02X  APEX_CONFIG1 0x%02X  WOM_CONFIG 0x%02X\n",
                   r.apex_config0, r.apex_config1, r.wom_config);
    cmdline_printf("MREG1: WOM_THR X %u Y %u Z %u (x3.9 mg)  APEX_CONFIG9 0x%02X "
                   "(SMD sensitivity %u)%s\n",
                   r.wom_thr[0], r.wom_thr[1], r.wom_thr[2], r.apex_config9,
                   (unsigned)((r.apex_config9 >> 1) & 0x07),
                   (r.mclk_rdy & 0x08) ? "" : "  [MCLK off: MREG1 values stale]");
    cmdline_printf("OK\n");
    return 0;
}

static int imu_read(void)
{
    static const char *ACT[] = { "stationary", "active", "unknown" };
    float ax;
    float ay;
    float az;
    float temp;

    cmdline_printf("Activity: %s\n", ACT[imu_manager_activity()]);

    if (imu_manager_read_accel(&ax, &ay, &az) != ESP_OK)
    {
        cmdline_printf("Error: Failed to read accelerometer\n");
        return 1;
    }

    cmdline_printf("Accel: %.3f %.3f %.3f g\n", ax, ay, az);

    if (imu_manager_read_temp(&temp) == ESP_OK)
    {
        cmdline_printf("Temperature: %.1f C\n", temp);
    }

    cmdline_printf("OK\n");
    return 0;
}

static int cmd_imu(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_args);

    if (nerrors != 0)
    {
        arg_print_errors(stderr, s_args.end, argv[0]);
        return 1;
    }

    if (s_args.id->count > 0)
    {
        uint8_t id;

        if (imu_manager_device_id(&id) != ESP_OK)
        {
            cmdline_printf("Error: Failed to read IMU ID\n");
            return 1;
        }

        cmdline_printf("IMU Device ID: 0x%02X\n", id);
        cmdline_printf("OK\n");
        return 0;
    }

    if (s_args.regs->count > 0)
    {
        return imu_regs();
    }

    return imu_read(); /* bare and -r/--read: the full v6 summary */
}

esp_err_t imu_manager_register_cli(void)
{
    static const esp_console_cmd_t CMD =
    {
        .command = "imu",
        .help = "IMU control and status",
        .hint = "Options: -i/--id, -r/--read, -x/--regs",
        .func = cmd_imu,
        .argtable = &s_args,
    };

    s_args.id = arg_lit0("i", "id", "Get IMU device ID");
    s_args.read = arg_lit0("r", "read", "Read activity, accel, temperature");
    s_args.regs = arg_lit0("x", "regs", "Dump the detector registers");
    s_args.end = arg_end(3);
    return cmdline_manager_register(&CMD);
}
