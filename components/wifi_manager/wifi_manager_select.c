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
 * @file wifi_manager_select.c
 * @brief Pure STA candidate selection + per-entry attempt-failure memory
 *        (an ordering hint, never a block: meatpi 2026-09-06).
 *        No IDF dependencies: compiled as-is by the host unit tests.
 *        Time comes in as a millisecond tick from the caller.
 */
#include "wifi_manager_private.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool wm_default_hostname(const char *configured, const char *device_id,
                         char *out, size_t cap)
{
    if (out == NULL || cap == 0)
    {
        return false;
    }

    out[0] = '\0';

    if (configured != NULL && configured[0] != '\0')
    {
        return snprintf(out, cap, "%s", configured) < (int)cap;
    }

    if (device_id == NULL || device_id[0] == '\0')
    {
        return false;
    }

    return snprintf(out, cap, "wican_%s", device_id) < (int)cap;
}

bool wm_parse_ipv4(const char *s, uint32_t *out)
{
    if (s == NULL || out == NULL)
    {
        return false;
    }

    uint32_t octet[4];

    for (int i = 0; i < 4; i++)
    {
        if (*s < '0' || *s > '9') /* reject empty / leading sign / space */
        {
            return false;
        }

        char *end = NULL;
        long v = strtol(s, &end, 10);

        if (end == s || v < 0 || v > 255)
        {
            return false;
        }

        octet[i] = (uint32_t)v;
        s = end;

        if (i < 3)
        {
            if (*s != '.')
            {
                return false;
            }
            s++;
        }
    }

    if (*s != '\0')
    {
        return false; /* trailing junk */
    }

    *out = (octet[0] << 24) | (octet[1] << 16) | (octet[2] << 8) | octet[3];
    return true;
}

bool wm_parse_ap_ipv4(const char *s, uint32_t *out)
{
    uint32_t v;

    if (!wm_parse_ipv4(s, &v))
    {
        return false;
    }

    /* a /24 host octet of 0 (network) or 255 (broadcast) is not a usable
       host/gateway address */
    if ((v & 0xFFu) == 0 || (v & 0xFFu) == 255)
    {
        return false;
    }

    *out = v;
    return true;
}

bool wm_netmask_valid(uint32_t mask)
{
    if (mask == 0 || mask == 0xFFFFFFFFu)
    {
        return false;
    }

    /* contiguous ones from the MSB: the inverse must be 2^n - 1 */
    uint32_t inv = ~mask;

    return ((inv + 1u) & inv) == 0;
}

void wm_select_init(wm_select_state_t *st)
{
    memset(st, 0, sizeof(*st));
    /* -1 = "nothing returned yet" so the first sequential pick is the
     * PRIMARY (a zeroed cursor made the first rotation start at [1]) */
    st->seq_cursor = -1;
    st->dep_cursor = -1;
}

/* ---- failure memory ------------------------------------------------------ */

static wm_fail_entry_t *entry(wm_select_state_t *st, int idx)
{
    return (idx >= 0 && idx < WM_MAX_CANDIDATES) ? &st->fails[idx] : NULL;
}

/* tick-wrap-safe: live while less than WM_FAIL_MEMORY_MS passed since the
   newest failure */
static bool memory_live(const wm_fail_entry_t *e, uint32_t now_ms)
{
    return e->fail_count > 0 &&
           (uint32_t)(now_ms - e->last_fail_ms) < WM_FAIL_MEMORY_MS;
}

uint8_t wm_select_fail_count(const wm_select_state_t *st, int idx,
                             uint32_t now_ms)
{
    if (idx < 0 || idx >= WM_MAX_CANDIDATES)
    {
        return 0;
    }

    return memory_live(&st->fails[idx], now_ms) ? st->fails[idx].fail_count
                                               : 0;
}

bool wm_select_is_deprioritised(const wm_select_state_t *st, int idx,
                                uint32_t now_ms)
{
    return wm_select_fail_count(st, idx, now_ms) >= WM_AUTH_FAIL_THRESHOLD;
}

void wm_select_on_attempt_fail(wm_select_state_t *st, int idx,
                               uint32_t now_ms)
{
    wm_fail_entry_t *e = entry(st, idx);

    if (e == NULL)
    {
        return;
    }

    if (!memory_live(e, now_ms))
    {
        e->fail_count = 0; /* faded: an old streak does not count */
    }

    if (e->fail_count < 255)
    {
        e->fail_count++;
    }

    e->last_fail_ms = now_ms;
}

void wm_select_deprioritise(wm_select_state_t *st, int idx, uint32_t now_ms)
{
    wm_fail_entry_t *e = entry(st, idx);

    if (e == NULL)
    {
        return;
    }

    if (!memory_live(e, now_ms) || e->fail_count < WM_AUTH_FAIL_THRESHOLD)
    {
        e->fail_count = WM_AUTH_FAIL_THRESHOLD;
    }

    e->last_fail_ms = now_ms;
}

void wm_select_on_success(wm_select_state_t *st, int idx)
{
    wm_fail_entry_t *e = entry(st, idx);

    if (e != NULL)
    {
        e->fail_count = 0;
        e->last_fail_ms = 0;
    }
}

/* ---- selection ----------------------------------------------------------- */

static bool ssid_present(const char (*present)[WM_SSID_LEN],
                         size_t present_count, const char *ssid)
{
    if (ssid == NULL || ssid[0] == '\0')
    {
        return false;
    }

    for (size_t i = 0; i < present_count; i++)
    {
        if (strcmp(present[i], ssid) == 0)
        {
            return true;
        }
    }

    return false;
}

int wm_select_from_scan(wm_select_state_t *st, const wm_network_t *cand,
                        size_t cand_count,
                        const char (*present)[WM_SSID_LEN],
                        size_t present_count, uint32_t now_ms)
{
    /* 1. priority order among the visible entries with a clean record */
    for (size_t i = 0; i < cand_count; i++)
    {
        if (ssid_present(present, present_count, cand[i].ssid) &&
            !wm_select_is_deprioritised(st, (int)i, now_ms))
        {
            return (int)i;
        }
    }

    /* 2. every visible entry failed lately: keep trying them anyway,
     *    round-robin, so two entries sharing an SSID (different
     *    passwords) alternate instead of the first one hogging every
     *    attempt (meatpi 2026-09-06: our best chance is still a chance) */
    for (size_t n = 0; n < cand_count; n++)
    {
        st->dep_cursor = (st->dep_cursor + 1) % (int)cand_count;

        if (ssid_present(present, present_count, cand[st->dep_cursor].ssid))
        {
            return st->dep_cursor;
        }
    }

    return -1; /* nothing configured is visible */
}

int wm_select_sequential(wm_select_state_t *st, const wm_network_t *cand,
                         size_t cand_count, uint32_t now_ms)
{
    if (cand_count == 0)
    {
        return -1;
    }

    /* rotate, skipping entries that failed lately */
    for (size_t i = 0; i < cand_count; i++)
    {
        st->seq_cursor = (st->seq_cursor + 1) % (int)cand_count;

        if (!wm_select_is_deprioritised(st, st->seq_cursor, now_ms))
        {
            return st->seq_cursor;
        }
    }

    /* all of them did: rotate through all of them regardless */
    st->seq_cursor = (st->seq_cursor + 1) % (int)cand_count;
    return st->seq_cursor;
}

int wm_select_better(wm_select_state_t *st, const wm_network_t *cand,
                     size_t cand_count, int current_idx,
                     const char (*present)[WM_SSID_LEN],
                     size_t present_count, uint32_t now_ms)
{
    if (current_idx <= 0 || (size_t)current_idx >= cand_count)
    {
        return -1;              /* already on the primary (or invalid)   */
    }

    for (int i = 0; i < current_idx; i++)
    {
        if (!ssid_present(present, present_count, cand[i].ssid) ||
            wm_select_is_deprioritised(st, i, now_ms))
        {
            continue;
        }

        /* the same SSID as the working connection is the same AP with
           another password on file: leaving it gains nothing */
        if (strcmp(cand[i].ssid, cand[current_idx].ssid) == 0)
        {
            continue;
        }

        return i;
    }

    return -1;
}

bool wm_sta_pause_for_ap_clients(uint16_t ap_clients, bool ever_connected,
                                 uint32_t pauses_so_far,
                                 uint32_t max_pauses)
{
    if (ap_clients == 0)
    {
        return false;               /* nobody to protect                */
    }
    if (!ever_connected)
    {
        return false;               /* first uplink of the boot: go     */
    }

    return pauses_so_far < max_pauses;
}

int wm_backoff_skip_loops(int32_t retry_count)
{
    if (retry_count <= 2)
    {
        return 0;                       /* keep the candidate walk fast */
    }

    int shift = retry_count - 2;        /* 3rd failure -> 1 loop (10 s) */

    if (shift > 4)
    {
        shift = 4;
    }

    int skip = (1 << shift) - 1;        /* 1, 3, 7, 15 loops            */

    return (skip > WM_BACKOFF_MAX_SKIP_LOOPS) ? WM_BACKOFF_MAX_SKIP_LOOPS
                                              : skip;
}

/* "Pause VPN" per network (issue #976, 2026-10-11): the flag of the
 * connected candidate; nothing without a station session or for an index
 * outside the configured list. The settings parser collapses empty slots
 * before this sees the list, so a backup that moved up keeps its flag. */
bool wm_vpn_pause_applies(const bool *flags, size_t count, int idx)
{
    if (flags == NULL || idx < 0 || (size_t)idx >= count ||
        count > WM_MAX_CANDIDATES)
    {
        return false;
    }

    return flags[idx];
}

/* A network list as a sender's setting carries it (2026-11-08, the data
 * destinations and the Home Assistant webhook: "active on these WiFi
 * networks"): network names separated by commas, blanks around a comma
 * ignored, each name compared whole and case-sensitively (an SSID is
 * bytes, not words). An empty or missing list names nothing: the caller
 * decides what that means (the senders read it as "every network"). A
 * name that itself contains a comma cannot be listed: documented. */
bool wm_ssid_list_has(const char *list, const char *ssid)
{
    if (list == NULL || ssid == NULL || ssid[0] == '\0')
    {
        return false;
    }

    size_t want = strlen(ssid);
    const char *p = list;

    while (*p != '\0')
    {
        const char *end = strchr(p, ',');
        const char *stop = (end != NULL) ? end : p + strlen(p);
        const char *a = p;
        const char *b = stop;

        while (a < b && *a == ' ')
        {
            a++;
        }

        while (b > a && b[-1] == ' ')
        {
            b--;
        }

        if ((size_t)(b - a) == want && memcmp(a, ssid, want) == 0)
        {
            return true;
        }

        if (end == NULL)
        {
            break;
        }

        p = end + 1;
    }

    return false;
}
