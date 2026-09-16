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
 * @file obd_chip.h
 * @brief WiCAN OBD chip manager (feature component) — the single owner of the
 *        external OBD chip (ELM327-dialect MIC3624) on UART1.
 *
 * Ownership pattern (Architecture §2): nobody else touches the chip's UART,
 * sleep pin (GPIO9), status pin (GPIO7) or reset pin (GPIO41) — ever.
 *
 * ## RX fan-out (broadcast contract)
 * One RX task reads ALL chip output and copies each chunk into every
 * subscriber's queue. The manager does NO routing, filtering or parsing on
 * the hot path — every subscriber receives everything (other components'
 * responses, monitor-mode frames, unsolicited output) and is responsible for
 * keeping or ignoring it. A full subscriber queue never stalls the RX task:
 * the chunk is dropped for that subscriber and counted.
 *
 * ## Modal protocol + claims
 * The chip is modal: strict request->response (terminated by the '>' prompt)
 * vs monitor-class streaming commands (ATMA/ATMR/ATMT/STM..; stopped by
 * sending a SPACE — never CR, which repeats the last command). Arbitration
 * is a light claim model:
 *   - obd_chip_request() claims COMMAND internally for one transaction.
 *   - a bridge running a monitor command claims MONITOR; while held,
 *     request() fails fast with ESP_ERR_INVALID_STATE (v1 policy "manual" —
 *     the settings key `monitor_policy` reserves "auto_interrupt").
 *   - firmware update claims EXCLUSIVE; normal fan-out pauses.
 *
 * Configuration is a settings descriptor ("obd_chip"), reboot-to-apply.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/** RX fan-out chunk: small, copied by value into each subscriber's queue. */
#include "obd_chip_guard.h" /* the EEPROM guard (pure) every TX path runs */

#define OBD_CHIP_CHUNK_SIZE 128

typedef struct
{
    uint16_t len;
    uint8_t  data[OBD_CHIP_CHUNK_SIZE];
} obd_chunk_t;

typedef enum
{
    OBD_CHIP_CLAIM_COMMAND = 0, /**< one request->response transaction        */
    OBD_CHIP_CLAIM_MONITOR,     /**< streaming (ATMA-class) session           */
    OBD_CHIP_CLAIM_EXCLUSIVE,   /**< firmware update: everything else paused  */
} obd_claim_t;

/* ---- lifecycle (standard §3) ---------------------------------------------- */

/** Register descriptors, allocate state, install the UART driver + pins.
 *  No chip traffic. */
esp_err_t obd_chip_init(void);

/**
 * Launch the chip bring-up (wake, conditional reset, baud negotiation, AT
 * init, legacy provisioning, RX fan-out task) on a one-shot background task
 * and return immediately — the ~2 s sequence was half the device boot
 * (async since 2026-07-26). Wire-touching APIs (send/request/monitor_stop/
 * firmware_update) gate internally on bring-up completion, so consumers
 * need no ordering: early calls block briefly or time out cleanly, never
 * corrupt the negotiation. Poll obd_chip_ready() for the outcome.
 * ESP_ERR_INVALID_STATE when unconfigured (§4.3); a bring-up failure is
 * reported by the task's log line (device degraded, boot continues).
 */
esp_err_t obd_chip_start(void);

/** True once bring-up finished WITH the chip answering (RX fan-out live).
 *  False while bring-up is in flight — and after it gave up. */
bool obd_chip_ready(void);

/** Stop the RX task; leaves the chip awake. Waits out an in-flight
 *  bring-up first. */
esp_err_t obd_chip_stop(void);

/* ---- pub-sub RX -------------------------------------------------------------- */

/**
 * Register a subscriber queue (item size MUST be sizeof(obd_chunk_t); the
 * queue is owned by the caller). @p name is for diagnostics/drop reporting.
 */
esp_err_t obd_chip_subscribe(QueueHandle_t q, const char *name);
esp_err_t obd_chip_unsubscribe(QueueHandle_t q);

/** Chunks dropped for @p q because its queue was full. 0 if unknown queue. */
uint32_t obd_chip_dropped(QueueHandle_t q);

/* ---- TX (bridges / raw) -------------------------------------------------------- */

/**
 * Serialized raw write to the chip. Interleaved writers cannot corrupt each
 * other mid-buffer; a single 4 KB ISO-TP (VT) payload is one call. Blocked
 * while EXCLUSIVE is held (returns ESP_ERR_INVALID_STATE).
 */
esp_err_t obd_chip_send(const uint8_t *data, size_t len);

/* ---- external-client activity clock --------------------------------------------- */

/**
 * Mark "an external app just wrote to the chip". Called by the bridge
 * glue's obd endpoint on every send (TCP/BLE/USB/WS apps) - NOT by
 * obd_chip_send itself, because autopid's own monitor sends go through
 * that too and would pause autopid against itself. ESP-side pollers
 * (autopid) read obd_chip_client_idle_ms() and yield the chip while an
 * app is driving it (legacy DEV_AUTOPID_ELM327_APP_BIT parity).
 */
void obd_chip_client_touch(void);

/** Milliseconds since the last obd_chip_client_touch(); UINT32_MAX when
 *  no external client ever wrote. */
uint32_t obd_chip_client_idle_ms(void);

/* ---- request -> response --------------------------------------------------------- */

/**
 * Send @p cmd (a trailing CR is appended if missing) and collect the chip's
 * response up to the '>' prompt into @p resp (NUL-terminated, echo and
 * prompt stripped). Claims COMMAND internally: concurrent callers serialize;
 * fails fast with ESP_ERR_INVALID_STATE while MONITOR/EXCLUSIVE is held.
 * ESP_ERR_TIMEOUT if the prompt never arrives. Unsolicited traffic that
 * interleaves with the response is tolerated (it is part of the response
 * window by design — the caller sees exactly what the chip printed).
 */
esp_err_t obd_chip_request(const char *cmd, char *resp, size_t resp_len,
                           TickType_t timeout);

/* ---- arbitration ------------------------------------------------------------------- */

/**
 * Take a claim. COMMAND/MONITOR wait up to @p timeout for the chip to be
 * free; EXCLUSIVE additionally pauses fan-out once granted. Claims are not
 * nested/recursive: one release per successful claim, from any task.
 */
esp_err_t obd_chip_claim(obd_claim_t type, TickType_t timeout);
esp_err_t obd_chip_release(void);

/**
 * Hold the chip for a multi-command transaction from the CALLING task
 * (2026-09-16: the UDS AT transport sends 7 setup commands + the request;
 * autopid's poll used to land between them and its reply became the UDS
 * "response" — 1 of 3 requests survived). Takes the COMMAND claim; every
 * obd_chip_request() from this task then nests inside the hold while other
 * requesters wait (their normal claim timeout). One end() per begin();
 * end() drops the hold whatever the nesting depth. ESP_ERR_INVALID_STATE
 * when this task already holds a transaction or MONITOR/EXCLUSIVE is held,
 * ESP_ERR_TIMEOUT when the chip stayed busy for @p timeout.
 */
esp_err_t obd_chip_txn_begin(TickType_t timeout);
void      obd_chip_txn_end(void);

/** True if @p cmd is a monitor-class (streaming) command — table-driven,
 *  case/whitespace-insensitive (ATMA/ATMR/ATMT/STM/STMA…). Pure. */
bool obd_chip_is_monitor_cmd(const char *cmd);

/** Send the monitor stop byte (SPACE — never CR) outside a transaction. */
esp_err_t obd_chip_monitor_stop(void);

/* ---- chip management ----------------------------------------------------------------- */

/** Drive the sleep pin (true = sleep, with pulldown+hold so it survives
 *  resets; false = the wake release sequence). */
esp_err_t obd_chip_sleep(bool sleep);

/** Hardware reset pulse (RESET low 5 ms). A chip mid-monitor (ATMA)
 *  IGNORES the sleep pin — legacy's re-sleep path hard-reset first,
 *  then slept; without it the nap loop burns all its retries and falls
 *  back to a recovery reboot (sleep matrix `elm_monitor`, 2026-07-21). */
esp_err_t obd_chip_hard_reset(void);

/** READY status pin level (chip-driven). See README: on the current bench
 *  the pin reads low while the chip is demonstrably awake — treat the AT
 *  probe as the source of truth until the semantics are confirmed. */
bool obd_chip_status_ok(void);

/** Chip firmware version via VTVERS (e.g. "MIC3624 V2.3.22"). */
esp_err_t obd_chip_get_version(char *buf, size_t buf_len, TickType_t timeout);

/**
 * Update the chip firmware from a vendor .txt file on the filesystem
 * (logical path, e.g. "/data/obd_fw/V2.3.22.txt"). Claims EXCLUSIVE for the
 * whole procedure (minutes). Follows the verified legacy flow:
 * VTVERS check -> VTDLMIC3422 -> VTDLDT per line -> FFF1 -> VTDLED -> chip
 * hardware reset. `force` skips the version check.
 */
esp_err_t obd_chip_firmware_update(const char *fs_path, bool force);

/**
 * Same procedure from the PACKAGED image (EMBED_TXTFILES, currently
 * V2.3.22 — the auto-update source). The `auto_update` setting (default
 * true, legacy parity) runs this with force=false after every bring-up:
 * a chip already at the packaged version answers one VTVERS and skips;
 * a different version — or a chip stuck in download mode ('?') — is
 * flashed. A VTVERS TIMEOUT (dead wire) aborts unless forced.
 */
esp_err_t obd_chip_firmware_update_builtin(bool force);

/** The packaged image's version string ("V2.3.22"). */
const char *obd_chip_builtin_fw_version(void);

/* ---- observability ---------------------------------------------------------------- */

typedef struct
{
    bool        ready;
    const char *claim;          /* none | command | monitor | exclusive   */
    uint32_t    rx_bytes;       /* chip output fanned out (after chunking) */
    uint32_t    rx_chunks;
    uint16_t    rx_max_chunk;
    uint32_t    rx_overflows;   /* UART FIFO/ring overflows (input flushed) */
    uint32_t    rx_buffered;    /* bytes waiting in the driver ring now   */
    uint32_t    tx_bytes;       /* bytes written to the chip              */
    uint32_t    client_idle_ms; /* obd_chip_client_idle_ms()              */
    uint32_t    guard_rewrites; /* EEPROM guard: ATSP->ATTP / ATM1->ATM0  */
    uint32_t    guard_blocked;  /* EEPROM guard: ATPP/ATSD/ATCV/STWBR refused */
} obd_chip_stats_t;

typedef struct
{
    const char *name;
    uint32_t    dropped;        /* chunks lost because the queue was full */
    uint32_t    queued;         /* chunks waiting now                     */
    uint32_t    depth;
} obd_chip_sub_stats_t;

esp_err_t obd_chip_get_stats(obd_chip_stats_t *out);
size_t    obd_chip_get_subscribers(obd_chip_sub_stats_t *out, size_t max);

/** GET /api/obd_chip (own-routes pattern; HTTP compositions only). */
esp_err_t obd_chip_register_http(void);

#ifdef __cplusplus
}
#endif
