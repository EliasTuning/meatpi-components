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
 * @file j2534_channel.h
 * @brief J2534 channel transport bindings: a CAN channel onto
 *        can_manager (raw frames), an ISO15765 channel onto the
 *        registered ISO-TP provider (can_isotp.h; without one,
 *        ISO15765 connect fails with a log line). Owned by
 *        j2534_server; the server pumps RX and pushes RX_MSG frames.
 *
 * Message data conventions (SAE J2534-1, both directions):
 *   CAN       msg.data = [4-byte big-endian CAN id][0..8 frame bytes]
 *   ISO15765  msg.data = [4-byte big-endian CAN id][UDS payload]; the
 *             ISO-TP session for the id comes from a FLOW_CONTROL filter
 *             (one session per filter, J2534_CH_MAX_SESSIONS per channel)
 *             or from the ids given at CONNECT (a WiCAN extension). A
 *             single-frame request to an id without a session (functional
 *             addressing) goes out raw; its single-frame replies come back
 *             through the channel's PASS filters. Padding follows each
 *             message's ISO15765_FRAME_PAD (2026-10-19; the bare payload
 *             and the pad flag on CONNECT before).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "j2534_proto.h"

#define J2534_CH_MAX_FILTERS  8
#define J2534_CH_MAX_SESSIONS 4   /* ISO-TP sessions (ECUs) per channel */

void j2534_channel_init(void);          /* one-time: mutex, the bus queues */
void j2534_channel_reset_all(void);     /* tester disconnected: tear down */

/**
 * The bus queue the RX pump may block on between frames: the first open
 * CAN or ISO15765 channel's (every such channel subscribes to every bus
 * frame), NULL when no channel has one (none open, or J1850 only). The
 * handle is static for the module's life (never deleted under a waiter);
 * xQueuePeek on it, never a receive: j2534_channel_poll_rx consumes.
 */
QueueHandle_t j2534_channel_wait_queue(void);

/**
 * The bus bitrate at CONNECT. The bus runs at the device's setting, at
 * what the listen-before-talk policy read off the wire, or at a bitrate a
 * tester stated; a CAN/ISO15765 CONNECT at another @p baud_bps is answered
 * ERR_INVALID_BAUDRATE. With `can_manager.baud = auto` and nothing proven
 * yet the tester's rate is stated to the node (can_manager_adopt_baud,
 * 2026-10-17): taken when the bus has been silent, otherwise the channel
 * still opens and its writes are refused with a log line. Returns a J2534
 * status code (NOERROR = go on and open the channel).
 */
uint32_t j2534_channel_bus_baud(uint32_t proto, uint32_t baud_bps);

/**
 * Bind a channel slot to a protocol. For ISO15765, @p tx_id/@p rx_id set
 * the ISO-TP addressing (0 = defer to a FLOW_CONTROL filter). Returns a
 * J2534 status code.
 */
uint32_t j2534_channel_connect(int slot, uint32_t protocol, uint32_t flags,
                               uint32_t tx_id, uint32_t rx_id, bool ext);

void j2534_channel_disconnect(int slot);
bool j2534_channel_active(int slot);

/**
 * Send one PASSTHRU_MSG on the channel. J2534 status code. An ISO15765
 * transmit is followed by a TxDone indication (RX_MSG with rx_status
 * TX_MSG_TYPE | TX_INDICATION and the id as data).
 */
uint32_t j2534_channel_write(int slot, const j2534_msg_t *msg);

/** The same for a periodic message: no TxDone indication (SAE J2534-1). */
uint32_t j2534_channel_write_periodic(int slot, const j2534_msg_t *msg);

/**
 * Add a filter. PASS/BLOCK (both protocols): matched in the RX pump over
 * the message data (the 4 id bytes first). FLOW_CONTROL (ISO15765 only):
 * pattern = the ECU's response id, flow-control message = the request id,
 * its TxFlags the 29-bit and padding choice; opens the ISO-TP session for
 * that ECU (re-arming an ECU's filter rebinds its session). Returns
 * status (ERR_EXCEEDED_LIMIT when the filter or session table is full);
 * *filter_id set on OK.
 */
uint32_t j2534_channel_add_filter(int slot, uint32_t type,
                                  const j2534_msg_t *mask,
                                  const j2534_msg_t *pattern,
                                  const j2534_msg_t *flow_control,
                                  uint32_t *filter_id);

uint32_t j2534_channel_stop_filter(int slot, uint32_t filter_id);
void j2534_channel_clear_filters(int slot);

/**
 * Poll one channel for a received message (non-blocking-ish; ISO15765
 * uses a short internal timeout). Returns true and fills @p out when a
 * message passed the channel's filters. The server's RX pump calls this
 * across all active channels and pushes RX_MSG frames.
 */
bool j2534_channel_poll_rx(int slot, j2534_msg_t *out);
