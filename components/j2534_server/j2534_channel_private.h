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
 * @file j2534_channel_private.h
 * @brief The channel record shared by j2534_channel.c (lifecycle, CAN,
 *        filters) and j2534_channel_iso.c (the ISO15765 side). Every
 *        function here runs with the channel lock held by the caller.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "can_isotp.h"

#include "j2534_channel.h"
#include "j2534_proto.h"

#define J2534_CH_CAN_QLEN 32
#define J2534_CH_MAX_IND  4   /* pending TxDone / START_OF_MESSAGE indications */

typedef struct
{
    uint32_t id;        /* filter id (1-based, per channel)                    */
    uint32_t type;      /* PASS / BLOCK / FLOW_CONTROL                         */
    uint8_t  mask[8];
    uint8_t  pattern[8];
    uint8_t  len;
    int      session;   /* FLOW_CONTROL: the ISO-TP session it bound, else -1 */
} j2534_ch_filter_t;

typedef struct
{
    bool                in_use;
    uint32_t            tx_id;
    uint32_t            rx_id;
    bool                ext;
    bool                pad;    /* frames padded to 8 bytes (the message TxFlags) */
    can_isotp_session_t isotp;
} j2534_iso_session_t;

typedef struct
{
    uint32_t rx_status;
    uint32_t id;
    bool     ext;
    uint32_t ts_us;
} j2534_ch_ind_t;

typedef struct
{
    bool     in_use;
    uint32_t protocol;
    uint32_t flags;
    bool     ext;                    /* CONNECT: 29-bit ids                      */

    QueueHandle_t can_q;             /* CAN: every frame on the bus; ISO15765:   */
    int           can_sub;           /* first-frame peeks + frames no session owns */

    j2534_ch_filter_t filters[J2534_CH_MAX_FILTERS];
    int               n_filters;
    uint32_t          next_filter_id;

    j2534_iso_session_t sess[J2534_CH_MAX_SESSIONS];

    j2534_ch_ind_t ind[J2534_CH_MAX_IND];   /* ring, drained by the RX pump     */
    uint8_t        ind_head;
    uint8_t        ind_tail;

    uint8_t leg_hdr[J2534_J1850_HDR];  /* J1850 (legacy): the ATSH header cache  */
    bool    leg_have_hdr;              /* so a repeated header is not re-sent     */

    /* Raw CAN TP2.0 block ACK. The tester's ACK otherwise waits for the
     * frame to cross WiFi and come back, and the ECU sits between blocks.
     * The ids come from the channel-setup response (opcode 0xD0), not from
     * whichever bus frame last looked like a wait-ack: other ids on this
     * bus use a low nibble that counts, and treating one of those as the
     * ECU makes the adapter ACK the tester's own channel. A wait-ack is
     * answered here only when its sequence is the next one, or a replay
     * of the last one. The tool's copy of that ACK is dropped. */
    bool     tp_learned;
    bool     tp_have_seq;
    uint32_t tp_tx_id;               /* tester -> ECU                        */
    uint32_t tp_rx_id;               /* ECU -> tester                        */
    uint8_t  tp_last_seq;
    uint8_t  tp_last_ack;
    int64_t  tp_ack_us;
} j2534_channel_t;

/* ---- j2534_channel.c, used by the ISO15765 file ---------------------- */

/** The once-per-CONNECT "write refused" line + the tx_refused counter. */
void j2534_ch_say_refused(const char *what);

/** A tester's request service byte: a UDS memory-transfer service
 *  (0x34..0x37) is counted on the status (`reflash_seen`), never refused
 *  (the content gate went on 2026-10-27). */
void j2534_ch_note_reflash(uint8_t sid);

/** PASS/BLOCK verdict over a [4-byte id][frame] view. No PASS filter at
 *  all passes everything (the CAN channel's rule). */
bool j2534_ch_filters_pass(const j2534_channel_t *c, const uint8_t *view,
                           size_t len);

/** True when at least one PASS filter is armed. */
bool j2534_ch_has_pass_filter(const j2534_channel_t *c);

/* ---- j2534_channel_iso.c -------------------------------------------- */

/** An ISO-TP provider is registered and the CAN node is up. */
bool j2534_iso_provider_ready(void);

/**
 * Open (or, for an rx id the channel already serves, reopen) the ISO-TP
 * session for an ECU. J2534 status: NOERROR (*slot = the session index),
 * ERR_EXCEEDED_LIMIT (the channel's or the provider's table is full),
 * ERR_FAILED (no provider, the id held elsewhere, the bus down).
 */
uint32_t j2534_iso_bind(j2534_channel_t *c, uint32_t tx_id, uint32_t rx_id,
                        bool ext, bool pad, int *slot);

void j2534_iso_unbind(j2534_channel_t *c, int slot);
void j2534_iso_unbind_all(j2534_channel_t *c);

/** WRITE_MSGS on an ISO15765 channel (the J2534-1 layout). @p indicate =
 *  queue a TxDone indication on success (false for periodic messages). */
uint32_t j2534_iso_write(j2534_channel_t *c, const j2534_msg_t *msg,
                         bool indicate);

/** One received message or indication, if any (the RX pump's poll). */
bool j2534_iso_poll(j2534_channel_t *c, j2534_msg_t *out);

/* ---- j2534_channel_legacy.c (J1850 VPW over the OBD chip) ------------ */

/** The OBD chip is present and finished bring-up (J1850 needs it). */
bool j2534_legacy_provider_ready(void);

/**
 * Bring a J1850 channel up: run the VPW protocol init on the OBD chip
 * (ATTP2, headers on, auto-receive, fixed timing). J2534 status:
 * NOERROR, ERR_FAILED (chip not ready), ERR_NOT_SUPPORTED (a J1850
 * protocol other than VPW, not built yet).
 */
uint32_t j2534_legacy_connect(j2534_channel_t *c);

/**
 * WRITE_MSGS on a J1850 channel: data = [3-byte header][payload]. Sets
 * the chip header (cached), sends the payload (a bare hex line, or
 * VTFullyRequestCk for the chip to segment), parses the reply lines into
 * the slot's RX ring. @p indicate queues a TxDone echo first. The reflash
 * gate applies to the payload's first byte.
 */
uint32_t j2534_legacy_write(j2534_channel_t *c, int slot,
                            const j2534_msg_t *msg, bool indicate);

/** One buffered J1850 message (or TxDone echo) for @p slot, if any. */
bool j2534_legacy_poll(j2534_channel_t *c, int slot, j2534_msg_t *out);

/** Drop every buffered message for @p slot (channel teardown). */
void j2534_legacy_drop(int slot);
