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
 * @file can_isotp_esp_private.h
 * @brief The pure part of the provider: a per-session mailbox ring of
 *        reassembled PDUs (no RTOS, host-testable). The provider task
 *        puts, the consumer's recv() takes; the RTOS side pairs every
 *        put that grows the ring with one counting-semaphore token.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Largest PDU one session moves: SAE J2534's PASSTHRU_MSG data cap. UDS
 *  never needs more (a 4 KB TransferData block is the reflash worst case). */
#define CAN_ISOTP_ESP_MAX_PDU    4128

/** PDUs a session can hold unread before the oldest is dropped (a 0x78
 *  responsePending burst ahead of the final response fits). */
#define CAN_ISOTP_ESP_MBOX_SLOTS 4

typedef struct
{
    uint16_t len;
    uint8_t  data[CAN_ISOTP_ESP_MAX_PDU];
} isotp_mbox_slot_t;

typedef struct
{
    isotp_mbox_slot_t *slots;   /* n_slots entries, caller-owned (PSRAM) */
    uint8_t            n_slots;
    uint8_t            head;    /* oldest unread */
    uint8_t            count;
    uint32_t           dropped; /* full: oldest discarded for the newest */
    uint32_t           refused; /* put() larger than a slot, or empty    */
} isotp_mbox_t;

void isotp_mbox_init(isotp_mbox_t *m, isotp_mbox_slot_t *slots,
                     uint8_t n_slots);

/** Store a PDU. Returns true when the unread count GREW (the RTOS side
 *  then gives one semaphore token); false when the ring was full and the
 *  oldest was replaced (count unchanged — no new token), or when @p len
 *  does not fit a slot (refused, nothing stored). */
bool isotp_mbox_put(isotp_mbox_t *m, const uint8_t *data, size_t len);

/** Take the oldest PDU. ESP_OK (+*len); ESP_ERR_NOT_FOUND when empty;
 *  ESP_ERR_NO_MEM when the PDU is larger than @p cap — it has been
 *  CONSUMED (the can_isotp.h contract callers drain on) and *len tells
 *  its size. */
esp_err_t isotp_mbox_take(isotp_mbox_t *m, uint8_t *buf, size_t cap,
                          size_t *len);

size_t isotp_mbox_count(const isotp_mbox_t *m);

#ifdef __cplusplus
}
#endif
