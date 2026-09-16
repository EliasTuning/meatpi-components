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
 * @file obd_gate_diag.c
 * @brief Pure diagnostics-hold bookkeeping (host-tested — no RTOS): the
 *        set of tools holding the bus for themselves, and the poller's
 *        acknowledgement. See obd_gate_private.h for the contract.
 */
#include "obd_gate_private.h"

#include <string.h>

void og_diag_init(og_diag_t *d)
{
    if (d != NULL)
    {
        memset(d, 0, sizeof(*d));
    }
}

bool og_diag_set(og_diag_t *d, const void *owner, bool on)
{
    if (d == NULL || owner == NULL)
    {
        return false;
    }

    bool before = d->n_holders > 0;
    int idx = -1;

    for (int i = 0; i < d->n_holders; i++)
    {
        if (d->holders[i] == owner)
        {
            idx = i;
            break;
        }
    }

    if (on)
    {
        if (idx < 0)
        {
            if (d->n_holders >= OG_DIAG_MAX_HOLDERS)
            {
                d->overflow++;
                return false;
            }

            d->holders[d->n_holders++] = owner;
            d->holds++;
        }
    }
    else if (idx >= 0)
    {
        for (int i = idx; i + 1 < d->n_holders; i++)
        {
            d->holders[i] = d->holders[i + 1];
        }

        d->n_holders--;
        d->holders[d->n_holders] = NULL;
        d->releases++;

        if (d->n_holders == 0)
        {
            d->acked = false; /* the next hold waits for a fresh ack */
        }
    }

    bool after = d->n_holders > 0;

    return before != after;
}

bool og_diag_held(const og_diag_t *d)
{
    return d != NULL && d->n_holders > 0;
}
