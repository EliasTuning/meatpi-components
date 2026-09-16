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
 * @file obd_chip_parse.c
 * @brief Pure response framing / classification for the ELM327-dialect chip.
 *        No IDF dependencies — compiled as-is by the host unit tests, which
 *        replay real chip logs re-split at every chunk boundary.
 */
#include "obd_chip_private.h"
#include "obd_chip_guard.h"

#include <ctype.h>
#include <string.h>

void obd_parse_reset(obd_resp_acc_t *acc)
{
    acc->len = 0;
    acc->done = false;
    acc->overflow = false;
    acc->buf[0] = '\0';
}

size_t obd_parse_feed(obd_resp_acc_t *acc, const char *data, size_t n)
{
    size_t consumed = 0;

    if (acc->done)
    {
        return 0; /* finished transactions consume nothing */
    }

    while (consumed < n)
    {
        char c = data[consumed++];

        /* the prompt is '>' at LINE START only — response DATA may
           contain '>' mid-line (STSLCS: "VL_WAKE: OFF, >13.50V ..."
           truncated here live until this check existed) */
        bool at_line_start =
            acc->len == 0 || acc->buf[acc->len - 1] == '\r' ||
            acc->buf[acc->len - 1] == '\n';

        if (acc->len < OBD_RESP_MAX - 1)
        {
            acc->buf[acc->len++] = c;
        }
        else
        {
            acc->overflow = true; /* keep consuming to find the prompt */
        }

        if (c == '>' && at_line_start && !acc->overflow)
        {
            acc->done = true;
            break;
        }

        if (c == '>' && acc->overflow)
        {
            /* overflowed: buf no longer tracks line starts — take any
               prompt rather than hang (truncated flag is already set) */
            acc->done = true;
            break;
        }
    }

    acc->buf[acc->len] = '\0';
    return consumed;
}

/** Compare ignoring case; @p cmd may or may not carry its trailing CR. */
static size_t echo_length(const char *buf, size_t len, const char *cmd)
{
    if (cmd == NULL)
    {
        return 0;
    }

    size_t cmd_len = strlen(cmd);

    while (cmd_len > 0 && (cmd[cmd_len - 1] == '\r' || cmd[cmd_len - 1] == '\n'))
    {
        cmd_len--;
    }

    if (cmd_len == 0 || len < cmd_len)
    {
        return 0;
    }

    for (size_t i = 0; i < cmd_len; i++)
    {
        if (toupper((unsigned char)buf[i]) != toupper((unsigned char)cmd[i]))
        {
            return 0;
        }
    }

    /* echo is followed by the chip's CR (possibly LF) */
    size_t e = cmd_len;

    while (e < len && (buf[e] == '\r' || buf[e] == '\n'))
    {
        e++;
    }

    return e;
}

size_t obd_parse_extract(const obd_resp_acc_t *acc, const char *cmd,
                         char *out, size_t out_len)
{
    if (out == NULL || out_len == 0)
    {
        return 0;
    }

    size_t start = 0;
    size_t end = acc->len;

    /* drop leading noise CR/LF, then the command echo if present */
    while (start < end && (acc->buf[start] == '\r' || acc->buf[start] == '\n'))
    {
        start++;
    }

    start += echo_length(acc->buf + start, end - start, cmd);

    /* drop the terminating prompt and trailing whitespace */
    if (end > start && acc->buf[end - 1] == '>')
    {
        end--;
    }

    while (end > start && (acc->buf[end - 1] == '\r' ||
                           acc->buf[end - 1] == '\n' ||
                           acc->buf[end - 1] == ' '))
    {
        end--;
    }

    size_t n = end - start;

    if (n >= out_len)
    {
        n = out_len - 1;
    }

    memcpy(out, acc->buf + start, n);
    out[n] = '\0';
    return n;
}

bool obd_parse_is_chip_error(const char *resp)
{
    if (resp == NULL)
    {
        return true;
    }

    /* a lone '?' is the chip's unknown-command marker */
    if (resp[0] == '?' && (resp[1] == '\0' || resp[1] == '\r'))
    {
        return true;
    }

    return strstr(resp, "UNABLE TO CONNECT") != NULL ||
           strstr(resp, "CAN ERROR") != NULL ||
           strstr(resp, "BUS ERROR") != NULL ||
           strstr(resp, "DATA ERROR") != NULL ||
           strstr(resp, "BUFFER FULL") != NULL;
}

/* Monitor-class commands stream frames until ANY non-CR byte is sent (stop
 * with SPACE — CR would repeat the last command and can re-enter monitor
 * mode). Table pending meatpi's authoritative list (task §11); extend here. */
static const char *const MONITOR_CMDS[] =
{
    "ATMA", "ATMR", "ATMT", "STMA", "STM",
};

bool obd_parse_is_monitor_cmd(const char *cmd)
{
    if (cmd == NULL)
    {
        return false;
    }

    /* normalize: strip spaces, uppercase, ignore trailing CR and arguments */
    char norm[16];
    size_t n = 0;

    for (const char *p = cmd; *p != '\0' && n < sizeof(norm) - 1; p++)
    {
        if (*p == ' ' || *p == '\t')
        {
            continue;
        }

        if (*p == '\r' || *p == '\n')
        {
            break;
        }

        norm[n++] = (char)toupper((unsigned char)*p);
    }

    norm[n] = '\0';

    for (size_t i = 0; i < sizeof(MONITOR_CMDS) / sizeof(MONITOR_CMDS[0]); i++)
    {
        size_t mlen = strlen(MONITOR_CMDS[i]);

        if (strncmp(norm, MONITOR_CMDS[i], mlen) == 0)
        {
            /* "STM" must not match "STMFR"-style unrelated cmds: accept only
               exact or followed by hex args (monitor filters) */
            char next = norm[mlen];

            if (next == '\0' || isxdigit((unsigned char)next))
            {
                return true;
            }
        }
    }

    return false;
}

/* ---- firmware file iteration ------------------------------------------------ */

void obd_fw_iter_init(obd_fw_iter_t *it, const char *data, size_t len)
{
    it->cur = data;
    it->end = data + len;
}

size_t obd_fw_iter_next(obd_fw_iter_t *it, char *line, size_t line_len)
{
    while (it->cur < it->end)
    {
        size_t n = 0;

        while (it->cur < it->end && *it->cur != '\n')
        {
            if (n < line_len - 1 && *it->cur != '\r')
            {
                line[n++] = *it->cur;
            }

            it->cur++;
        }

        if (it->cur < it->end)
        {
            it->cur++; /* skip the newline */
        }

        line[n] = '\0';

        if (n > 0)
        {
            return n; /* skip empty lines silently */
        }
    }

    return 0;
}

bool obd_fw_line_is_end_marker(const char *line)
{
    return strncmp(line, "FFF1", 4) == 0;
}

/* ---- EEPROM guard (obd_chip_guard.h, 2026-09-16) --------------------------
 * One scan serves three callers: autopid's config parser (init strings,
 * ';'-separated), the request engine (one command) and the raw bridge path
 * (an app's or terminal's bytes, no NUL). Rewrites keep the length so a
 * raw chunk can be patched in place. */

static bool guard_boundary(const char *buf, size_t i)
{
    if (i == 0)
    {
        return true;
    }

    char p = buf[i - 1];

    return p == '\r' || p == '\n' || p == ' ' || p == '\t' || p == ';';
}

static size_t guard_skip_ws(const char *buf, size_t len, size_t i)
{
    while (i < len && (buf[i] == ' ' || buf[i] == '\t'))
    {
        i++;
    }

    return i;
}

static char guard_lc(char c)
{
    return (char)tolower((unsigned char)c);
}

static obd_guard_t guard_scan(char *buf, size_t len, bool apply)
{
    obd_guard_t verdict = OBD_GUARD_PASS;

    if (buf == NULL)
    {
        return verdict;
    }

    for (size_t i = 0; i + 1 < len; i++)
    {
        char c0 = guard_lc(buf[i]);
        char c1 = guard_lc(buf[i + 1]);
        bool at = (c0 == 'a' && c1 == 't');
        bool st = (c0 == 's' && c1 == 't');

        if ((!at && !st) || !guard_boundary(buf, i))
        {
            continue; /* "DATA", the "ST" inside "ATSTFF", hex payloads */
        }

        size_t j = guard_skip_ws(buf, len, i + 2);

        if (j >= len)
        {
            break;
        }

        if (at)
        {
            char l1 = guard_lc(buf[j]);
            size_t k = guard_skip_ws(buf, len, j + 1);
            char l2 = (k < len) ? guard_lc(buf[k]) : '\0';

            if (l1 == 's' && l2 == 'p')
            {
                /* ATSP (set + save protocol) -> ATTP (try protocol, RAM) */
                if (apply)
                {
                    buf[i] = 'A';
                    buf[i + 1] = 'T';
                    buf[j] = 'T';
                    buf[k] = 'P';
                }

                verdict = OBD_GUARD_REWRITTEN;
                i = k;
            }
            else if (l1 == 'm' && l2 == '1')
            {
                /* ATM1 (memory on: later protocol changes stick) -> ATM0;
                   ATMA/ATMR/ATMT are monitors, not memory (no digit) */
                if (apply)
                {
                    buf[i] = 'A';
                    buf[i + 1] = 'T';
                    buf[j] = 'M';
                    buf[k] = '0';
                }

                verdict = OBD_GUARD_REWRITTEN;
                i = k;
            }
            else if (l1 == 'p' && l2 == 'p')
            {
                /* ATPP xx SV yy / ON / OFF write programmable parameters
                   (0C/0F re-baud the UART); only ATPPS, the summary READ,
                   is harmless */
                size_t m = guard_skip_ws(buf, len, k + 1);
                char l3 = (m < len) ? guard_lc(buf[m]) : '\0';

                if (l3 != 's')
                {
                    return OBD_GUARD_BLOCKED;
                }

                i = m;
            }
            else if ((l1 == 's' && l2 == 'd') || (l1 == 'c' && l2 == 'v'))
            {
                /* ATSD hh (store data byte), ATCV dddd (voltage calibration) */
                return OBD_GUARD_BLOCKED;
            }
        }
        else
        {
            /* STN dialect: STWBR (write UART baud), STSAVCAL (save
               calibration). STSBR (set baud, RAM), STSLCS (read) pass. */
            char word[9];
            size_t n = 0;
            size_t p = j;

            while (p < len && n < sizeof(word) - 1 &&
                   isalpha((unsigned char)buf[p]))
            {
                word[n++] = (char)toupper((unsigned char)buf[p]);
                p++;
            }

            word[n] = '\0';

            if (strcmp(word, "WBR") == 0 || strcmp(word, "SAVCAL") == 0)
            {
                return OBD_GUARD_BLOCKED;
            }
        }
    }

    return verdict;
}

obd_guard_t obd_chip_guard_cmd(char *buf, size_t len)
{
    return guard_scan(buf, len, true);
}

obd_guard_t obd_chip_guard_check(const char *buf, size_t len)
{
    /* apply=false never writes; the cast only satisfies the shared scanner */
    return guard_scan((char *)buf, len, false);
}
