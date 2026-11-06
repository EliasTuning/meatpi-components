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
 * @file data_logger_cursor.c
 * @brief The export route's pure rules (2026-11-06, store and forward):
 *        where in a stream's file list a GET /api/logger/export request
 *        starts for its `since` value, which lines of a file window go
 *        out (the name filter, the clock-step marker) and how a window is
 *        walked against the page's budgets. Host-tested with the file-name
 *        rules; data_logger_export.c applies them. Kept apart from
 *        data_logger_files.c, which sits at the 700-line rule.
 */
#include <string.h>

#include "data_logger_private.h"

int dl_export_start(const int64_t *epochs, int n, int64_t since_epoch,
                    bool exact, long *off)
{
    if (since_epoch <= 0 || n <= 0)
    {
        *off = 0;
        return 0;
    }

    if (exact)
    {
        int idx = 0;

        while (idx < n && epochs[idx] < since_epoch)
        {
            idx++;
        }

        if (idx >= n || epochs[idx] != since_epoch)
        {
            *off = 0; /* the file is gone (retired) or never existed */
        }

        return idx;
    }

    /* the newest file opened at or before the moment; the oldest when the
       moment predates every file (the rows the client asks for may sit in
       any of them) */
    int idx = 0;

    for (int i = 0; i < n; i++)
    {
        if (epochs[i] <= since_epoch)
        {
            idx = i;
        }
    }

    *off = 0;
    return idx;
}

/* ?name=<param> filter (dashboard history, 2026-09-06): a record's
 * "param":"<source>.<name>" matches on the full value or on the part
 * after its last '.', so the UI can ask by parameter name without knowing
 * the sink's source prefix. Plain substring scan: no JSON parse. */
bool dl_export_line_matches(const char *line, size_t len,
                                const char *name, bool csv)
{
    static const char KEY[] = "\"param\":\"";
    const size_t klen = sizeof(KEY) - 1;
    const char *p = NULL;
    const char *q = NULL;

    if (csv)
    {
        /* "ts_ms,source.name,value": the param is the second field (the
         * header row "ts_ms,param,value" never matches a real name) */
        p = memchr(line, ',', len);

        if (p == NULL)
        {
            return false;
        }

        p++;
        q = memchr(p, ',', len - (size_t)(p - line));
    }
    else
    {
        for (size_t i = 0; i + klen <= len; i++)
        {
            if (line[i] == '"' && memcmp(line + i, KEY, klen) == 0)
            {
                p = line + i + klen;
                break;
            }
        }

        if (p == NULL)
        {
            return false;
        }

        q = memchr(p, '"', len - (size_t)(p - line));
    }

    if (q == NULL)
    {
        return false;
    }

    size_t vlen = (size_t)(q - p);
    size_t nlen = strlen(name);

    if (vlen == nlen)
    {
        return memcmp(p, name, nlen) == 0;
    }

    return vlen > nlen && p[vlen - nlen - 1] == '.' &&
           memcmp(p + vlen - nlen, name, nlen) == 0;
}

/* the in-band clock-step marker (2026-10-13): not a record, never a point
 * for a consumer of this route (the journal and the file itself keep it) */
bool dl_export_line_is_marker(const char *line, size_t len, bool csv)
{
    static const char JSONL[] = "\"event\":\"clock_step\"";
    static const char CSV[] = ",logger.clock_step,";
    const char *key = csv ? CSV : JSONL;
    size_t klen = (csv ? sizeof(CSV) : sizeof(JSONL)) - 1;

    for (size_t i = 0; i + klen <= len; i++)
    {
        if (line[i] == key[0] && memcmp(line + i, key, klen) == 0)
        {
            return true;
        }
    }

    return false;
}

size_t dl_export_walk(const char *buf, size_t n, const char *name, bool csv,
                      int limit, int *lines_left, int *bytes_left,
                      dl_export_send_fn send, void *ctx, bool *stop,
                      bool *aborted)
{
    size_t consumed = 0;

    *stop = false;

    for (size_t i = 0; i < n && !*stop; i++)
    {
        if (buf[i] != '\n')
        {
            continue;
        }

        const char *line = buf + consumed;
        size_t len = i + 1 - consumed;

        if (!dl_export_line_is_marker(line, len, csv) &&
            (name[0] == '\0' || dl_export_line_matches(line, len, name, csv)))
        {
            /* the first line of a request always goes out, however small
               the budget; a later one that does not fit opens the next
               request instead (nothing consumed: the caller must not read
               that as a window without a line boundary, run 5) */
            if ((int)len > *bytes_left && *lines_left < limit)
            {
                *stop = true;
                break;
            }

            if (!send(ctx, line, len))
            {
                *aborted = true;
                *stop = true;
                break;
            }

            *bytes_left -= (int)len;
            (*lines_left)--;
        }

        consumed = i + 1;

        if (*lines_left == 0 || *bytes_left <= 0)
        {
            *stop = true;
        }
    }

    return consumed;
}
