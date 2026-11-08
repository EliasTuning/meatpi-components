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
 * @file ble_manager_diag.c
 * @brief The pure half of the link diagnostics (2026-11-08): the words the
 *        session log lines are made of. No stack, no clock, no logging:
 *        host-tested (host_test/main/test_diag.c). The log lines themselves
 *        are written by ble_manager.c from the GATT layer's events.
 *
 * Why: a field report about an app ("takes four tries", "connects and
 * answers nothing", "drops after a minute") used to leave two lines on the
 * console, `client connected` and `client disconnected`, and the build
 * compiles the debug level out (CONFIG_LOG_MAXIMUM_LEVEL 3), so nothing
 * said who connected, which connection window the phone settled on, when
 * it paired, when the app first wrote, how much flowed, or why the link
 * ended. Those are the facts that separate an app problem from a link
 * problem from a pipe problem, and they are one line each per session.
 */
#include "ble_manager_private.h"

#include <stdio.h>
#include <string.h>

/* NimBLE reports a disconnect as BLE_HS_ERR_HCI_BASE (0x200) + the HCI
   error code of the Disconnection Complete event; a host-side reason
   (a timeout the host raised itself) comes bare. The names are the Core
   spec's, shortened to what a reader needs. */
#define BLM_HCI_BASE 0x200

const char *blm_diag_reason_name(int reason)
{
    if (reason == 0)
    {
        return "no reason given";
    }

    if (reason < BLM_HCI_BASE || reason > BLM_HCI_BASE + 0xFF)
    {
        return "host error";
    }

    switch (reason - BLM_HCI_BASE)
    {
        case 0x05: return "authentication failure";
        case 0x06: return "key missing on the peer (stale bond)";
        case 0x08: return "supervision timeout (radio link lost)";
        case 0x13: return "the peer ended it";
        case 0x14: return "the peer ended it (low resources)";
        case 0x15: return "the peer ended it (power off)";
        case 0x16: return "the device ended it";
        case 0x1F: return "unspecified";
        case 0x22: return "LL response timeout";
        case 0x3B: return "unacceptable connection parameters";
        case 0x3D: return "MIC failure (encryption)";
        case 0x3E: return "connection failed to be established";
        default:   return "other HCI reason";
    }
}

const char *blm_diag_addr_type_name(uint8_t type)
{
    /* NimBLE's BLE_ADDR_* values: 0 public, 1 random, 2 public identity,
       3 random identity (the last two: an RPA the controller resolved) */
    switch (type)
    {
        case 0:  return "public";
        case 1:  return "random";
        case 2:  return "public identity";
        case 3:  return "random identity";
        default: return "unknown type";
    }
}

int blm_diag_format_addr(const uint8_t addr[6], char *out, size_t len)
{
    if (addr == NULL)
    {
        return snprintf(out, len, "unknown");
    }

    /* NimBLE keeps addresses little-endian: the printed form is reversed */
    return snprintf(out, len, "%02X:%02X:%02X:%02X:%02X:%02X",
                    addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
}

int blm_diag_format_window(uint16_t itvl_units, uint16_t latency,
                           uint16_t timeout_units, char *out, size_t len)
{
    /* the interval in 1.25 ms units, the supervision timeout in 10 ms units
       (Core spec 7.8.12); printed in the units a phone's log shows */
    unsigned itvl_x100 = (unsigned)itvl_units * 125u;

    return snprintf(out, len, "interval %u.%02u ms latency %u timeout %u ms",
                    itvl_x100 / 100u, itvl_x100 % 100u, (unsigned)latency,
                    (unsigned)timeout_units * 10u);
}

static int put_secs(char *out, size_t len, uint32_t ms)
{
    return snprintf(out, len, "%lu.%lu s", (unsigned long)(ms / 1000u),
                    (unsigned long)((ms % 1000u) / 100u));
}

/* The whole disconnect line must fit the log manager's line
   (CONFIG_LOG_MANAGER_LINE_MAX, 256 B with the level, the time and the tag):
   a longer one is cut without its newline and the next line is glued on
   (seen on the first image, 2026-11-08). Hence the short words: wr =
   writes, ntf = notifications, the drops as "sub" (the subscriber queue,
   bridge side) and "tx" (the radio queue). */
int blm_diag_format_summary(const blm_diag_session_t *s, char *out, size_t len)
{
    char dur[24], sec[48], first[48], last[72];

    put_secs(dur, sizeof(dur), s->duration_ms);

    if (s->secured_at_ms != BLM_DIAG_NEVER)
    {
        char t[16];

        put_secs(t, sizeof(t), s->secured_at_ms);
        snprintf(sec, sizeof(sec), "secured %s", t);
    }
    else
    {
        snprintf(sec, sizeof(sec), "never secured");
    }

    if (s->first_write_at_ms != BLM_DIAG_NEVER)
    {
        char t[16];

        put_secs(t, sizeof(t), s->first_write_at_ms);
        snprintf(first, sizeof(first), "first write %s", t);
    }
    else
    {
        snprintf(first, sizeof(first), "no app write");
    }

    if (s->last_write_at_ms != BLM_DIAG_NEVER && s->duration_ms >= s->last_write_at_ms)
    {
        char t[16];

        put_secs(t, sizeof(t), s->duration_ms - s->last_write_at_ms);
        snprintf(last, sizeof(last), ", last write %s before end", t);
    }
    else
    {
        last[0] = '\0';
    }

    return snprintf(out, len,
                    "%s, %s, %s%s, pipe %s, app->dev %lu wr %lu B, dev->app %lu ntf "
                    "%lu B, drops sub %lu tx %lu",
                    dur, sec, first, last,
                    s->subscribed ? "subscribed" : "not subscribed",
                    (unsigned long)s->rx_writes, (unsigned long)s->rx_bytes,
                    (unsigned long)s->tx_notifications, (unsigned long)s->tx_bytes,
                    (unsigned long)s->sub_drops, (unsigned long)s->tx_drops);
}
