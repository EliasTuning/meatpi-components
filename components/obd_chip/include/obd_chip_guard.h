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
 * @file obd_chip_guard.h
 * @brief The chip's EEPROM guard (PURE, host-tested; implemented in
 *        obd_chip_parse.c). Dependency-free so autopid's config parser and
 *        the UDS transport can call it from their pure halves.
 *
 * The MIC3624 (ELM327 dialect) persists a handful of settings to EEPROM,
 * and EEPROM has a write budget. Init strings replay on every poll
 * transition, apps re-send their setup on every connect, and a terminal
 * user can type anything, so the wire is guarded at the driver (2026-09-16,
 * meatpi: "fix any OBD chip command that writes to eeprom"):
 *
 *   rewritten in place, same length, case/spacing kept:
 *     ATSP.. (set + SAVE protocol)     -> ATTP.. (try protocol, RAM only)
 *     ATM1   (memory on: every later
 *             protocol change sticks)  -> ATM0
 *   refused (no RAM-only twin; the raw path answers the ELM "?"):
 *     ATPP .. SV/ON/OFF  programmable parameters (0C/0F re-baud the UART:
 *                        a bricked link) — ATPPS (the summary READ) passes
 *     ATSD hh            store data byte
 *     ATCV dddd          voltage calibration
 *     STWBR              write UART baud rate (STN dialect)
 *     STSAVCAL           save calibration (STN dialect)
 *
 * A token counts only at a command boundary (buffer start, or after CR /
 * LF / space / tab / ';'), so "DATA", "ATSTFF" (the "ST" inside) and hex
 * payloads never match. Boot provisioning (obd_chip.c bare_probe: STSL*,
 * ATPP 0E/0F, STWBR, once, behind a matching check) and the firmware
 * update flow write the UART directly and are not guarded by design.
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    OBD_GUARD_PASS = 0,   /**< nothing EEPROM-related in the buffer          */
    OBD_GUARD_REWRITTEN,  /**< ATSP/ATM1 rewritten in place; send the buffer */
    OBD_GUARD_BLOCKED,    /**< an EEPROM write with no RAM twin: do not send */
} obd_guard_t;

/**
 * Scan @p len bytes of chip-bound text (a command, a ';'-separated init
 * chain, or a raw bridge chunk — NUL termination not required) and
 * rewrite ATSP -> ATTP / ATM1 -> ATM0 in place. Returns BLOCKED as soon as
 * a refused command is found (the buffer may be partially rewritten then).
 */
obd_guard_t obd_chip_guard_cmd(char *buf, size_t len);

/** The same scan without touching the buffer. */
obd_guard_t obd_chip_guard_check(const char *buf, size_t len);

#ifdef __cplusplus
}
#endif
