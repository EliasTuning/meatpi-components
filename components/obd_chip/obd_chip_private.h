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
 * @file obd_chip_private.h
 * @brief Internals shared between the obd_chip sources. The parse section is
 *        PURE (no IDF deps) — the host unit tests compile obd_chip_parse.c
 *        directly against it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "obd_chip_guard.h" /* obd_guard_t for the guard accounting hook */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- pure parsing / framing (obd_chip_parse.c, host-testable) -------------- */

/* 4096 -> 16384 (2026-09-08): a 4 KB ISO-TP response (the chip's maximum,
 * "4K data = 8K ASCII") is 8.2 KB of hex with spaces off and ~12.5 KB with
 * spaces on plus `N:` line prefixes; the engine truncated it at 4096
 * ("response to '23230001' exceeded 4096 bytes"). Two accumulators live in
 * PSRAM (cmd engine + bare_probe), so the cost is 24 KB of PSRAM. */
#define OBD_RESP_MAX 16384

/** Response accumulator: feed RX bytes until the '>' prompt terminates it. */
typedef struct
{
    char   buf[OBD_RESP_MAX];
    size_t len;
    bool   done;     /**< prompt seen                                        */
    bool   overflow; /**< response exceeded OBD_RESP_MAX (kept truncated)    */
} obd_resp_acc_t;

void obd_parse_reset(obd_resp_acc_t *acc);

/**
 * Append @p n bytes; stops consuming at the first '>' (sets done). Returns
 * the number of bytes consumed (== n unless the prompt appeared earlier).
 * Bytes after the prompt belong to the next transaction/stream.
 */
size_t obd_parse_feed(obd_resp_acc_t *acc, const char *data, size_t n);

/**
 * Post-process a finished accumulator into a caller buffer: strip the echo
 * of @p cmd (chips boot with echo on), the terminating prompt, and leading/
 * trailing CR/LF. Returns the response length.
 */
size_t obd_parse_extract(const obd_resp_acc_t *acc, const char *cmd,
                         char *out, size_t out_len);

/** '?' alone / "UNABLE TO CONNECT" / "CAN ERROR"-style chip errors. */
bool obd_parse_is_chip_error(const char *resp);

/** Case/whitespace-insensitive prefix classification of monitor commands. */
bool obd_parse_is_monitor_cmd(const char *cmd);

/* ---- STSLCS sleep-config (obd_chip_stslcs.c, pure, host-testable) -----------
 * Ported from legacy main/obd.c — the sscanf patterns are the accepted
 * chip-output grammar. */

typedef struct
{
    char ctrl_mode[10]; /* "ELM327" or "NATIVE"                          */
    int  pwr_ctrl;      /* 1 = HIGH, 0 = LOW                             */
    struct { int en; uint32_t time; } uart_sleep;
    struct { int en; uint32_t min_time; uint32_t max_time; } uart_wake;
    int  ext_input_level;
    struct { int en; int level; uint32_t time; } ext_sleep;
    struct { int en; int level; uint32_t time; } ext_wake;
    struct { int en; float voltage; uint32_t time; } vl_sleep;
    struct { int en; float voltage; uint32_t time; } vl_wake;
    struct { int en; float voltage_change; uint32_t time; } vchg_wake;
} obd_stslcs_t;

/** Parse a full STSLCS response (any line endings) into @p out. */
void obd_stslcs_parse(const char *response, obd_stslcs_t *out);

/** The legacy reprogram condition: true when the chip's stored sleep
 *  config must be rewritten (autonomous controls on, or thresholds/time
 *  differ from settings). */
bool obd_stslcs_needs_provision(const obd_stslcs_t *c, float wake_v,
                                float sleep_v, uint32_t sleep_time_s);

/* Firmware-file line iterator (pure): yields trimmed non-empty lines. */
typedef struct
{
    const char *cur;
    const char *end;
} obd_fw_iter_t;

void   obd_fw_iter_init(obd_fw_iter_t *it, const char *data, size_t len);
size_t obd_fw_iter_next(obd_fw_iter_t *it, char *line, size_t line_len);
bool   obd_fw_line_is_end_marker(const char *line); /* "FFF1..." */

/* ---- UART layer (obd_chip_uart.c, target only) ------------------------------ */

esp_err_t obd_uart_init(int baud);
esp_err_t obd_uart_set_baud(int baud);
esp_err_t obd_uart_write(const uint8_t *data, size_t len);
int       obd_uart_read(uint8_t *buf, size_t len, uint32_t timeout_ms);
void      obd_uart_flush_input(void);
void      obd_uart_get_stats(uint32_t *tx_bytes, uint32_t *rx_overflows,
                             uint32_t *rx_buffered);
esp_err_t obd_uart_rx_task_start(void);
void      obd_uart_rx_task_stop(void);

/* pins */
void obd_pins_init(void);
void obd_pin_wake(void);       /* hold-release sequence, sleep pin high      */
void obd_pin_sleep(void);      /* low + pulldown + hold (survives resets)     */
void obd_pin_reset_pulse(void);/* RESET low 5 ms then high                    */
bool obd_pin_ready(void);

/* ---- core bridges ------------------------------------------------------------ */

/** Fan-out entry point called by the RX task with each chunk. */
void obd_core_fanout(const uint8_t *data, size_t len);

/** True while EXCLUSIVE is held (RX task pauses fan-out; TX rejects). */
bool obd_core_exclusive_held(void);

/* Async bring-up gate (2026-07-26): start() launches the wake/negotiate/
 * provision sequence on a one-shot task; wire-touching APIs wait here so
 * nothing interleaves with the bare-UART probes. Returns true when
 * bring-up has finished (chip up OR given up) or never launched
 * (degraded start — legacy pass-through behavior); false only if it is
 * still in flight after timeout_ms. */
#define OBD_BRINGUP_WAIT_MS 20000 /* > worst case: hard-reset path + full
                                     2-baud x 3-retry walk + provisioning */
bool obd_core_bringup_wait(uint32_t timeout_ms);

/* claim internals shared by request engine + fw update */
esp_err_t obd_core_claim(int type, uint32_t timeout_ms);
void      obd_core_release(void);

/** EEPROM guard accounting + log line (GET /api/obd_chip
 *  `eeprom_guard`): called by every TX path that ran the guard with a
 *  non-PASS verdict; @p cmd/@p len is the text as the caller received it. */
void obd_core_guard_note(obd_guard_t v, const char *cmd, size_t len);

/** obd_gate owner identity for the MIC chip (address only — defined in
 *  obd_chip.c, shared with the request engine in obd_chip_cmd.c). */
extern const char obd_chip_gate_owner[1];

/** Chip UART speed — FIXED, deliberately not a setting (meatpi 2026-09-07:
 *  exposing it invited a user changing it, breaking the chip and claiming
 *  warranty). The chip powers on at 115200 until `STWBR`/`ATPP 0F` persist
 *  this as its default; bring-up walks 2 M -> 115200 and switches (`STSBR`). */
#define OBD_CHIP_BAUD 2000000

/* config applied at boot by the settings descriptor */
typedef struct
{
    bool auto_sleep;
    bool monitor_auto_interrupt;
    bool auto_update;       /* flash the packaged fw on version mismatch
                               after bring-up (legacy parity, default on) */
    float    wake_voltage;  /* chip VL wake threshold (V)               */
    float    sleep_voltage; /* chip VL sleep threshold (V)              */
    uint32_t sleep_time_s;  /* chip VL sleep hold; minutes*60+30 legacy */
} obd_config_t;

const obd_config_t *obd_settings_config(void);
bool obd_settings_is_configured(void);
esp_err_t obd_settings_register(void);

/* request engine (obd_chip_cmd.c) */
esp_err_t obd_cmd_engine_init(void);

/* fw update helper (obd_chip_fw.c): VTVERS with update-mode semantics */
esp_err_t obd_chip_get_fw_version_raw(char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif

/* event_manager glue (obd_chip_events.c) */
void oc_events_register(void);
