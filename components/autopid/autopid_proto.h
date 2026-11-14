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
 * @file autopid_proto.h
 * @brief The vehicle protocols the OBD chip speaks, as ONE table (pure,
 *        host-tested): the ELM number, the name a person reads, the kind of
 *        bus, the pins, the bit rate, how a reply is framed, and what it
 *        costs to start the bus and to keep it.
 *
 * Until 2026-11-12 the firmware knew the four ISO 15765-4 CAN protocols and
 * kept what it knew in separate maps (the setting's enum, the prelude, the
 * bus guard's bit rates, the 29-bit rule, the page's names). The pre-CAN
 * protocols (J1850 PWM and VPW, ISO 9141-2, ISO 14230-4) come in through
 * this table: the setting, the prelude, the reply layout, the detection's
 * walk and GET /api/autopid/protocols read it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    AP_PK_J1850 = 1,    /* SAE J1850 PWM / VPW: pins 2 (and 10)              */
    AP_PK_KLINE,        /* ISO 9141-2 / ISO 14230-4: pin 7, a started bus    */
    AP_PK_CAN,          /* ISO 15765-4: pins 6 and 14                        */
    AP_PK_J1939,        /* SAE J1939 on CAN                                  */
    AP_PK_USER,         /* the chip's user CAN slots (B, C)                  */
} ap_proto_kind_t;

/** How the chip prints a reply with headers on. */
typedef enum
{
    AP_PF_CAN = 0,      /* the CAN id, then the frame (PCI first)            */
    AP_PF_HDR3_SUM,     /* three header bytes (.., .., the ECU), the data, a
                           checksum that is the sum of all before it: ISO
                           9141-2 (`48 6B 10 41 0C 0C 80 9C`)                */
    AP_PF_HDR3_CRC,     /* the same with the SAE J1850 CRC: VPW and PWM
                           (`48 6B 10 41 0C 0C 80 02`, `41 6B 10 ..`)        */
    AP_PF_KWP_SUM,      /* ISO 14230: a format byte with the data length in
                           its low six bits (0 = a length byte follows the
                           addresses), the tester, the ECU, the data, the
                           sum (`84 F1 10 41 0C 0C 80 5E`)                   */
} ap_proto_frame_t;

typedef struct
{
    char             id;        /* the ELM number: '1'..'9', 'A'..'C'        */
    ap_proto_kind_t  kind;
    ap_proto_frame_t frame;
    const char      *name;      /* "ISO 9141-2"                              */
    const char      *detail;    /* "K-line, 5 baud init"                     */
    const char      *pins;      /* of the OBD connector: "7"                 */
    uint32_t         bps;       /* the bus's bit rate, 0 = the user's own    */
    uint8_t          id_bits;   /* CAN: 11 or 29; 0 otherwise                */
    uint16_t         start_ms;  /* the bus is STARTED by its first request,
                                   and that takes this long (measured, the
                                   MIC3624 V2.3.24, 2026-11-12); 0 = none    */
    uint16_t         session_ms; /* the ECU ends the session after this long
                                   without a request; 0 = no session         */
} ap_proto_t;

/** The entry of an ELM number (any case), NULL when it is none ('0', the
 *  chip's search, is no protocol). */
const ap_proto_t *ap_proto(char id);

/** A protocol in words, the table's name and detail: "ISO 15765-4 CAN,
 *  11-bit, 500 kbit/s". "" for an id the table does not know. Returns the
 *  length written (2026-11-15: the push's `can_protocol`). */
size_t ap_proto_text(char id, char *out, size_t cap);

/** The table, in the order a person reads it (1 to 9, A to C). */
size_t ap_proto_count(void);
const ap_proto_t *ap_proto_at(size_t i);

/** J1850, ISO 9141-2 or ISO 14230-4: a reply with three header bytes and a
 *  checksum, no CAN id and no ISO-TP. */
bool ap_proto_precan(char id);

/** ISO 9141-2 or ISO 14230-4: the bus is started, and the session ends by
 *  itself. */
bool ap_proto_kline(char id);

/** What `std_protocol` may name beside "0" (automatic): the legislated
 *  OBD-II protocols, 1 to 9. */
bool ap_proto_pinnable(char id);

/** The pre-CAN protocols tried, in this order, when no CAN protocol
 *  answered: the cheapest first. SAE J1850 has no start of the bus and
 *  says `NO DATA` after the reply wait (VPW 0.67 s, PWM 0.62 s); the
 *  K-line's fast init gives up in 1.0 s, a 5 baud start in 2.8 s (ISO
 *  9141-2 before the rarer ISO 14230 5 baud). 7.9 s when nobody is there
 *  (bench 2026-11-13). A try on one of them is not answered by a car on
 *  another: VPW and PWM do not hear each other, and the key bytes of the
 *  5 baud start tell ISO 9141-2 from ISO 14230. */
const char *ap_proto_walk_precan(void);

/** A K-line session that is asked nothing for this long gets one short
 *  request from the poller (the ECU ends it at 5 s, the chip keeps none
 *  alive: bench 2026-11-12). Between this and the session's end the poller,
 *  which looks every half second, has a second to send it. */
#define AP_KLINE_KEEP_MS 3500

/** Does the poller send that request now? @p idle_ms: since the bus last
 *  answered in a live session (UINT32_MAX = no session); @p wait_ms: until
 *  its next row is due; @p since_keep_ms: since it last sent one
 *  (UINT32_MAX = never). Not when a row comes soon enough by itself, not
 *  twice for one silence (an unanswered one means the car is off: the
 *  session is left to end, and the next row starts the bus again). */
bool ap_proto_kline_keep(uint32_t idle_ms, uint32_t wait_ms,
                         uint32_t since_keep_ms);

/** The ISO 9141-2 / ISO 14230 checksum: the sum of the bytes. */
uint8_t ap_proto_sum8(const uint8_t *b, size_t n);

/** The SAE J1850 CRC (x^8 + x^4 + x^3 + x^2 + 1, all ones in, inverted
 *  out). */
uint8_t ap_proto_crc_j1850(const uint8_t *b, size_t n);

#ifdef __cplusplus
}
#endif
