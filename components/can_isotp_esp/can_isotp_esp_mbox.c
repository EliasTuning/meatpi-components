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
 * @file can_isotp_esp_mbox.c
 * @brief PDU mailbox ring (pure; see can_isotp_esp_private.h).
 */
#include "can_isotp_esp_private.h"

#include <string.h>

void isotp_mbox_init(isotp_mbox_t *m, isotp_mbox_slot_t *slots,
                     uint8_t n_slots)
{
    memset(m, 0, sizeof(*m));
    m->slots = slots;
    m->n_slots = n_slots;
}

bool isotp_mbox_put(isotp_mbox_t *m, const uint8_t *data, size_t len)
{
    if (m == NULL)
    {
        return false;
    }

    if (m->slots == NULL || m->n_slots == 0 || data == NULL || len == 0 ||
        len > CAN_ISOTP_ESP_MAX_PDU)
    {
        m->refused++;
        return false;
    }

    bool grew = true;

    if (m->count == m->n_slots)
    {
        /* full: the newest PDU matters more than the oldest unread one
           (a late reply nobody waited for) */
        m->head = (uint8_t)((m->head + 1) % m->n_slots);
        m->count--;
        m->dropped++;
        grew = false;
    }

    isotp_mbox_slot_t *slot = &m->slots[(m->head + m->count) % m->n_slots];

    memcpy(slot->data, data, len);
    slot->len = (uint16_t)len;
    m->count++;
    return grew;
}

esp_err_t isotp_mbox_take(isotp_mbox_t *m, uint8_t *buf, size_t cap,
                          size_t *len)
{
    if (m == NULL || buf == NULL || len == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *len = 0;

    if (m->count == 0)
    {
        return ESP_ERR_NOT_FOUND;
    }

    isotp_mbox_slot_t *slot = &m->slots[m->head];

    m->head = (uint8_t)((m->head + 1) % m->n_slots);
    m->count--;
    *len = slot->len;

    if (slot->len > cap)
    {
        return ESP_ERR_NO_MEM; /* consumed, per the provider contract */
    }

    memcpy(buf, slot->data, slot->len);
    return ESP_OK;
}

size_t isotp_mbox_count(const isotp_mbox_t *m)
{
    return (m != NULL) ? m->count : 0;
}
