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
 * @file autopid_proto.c
 * @brief The vehicle protocol table (autopid_proto.h): PURE, host-tested.
 *
 * The start and session times are the bench's (2026-11-12, the MIC3624
 * V2.3.24 against ECU sim v2): a 5 baud start prints `OK` at 2.6 s and the
 * reply 150 ms later, or `ERROR` at 2.9 s when nobody is there; a fast init
 * is through in 0.7 s (1.0 s to `ERROR`); a session lasts through 4.6 s
 * without a request and is gone after 5.0 s. `start_ms` is what a request
 * that starts the bus is given, with room; `session_ms` is how long a
 * session is taken to last.
 */
#include "autopid_proto.h"

#include <stdio.h>

static const ap_proto_t PROTOCOLS[] =
{
    { '1', AP_PK_J1850, AP_PF_HDR3_CRC, "SAE J1850 PWM", "41.6 kbit/s, Ford",
      "2 and 10", 41600, 0, 0, 0 },
    { '2', AP_PK_J1850, AP_PF_HDR3_CRC, "SAE J1850 VPW", "10.4 kbit/s, GM",
      "2", 10400, 0, 0, 0 },
    { '3', AP_PK_KLINE, AP_PF_HDR3_SUM, "ISO 9141-2", "K-line, 5 baud init",
      "7", 10400, 0, 5000, 4500 },
    { '4', AP_PK_KLINE, AP_PF_KWP_SUM, "ISO 14230-4 KWP",
      "K-line, 5 baud init", "7", 10400, 0, 5000, 4500 },
    { '5', AP_PK_KLINE, AP_PF_KWP_SUM, "ISO 14230-4 KWP",
      "K-line, fast init", "7", 10400, 0, 3000, 4500 },
    { '6', AP_PK_CAN, AP_PF_CAN, "ISO 15765-4 CAN", "11-bit, 500 kbit/s",
      "6 and 14", 500000, 11, 0, 0 },
    { '7', AP_PK_CAN, AP_PF_CAN, "ISO 15765-4 CAN", "29-bit, 500 kbit/s",
      "6 and 14", 500000, 29, 0, 0 },
    { '8', AP_PK_CAN, AP_PF_CAN, "ISO 15765-4 CAN", "11-bit, 250 kbit/s",
      "6 and 14", 250000, 11, 0, 0 },
    { '9', AP_PK_CAN, AP_PF_CAN, "ISO 15765-4 CAN", "29-bit, 250 kbit/s",
      "6 and 14", 250000, 29, 0, 0 },
    { 'A', AP_PK_J1939, AP_PF_CAN, "SAE J1939", "29-bit, 250 kbit/s",
      "6 and 14", 250000, 29, 0, 0 },
    { 'B', AP_PK_USER, AP_PF_CAN, "User CAN 1", "the chip's own definition",
      "6 and 14", 0, 29, 0, 0 },
    { 'C', AP_PK_USER, AP_PF_CAN, "User CAN 2", "the chip's own definition",
      "6 and 14", 0, 29, 0, 0 },
};

#define N_PROTOCOLS (sizeof(PROTOCOLS) / sizeof(PROTOCOLS[0]))

const ap_proto_t *ap_proto(char id)
{
    if (id >= 'a' && id <= 'c')
    {
        id = (char)(id - 'a' + 'A');
    }

    for (size_t i = 0; i < N_PROTOCOLS; i++)
    {
        if (PROTOCOLS[i].id == id)
        {
            return &PROTOCOLS[i];
        }
    }

    return NULL;
}

size_t ap_proto_count(void)
{
    return N_PROTOCOLS;
}

size_t ap_proto_text(char id, char *out, size_t cap)
{
    const ap_proto_t *p = ap_proto(id);

    if (out == NULL || cap == 0)
    {
        return 0;
    }

    out[0] = '\0';

    if (p == NULL)
    {
        return 0;
    }

    int n = snprintf(out, cap, "%s, %s", p->name, p->detail);

    return (n < 0) ? 0 : ((size_t)n < cap ? (size_t)n : cap - 1);
}

const ap_proto_t *ap_proto_at(size_t i)
{
    return (i < N_PROTOCOLS) ? &PROTOCOLS[i] : NULL;
}

bool ap_proto_precan(char id)
{
    const ap_proto_t *p = ap_proto(id);

    return p != NULL && (p->kind == AP_PK_J1850 || p->kind == AP_PK_KLINE);
}

bool ap_proto_kline(char id)
{
    const ap_proto_t *p = ap_proto(id);

    return p != NULL && p->kind == AP_PK_KLINE;
}

bool ap_proto_pinnable(char id)
{
    return id >= '1' && id <= '9';
}

const char *ap_proto_walk_precan(void)
{
    return "21534";
}

bool ap_proto_kline_keep(uint32_t idle_ms, uint32_t wait_ms,
                         uint32_t since_keep_ms)
{
    const uint32_t session_ms = 4500; /* the K-line entries' session_ms */

    if (idle_ms == UINT32_MAX || idle_ms < AP_KLINE_KEEP_MS ||
        idle_ms >= session_ms)
    {
        return false; /* no session, one that is fresh, or one that is gone */
    }

    if (wait_ms < 500)
    {
        return false; /* the next row is the request */
    }

    /* one per silence: the answer to it sets the idle time back to zero */
    return since_keep_ms >= AP_KLINE_KEEP_MS;
}

uint8_t ap_proto_sum8(const uint8_t *b, size_t n)
{
    uint8_t sum = 0;

    for (size_t i = 0; i < n; i++)
    {
        sum = (uint8_t)(sum + b[i]);
    }

    return sum;
}

uint8_t ap_proto_crc_j1850(const uint8_t *b, size_t n)
{
    uint8_t crc = 0xFF;

    for (size_t i = 0; i < n; i++)
    {
        crc ^= b[i];

        for (int bit = 0; bit < 8; bit++)
        {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x1D)
                               : (uint8_t)(crc << 1);
        }
    }

    return (uint8_t)~crc;
}
