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
 *        No IDF dependencies: compiled as-is by the host unit tests, which
 *        replay real chip logs re-split at every chunk boundary.
 */
#include "obd_chip_private.h"
#include "obd_chip_guard.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
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

        /* the prompt is '>' at LINE START only: response DATA may
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
            /* overflowed: buf no longer tracks line starts, take any
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

/**
 * A byte the chip never means: a CR/LF, or a byte outside printable ASCII.
 * The MIC3624's answer to a full reset (`ATZ`) begins with such bytes now
 * and then (bench 2026-11-02: `00`, `00 F0 FD 00 00`, `BF 80 04`,
 * `06 00 00 F8` ahead of the banner in 5 of 16 tries, the chip's UART
 * re-initialising through the reset); a NUL among them ended the C string
 * before "ELM" and the bring-up counted a chip that had reset fine as "not
 * confirmed". Skipped at the start of an answer and again behind the echo.
 */
static bool is_noise(unsigned char c)
{
    return c == '\r' || c == '\n' || c < 0x20 || c >= 0x7F;
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

    /* drop leading noise, then the command echo if present, then the
       noise a reset puts behind its echo (is_noise above) */
    while (start < end && is_noise((unsigned char)acc->buf[start]))
    {
        start++;
    }

    start += echo_length(acc->buf + start, end - start, cmd);

    while (start < end && is_noise((unsigned char)acc->buf[start]))
    {
        start++;
    }

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

bool obd_parse_is_ok(const char *resp)
{
    if (resp == NULL)
    {
        return false;
    }

    const char *p = resp;

    while (*p == '\r' || *p == '\n' || *p == ' ')
    {
        p++;
    }

    if (p[0] != 'O' || p[1] != 'K')
    {
        return false;
    }

    for (p += 2; *p != '\0'; p++)
    {
        if (*p != '\r' && *p != '\n' && *p != ' ')
        {
            return false;
        }
    }

    return true;
}

/* Monitor-class commands stream frames until ANY non-CR byte is sent (stop
 * with SPACE: CR would repeat the last command and can re-enter monitor
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

/* ---- protocol switches (issue #1023, bench 2026-11-01) ----------------------
 * The chip's memory function (ELM `AT M1` / `AT M0`) is ON after every reset
 * (the vendor's pin 51 strap; nothing persists the switch). With it on, a
 * protocol SWITCH that a request then answers on is written to the chip's
 * non-volatile protocol store, RAM-only `ATTP` or not, and the MIC3624's
 * EEPROM is rated for 10k cycles. The request engine sends `ATM0` ahead of
 * every word this names; the words themselves go out as they are. */

bool obd_parse_sets_protocol(const char *cmd)
{
    /* as the chip reads it: spaces dropped, any case, up to the CR (a ';'
       ends it too: the first element of a chain) */
    char c[10];
    size_t n = 0;

    if (cmd == NULL)
    {
        return false;
    }

    for (const char *p = cmd;
         *p != '\0' && *p != '\r' && *p != '\n' && *p != ';'; p++)
    {
        if (*p == ' ' || *p == '\t')
        {
            continue;
        }

        if (n >= sizeof(c) - 1)
        {
            return false; /* longer than any protocol switch */
        }

        c[n++] = (char)toupper((unsigned char)*p);
    }

    c[n] = '\0';

    const char *arg = NULL;
    size_t min_len = 1;
    size_t max_len = 2;

    if (n > 4 && c[0] == 'A' && c[1] == 'T' && (c[2] == 'S' || c[2] == 'T') &&
        c[3] == 'P')
    {
        arg = c + 4; /* ATSPh, ATTPh, ATSPAh, ATTPAh, ATSP00 (the search) */
    }
    else if (n > 3 && c[0] == 'S' && c[1] == 'T' && c[2] == 'P')
    {
        arg = c + 3; /* STPhh: the STN form, two digits (STPC/STPO are not) */
        min_len = 2;
    }
    else if (n > 3 && c[0] == 'V' && c[1] == 'T' && c[2] == 'P')
    {
        arg = c + 3; /* VTPhhh: the vendor form */
        max_len = 3;
    }

    if (arg == NULL || strlen(arg) < min_len || strlen(arg) > max_len)
    {
        return false;
    }

    for (const char *a = arg; *a != '\0'; a++)
    {
        if (!isxdigit((unsigned char)*a))
        {
            return false; /* STPR, STPBR, VTPRON, ATTPG: reads and junk */
        }
    }

    return true;
}

obd_reset_t obd_parse_reset_word(const char *buf, size_t len)
{
    char c[6];
    size_t n = 0;
    size_t i = 0;

    if (buf == NULL)
    {
        return OBD_RESET_NONE;
    }

    /* line ends and blanks ahead of the word */
    while (i < len && (buf[i] == '\r' || buf[i] == '\n' || buf[i] == ' ' ||
                       buf[i] == '\t'))
    {
        i++;
    }

    /* the word: blanks dropped, upper case, up to its line end */
    for (; i < len && buf[i] != '\r' && buf[i] != '\n'; i++)
    {
        if (buf[i] == ' ' || buf[i] == '\t')
        {
            continue;
        }

        if (n >= sizeof(c) - 1)
        {
            return OBD_RESET_NONE; /* longer than any reset word */
        }

        c[n++] = (char)toupper((unsigned char)buf[i]);
    }

    c[n] = '\0';

    /* nothing but line ends and blanks may follow: one command per chunk
       (a second command behind a reset is lost in the reset anyway) */
    for (; i < len; i++)
    {
        if (buf[i] != '\r' && buf[i] != '\n' && buf[i] != ' ' &&
            buf[i] != '\t')
        {
            return OBD_RESET_NONE;
        }
    }

    if (strcmp(c, "ATZ") == 0)
    {
        return OBD_RESET_ATZ;
    }

    if (strcmp(c, "ATWS") == 0)
    {
        return OBD_RESET_ATWS;
    }

    if (strcmp(c, "ATD") == 0)
    {
        return OBD_RESET_ATD; /* not ATD0 / ATD1 / ATDP / ATDPN */
    }

    return OBD_RESET_NONE;
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

/* ---- ATPPS (the programmable parameter summary) --------------------------------- */

bool obd_parse_pp(const char *atpps, uint8_t pp, uint8_t *value, bool *on)
{
    if (atpps == NULL || value == NULL || on == NULL)
    {
        return false;
    }

    char key[4];

    snprintf(key, sizeof(key), "%02X:", (unsigned)pp);

    for (const char *p = strstr(atpps, key); p != NULL; p = strstr(p + 1, key))
    {
        /* a token start ("0F:95 N"), not the tail of a longer one */
        if (p != atpps && !isspace((unsigned char)p[-1]))
        {
            continue;
        }

        const char *v = p + 3;

        if (!isxdigit((unsigned char)v[0]) || !isxdigit((unsigned char)v[1]))
        {
            continue;
        }

        const char *f = v + 2;

        while (*f == ' ')
        {
            f++;
        }

        if (*f != 'N' && *f != 'F')
        {
            continue;
        }

        char hex[3] = { v[0], v[1], '\0' };

        *value = (uint8_t)strtoul(hex, NULL, 16);
        *on = (*f == 'N');
        return true;
    }

    return false;
}
