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
 * @file j2534_channel_tp20.h
 * @brief SAE J2534-2 TP2.0 (protocol 0x800E) for one channel.
 *
 * Every function runs with the channel lock held by the caller, the same
 * way j2534_channel_iso.c does. Raw CAN does not call in here.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "j2534_proto.h"

struct j2534_channel;

/** Allocate the session table (four connections) and apply table-77 defaults. */
uint32_t j2534_tp20_attach(struct j2534_channel *c);

/** Send 0xA8 on every connection that has a TX id, then free the table. */
void j2534_tp20_detach(struct j2534_channel *c);

/**
 * WRITE_MSGS. A matching TX id is segmented (7-byte telegrams, block ACKs
 * on the device) and goes out as written: the bytes behind the id are the
 * message, the device puts nothing in front of them (SAE J2534-2 19.3.1).
 * A broadcast flag sends the five-shot. Anything else that
 * fits in one CAN frame goes out raw; a longer write to an unknown id is
 * ERR_NO_CONNECTION_ESTABLISHED. @p indicate is unused: a TxDone would look
 * like a KWP payload to the tester.
 */
uint32_t j2534_tp20_write(struct j2534_channel *c, const j2534_msg_t *msg,
                          bool indicate);

/**
 * Firing number @p tick of a periodic message (0 = the one sent when the
 * periodic message is started). With the broadcast flag this is the
 * re-triggered broadcast (SAE J2534-2 19.3.2.3): the five telegrams at
 * tick 0, then one telegram per tick, the key bytes alternating over the
 * whole run. Without the flag it is j2534_tp20_write() with no indication.
 */
uint32_t j2534_tp20_write_periodic(struct j2534_channel *c,
                                   const j2534_msg_t *msg, uint32_t tick);

/**
 * One application message, connection indication, or raw frame that matched
 * a user filter and belongs to no connection. Advances setup retries,
 * the parameter exchange, and the connection test (one every T_CTa,
 * answered by the other side with a connection acknowledge).
 */
bool j2534_tp20_poll(struct j2534_channel *c, j2534_msg_t *out);

/**
 * SET_CONFIG, GET_CONFIG, REQUEST_CONNECTION, TEARDOWN_CONNECTION.
 * GET_CONFIG writes `u32 count` plus the filled pairs into @p out and sets
 * @p out_len. The others leave @p out_len 0.
 */
uint32_t j2534_tp20_ioctl(struct j2534_channel *c, uint32_t ioctl_id,
                          const uint8_t *body, uint32_t body_len,
                          uint8_t *out, uint32_t out_cap, uint32_t *out_len);
