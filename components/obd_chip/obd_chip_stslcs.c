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
 * @file obd_chip_stslcs.c
 * @brief PURE STSLCS sleep-config parsing + the boot provisioning
 *        policy (ported from legacy main/obd.c: the sscanf patterns
 *        ARE the accepted line grammar), and the VTPPSWS read of the
 *        vendor's PowerEn switch (2026-11-07). No IDF deps; host-tested.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "obd_chip_private.h"

static int parse_on_off(const char *s)
{
    return (strcmp(s, "ON") == 0) ? 1 : 0;
}

static int parse_high_low(const char *s)
{
    return (strcmp(s, "HIGH") == 0) ? 1 : 0;
}

static uint32_t parse_time(const char *s)
{
    uint32_t value = 0;
    char unit[5];

    if (sscanf(s, "%lu %4s", (unsigned long *)&value, unit) == 2)
    {
        if (strcmp(unit, "ms") == 0 || strcmp(unit, "s") == 0)
        {
            return value;
        }
    }

    return 0;
}

static void parse_line(const char *line, obd_stslcs_t *c)
{
    char buffer[64];
    char enable[10];
    char level[10];
    char timebuf[50];
    float voltage = 0;

    if (strstr(line, "CTRL MODE"))
    {
        sscanf(line, "CTRL MODE: %9s", c->ctrl_mode);
    }
    else if (strstr(line, "PWR_CTRL"))
    {
        if (sscanf(line, "PWR_CTRL: %*[^=]= %63s", buffer) == 1)
        {
            c->pwr_ctrl = parse_high_low(buffer);
        }
    }
    else if (strstr(line, "UART_SLEEP"))
    {
        if (sscanf(line, "UART_SLEEP: %9[^,], %49[^\n]", enable,
                   timebuf) == 2)
        {
            c->uart_sleep.en = parse_on_off(enable);
            c->uart_sleep.time = parse_time(timebuf);
        }
    }
    else if (strstr(line, "UART_WAKE"))
    {
        if (sscanf(line, "UART_WAKE: %9[^,], %lu-%lu", enable,
                   (unsigned long *)&c->uart_wake.min_time,
                   (unsigned long *)&c->uart_wake.max_time) >= 1)
        {
            c->uart_wake.en = parse_on_off(enable);
        }
    }
    else if (strstr(line, "EXT_INPUT"))
    {
        if (sscanf(line, "EXT_INPUT: %9s", level) == 1)
        {
            c->ext_input_level = parse_high_low(level);
        }
    }
    else if (strstr(line, "EXT_SLEEP"))
    {
        if (sscanf(line, "EXT_SLEEP: %9[^,], %9[^,], FOR %49[^\n]",
                   enable, level, timebuf) == 3)
        {
            c->ext_sleep.en = parse_on_off(enable);
            c->ext_sleep.level = parse_high_low(level);
            c->ext_sleep.time = parse_time(timebuf);
        }
    }
    else if (strstr(line, "EXT_WAKE"))
    {
        if (sscanf(line, "EXT_WAKE: %9[^,], %9[^,], FOR %49[^\n]",
                   enable, level, timebuf) == 3)
        {
            c->ext_wake.en = parse_on_off(enable);
            c->ext_wake.level = parse_high_low(level);
            c->ext_wake.time = parse_time(timebuf);
        }
    }
    else if (strstr(line, "VL_SLEEP"))
    {
        if (sscanf(line, "VL_SLEEP: %9[^,], <%fV FOR %49[^\n]", enable,
                   &voltage, timebuf) == 3)
        {
            c->vl_sleep.en = parse_on_off(enable);
            c->vl_sleep.voltage = voltage;
            c->vl_sleep.time = parse_time(timebuf);
        }
    }
    else if (strstr(line, "VL_WAKE"))
    {
        /* the chip prints ">!" once armed-and-tripped; plain ">" armed */
        if (sscanf(line, "VL_WAKE: %9[^,], >!%fV FOR %49[^\n]", enable,
                   &voltage, timebuf) != 3)
        {
            if (sscanf(line, "VL_WAKE: %9[^,], >%fV FOR %49[^\n]",
                       enable, &voltage, timebuf) != 3)
            {
                return;
            }
        }

        c->vl_wake.en = parse_on_off(enable);
        c->vl_wake.voltage = voltage;
        c->vl_wake.time = parse_time(timebuf);
    }
    else if (strstr(line, "VCHG WAKE"))
    {
        if (sscanf(line, "VCHG WAKE: %9[^,], %fV IN %49[^\n]", enable,
                   &voltage, timebuf) == 3)
        {
            c->vchg_wake.en = parse_on_off(enable);
            c->vchg_wake.voltage_change = voltage;
            c->vchg_wake.time = parse_time(timebuf);
        }
    }
}

void obd_stslcs_parse(const char *response, obd_stslcs_t *out)
{
    char line[128];
    size_t n = 0;

    memset(out, 0, sizeof(*out));

    for (const char *p = response;; p++)
    {
        if (*p == '\r' || *p == '\n' || *p == '\0')
        {
            if (n > 0)
            {
                line[n] = '\0';
                parse_line(line, out);
                n = 0;
            }

            if (*p == '\0')
            {
                break;
            }
        }
        else if (n < sizeof(line) - 1)
        {
            line[n++] = *p;
        }
    }
}

bool obd_stslcs_needs_provision(const obd_stslcs_t *c, float wake_v,
                                float sleep_v, uint32_t sleep_time_s)
{
    /* the legacy reprogram condition: any autonomous control left ON,
       or stored thresholds/time differ from the settings. Voltage
       compare needs an epsilon here: the chip echoes text, we hold
       millivolt-derived floats. */
    return c->uart_wake.en == 1 || c->uart_sleep.en == 1 ||
           c->vl_wake.en == 1 || c->vl_sleep.en == 1 ||
           fabsf(c->vl_wake.voltage - wake_v) > 0.005f ||
           fabsf(c->vl_sleep.voltage - sleep_v) > 0.005f ||
           c->vl_sleep.time != sleep_time_s;
}

/* ---- VTPPSWS: the vendor's PowerEn switch (2026-11-07) ------------------------ */

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }

    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }

    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }

    return -1;
}

bool obd_parse_ppsw(const char *resp, uint8_t *value)
{
    if (resp == NULL || value == NULL)
    {
        return false;
    }

    const char *p = resp;

    while (*p != '\0')
    {
        while (*p == '\r' || *p == '\n' || *p == ' ')
        {
            p++;
        }

        const char *line = p;

        while (*p != '\0' && *p != '\r' && *p != '\n')
        {
            p++;
        }

        size_t n = (size_t)(p - line);

        while (n > 0 && line[n - 1] == ' ')
        {
            n--;
        }

        if (n == 2 && hex_digit(line[0]) >= 0 && hex_digit(line[1]) >= 0)
        {
            *value = (uint8_t)((hex_digit(line[0]) << 4) | hex_digit(line[1]));
            return true;
        }
    }

    return false;
}
