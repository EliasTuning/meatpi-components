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
 * @brief WiCAN OBD chip manager (feature component): the single owner of the
 *        external OBD chip (ELM327-dialect MIC3624) on UART1.
 *
 * Ownership pattern (Architecture §2): nobody else touches the chip's UART,
 * sleep pin (GPIO9), status pin (GPIO7) or reset pin (GPIO41), ever.
 *
 * ## RX fan-out (broadcast contract)
 * One RX task reads ALL chip output and copies each chunk into every
 * subscriber's queue. The manager does NO routing, filtering or parsing on
 * the hot path: every subscriber receives everything (other components'
 * responses, monitor-mode frames, unsolicited output) and is responsible for
 * keeping or ignoring it. A full subscriber queue never stalls the RX task:
 * the chunk is dropped for that subscriber and counted.
 *
 * ## Modal protocol + claims
 * The chip is modal: strict request->response (terminated by the '>' prompt)
 * vs monitor-class streaming commands (ATMA/ATMR/ATMT/STM..; stopped by
 * sending a SPACE: never CR, which repeats the last command). Arbitration
 * is a light claim model:
 *   - obd_chip_request() claims COMMAND internally for one transaction.
 *   - a bridge running a monitor command claims MONITOR; while held,
 *     request() fails fast with ESP_ERR_INVALID_STATE (v1 policy "manual":
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
 * and return immediately: the ~2 s sequence was half the device boot
 * (async since 2026-07-26). Wire-touching APIs (send/request/monitor_stop/
 * firmware_update) gate internally on bring-up completion, so consumers
 * need no ordering: early calls block briefly or time out cleanly, never
 * corrupt the negotiation. Poll obd_chip_ready() for the outcome.
 * ESP_ERR_INVALID_STATE when unconfigured (§4.3); a bring-up failure is
 * reported by the task's log line (device degraded, boot continues).
 */
esp_err_t obd_chip_start(void);

/** True once bring-up finished WITH the chip answering (RX fan-out live).
 *  False while bring-up is in flight, and after it gave up. */
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
 * while EXCLUSIVE is held (returns ESP_ERR_INVALID_STATE). The EEPROM guard
 * runs on the bytes (obd_chip_guard.h), and a chunk that is exactly an
 * app's reset word (`ATZ`, `ATWS`, `ATD`; issue #1023, 2026-11-01) goes
 * out as a warm start (`ATZ` as `ATWS`: the same banner without the 1 s
 * LED test) with an `ATM0` written behind its answer and that answer kept
 * from every subscriber: the app sees exactly the reply it asked for, and
 * the chip's memory function is off for the rest of the session.
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

/** The chip's run-time settings generation (2026-10-28): a number that
 *  moves whenever the chip's volatile settings (echo, headers, protocol,
 *  timeouts, filters) are no longer what a holder last set them to: at
 *  every bring-up (boot, a reflash's fresh start, a recovery) and when a
 *  holder that changed them says so with obd_chip_settings_changed() at
 *  the end of its transaction (the CAN self-test's ATD + ATE0). AutoPID
 *  re-sends its prelude when the number moved; any holder that keeps chip
 *  state should do the same. */
uint32_t obd_chip_settings_generation(void);
void     obd_chip_settings_changed(void);

/* ---- request -> response --------------------------------------------------------- */

/**
 * Send @p cmd (a trailing CR is appended if missing) and collect the chip's
 * response up to the '>' prompt into @p resp (NUL-terminated, echo and
 * prompt stripped). Claims COMMAND internally: concurrent callers serialize;
 * fails fast with ESP_ERR_INVALID_STATE while MONITOR/EXCLUSIVE is held.
 * ESP_ERR_TIMEOUT if the prompt never arrives. Unsolicited traffic that
 * interleaves with the response is tolerated (it is part of the response
 * window by design: the caller sees exactly what the chip printed).
 *
 * A protocol switch (`ATTP`, the `ATSP` the guard rewrites, STN `STP`,
 * vendor `VTP`) is preceded by an `ATM0` transaction of its own (issue
 * #1023, bench 2026-11-01): the chip's memory function is ON after every
 * reset and writes the switch into its EEPROM-backed protocol store as soon
 * as a request answers on the new protocol; a profile that flips protocols
 * between rows wrote it on every poll. The pair runs inside one transaction
 * hold when the caller holds none. The raw bridge path (obd_chip_send)
 * covers its side differently: an app's ATZ / ATWS / ATD is answered as a
 * warm start (ATZ sent as ATWS) with an ATM0 behind it whose answer the
 * fan-out hides, so an app's session runs with the memory function off too
 * and nothing it sends is answered twice. Counted in
 * obd_chip_stats_t.memory_off and .app_resets.
 */
esp_err_t obd_chip_request(const char *cmd, char *resp, size_t resp_len,
                           TickType_t timeout);

/**
 * Teach the chip its BASE protocol once: a REAL `ATSP<proto>` (set AND
 * save to EEPROM) on the same serialized transaction path as
 * obd_chip_request(), bypassing the EEPROM guard that rewrites every
 * other ATSP to ATTP (2026-10-01, TASK_quick_setup.md "Protocol policy").
 * Why it exists: with the guard alone the chip would power up on
 * whatever its EEPROM held from the factory or an old app and search
 * every boot; one save after a successful vehicle detection makes the
 * stored protocol the right one, while per-PID protocol switches in
 * profile init chains stay RAM-only (ATTP). Who may call it: autopid's
 * vehicle detection job ONLY, when the detected base protocol differs
 * from the car record's `chip_protocol`. Rules enforced here: @p proto
 * must be '1'..'9' or 'A'..'C' (ESP_ERR_INVALID_ARG; never ATSP0), at
 * most ONE save per boot (ESP_ERR_INVALID_STATE afterwards), the chip
 * must answer `OK` (ESP_FAIL otherwise; ESP_ERR_TIMEOUT on silence).
 * Counted in obd_chip_stats_t.protocol_saves and `GET /api/obd_chip`
 * `eeprom_guard.protocol_saves`.
 */
esp_err_t obd_chip_protocol_save(char proto);

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
 * "response": 1 of 3 requests survived). Takes the COMMAND claim; every
 * obd_chip_request() from this task then nests inside the hold while other
 * requesters wait (their normal claim timeout). One end() per begin();
 * end() drops the hold whatever the nesting depth. ESP_ERR_INVALID_STATE
 * when this task already holds a transaction or MONITOR/EXCLUSIVE is held,
 * ESP_ERR_TIMEOUT when the chip stayed busy for @p timeout.
 */
esp_err_t obd_chip_txn_begin(TickType_t timeout);
void      obd_chip_txn_end(void);

/** True if @p cmd is a monitor-class (streaming) command: table-driven,
 *  case/whitespace-insensitive (ATMA/ATMR/ATMT/STM/STMA…). Pure. */
bool obd_chip_is_monitor_cmd(const char *cmd);

/** Send the monitor stop byte (SPACE: never CR) outside a transaction. */
esp_err_t obd_chip_monitor_stop(void);

/* ---- chip management ----------------------------------------------------------------- */

/** Drive the sleep pin (true = sleep, with pulldown+hold so it survives
 *  resets; false = the wake release sequence). */
esp_err_t obd_chip_sleep(bool sleep);

/** Leave the chip asleep from a boot that never initialised this component
 *  (the composition root's crash park). Pins only, no init needed. A chip
 *  that is asleep already (READY high: the boot follows a sleep and the
 *  sleep pin's hold is still on) is left asleep, its sleep pin never driven
 *  high. A chip that is awake (a power-on, or the run before had it up) is
 *  hardware-reset first, so that a session or a monitor left open is closed
 *  and the sleep pin is obeyed, then put to sleep with the hold: about
 *  0.6 s. Either way the next normal boot finds it asleep and wakes it as
 *  after any sleep. */
esp_err_t obd_chip_park(void);

/** Hardware reset pulse (RESET low 5 ms). A chip mid-monitor (ATMA)
 *  IGNORES the sleep pin: legacy's re-sleep path hard-reset first,
 *  then slept; without it the nap loop burns all its retries and falls
 *  back to a recovery reboot (sleep matrix `elm_monitor`, 2026-07-21). */
esp_err_t obd_chip_hard_reset(void);

/** READY status pin level (chip-driven). See README: on the current bench
 *  the pin reads low while the chip is demonstrably awake, treat the AT
 *  probe as the source of truth until the semantics are confirmed. */
bool obd_chip_status_ok(void);

/** Chip firmware version via VTVERS (e.g. "MIC3624 V2.3.22"). */
esp_err_t obd_chip_get_version(char *buf, size_t buf_len, TickType_t timeout);

/**
 * Which chip an update leaves alone (2026-11-01, Ali: no forced rewrite
 * anywhere). Under either rule a chip at the image's version or above it
 * is never written, a chip stuck in download mode ('?', or bytes without
 * a prompt at either baud) is recovered by completing the download, and a
 * wire silent at both bauds aborts.
 */
typedef enum
{
    /** The bring-up's automatic update: a chip on an accepted version
     *  (V2.3.22 or V2.3.24, the v4.52p list) is left alone and a chip
     *  below the list gets the packaged image. A chip left on V2.3.22 is
     *  flagged (`fw.outdated`) and waits for the user's update. */
    OBD_FW_RULE_STARTUP,
    /** The user's update (the Maintenance card, the API, the console):
     *  written only when the image's version is above the chip's. */
    OBD_FW_RULE_NEWER,
} obd_fw_rule_t;

/**
 * Update the chip firmware from a vendor .txt file on the filesystem
 * (logical path, e.g. "/data/obd_fw/V2.3.24.txt": the file's NAME carries
 * the image's version, ESP_ERR_INVALID_ARG without one). The user's rule
 * (OBD_FW_RULE_NEWER): the chip is written only when that version is
 * above the one it reports. Claims EXCLUSIVE for the whole procedure
 * (about 20 s). Follows the verified legacy flow: VTVERS check ->
 * VTDLMIC3422 -> VTDLDT per line -> FFF1 -> VTDLED -> chip hardware reset.
 */
esp_err_t obd_chip_firmware_update(const char *fs_path);

/**
 * Same procedure from the PACKAGED image (EMBED_TXTFILES, V2.3.24 since
 * 2026-10-28). The `auto_update` setting (default true, legacy parity)
 * runs this with OBD_FW_RULE_STARTUP after every bring-up: a chip at a
 * CURRENT version (V2.3.22 or V2.3.24, the v4.52p policy: v6 used to
 * downgrade a .24 chip to .22) answers one VTVERS and skips; an older
 * version, or a chip stuck in download mode: is flashed. The user's
 * entries run it with OBD_FW_RULE_NEWER (issue #1023).
 */
esp_err_t obd_chip_firmware_update_builtin(obd_fw_rule_t rule);

/** The packaged image's version string ("V2.3.24"). */
const char *obd_chip_builtin_fw_version(void);

/** True while a chip firmware update owns the wire (EXCLUSIVE, ~90 s).
 *  The sleep entry waits for it: the chip's sleep pin pulled under a
 *  half-written image strands it in its bootloader (issue #1023). */
bool obd_chip_update_in_progress(void);

typedef struct
{
    bool        running;
    uint32_t    lines;     /* records sent so far                         */
    uint32_t    total;     /* records in the image                        */
    const char *source;    /* "builtin V2.3.24" or the file path          */
    bool        done_once; /* a run finished since boot (last_err valid)  */
    esp_err_t   last_err;
    const char *chip;      /* the chip's VTVERS as last read ("" = none)  */
} obd_chip_fw_status_t;

void obd_chip_fw_status(obd_chip_fw_status_t *out);

/** Run the user's update (OBD_FW_RULE_NEWER) on its own task (the console
 *  / HTTP entry, 2026-10-28): @p fs_path NULL = the packaged image, else a
 *  file whose name carries its version (ESP_ERR_INVALID_ARG without one).
 *  ESP_ERR_INVALID_STATE while one runs. A reflashed chip comes back
 *  through a fresh bring-up afterwards. */
esp_err_t obd_chip_firmware_update_start(const char *fs_path);

/** Bare-wire diagnostic (the `obd_chip probe` command, issue #1023): send
 *  @p cmd (with its CR) at @p baud (0 = as is) and collect what comes back
 *  within @p timeout_ms, prompt or not. @p rx_got counts every byte, the
 *  first @p rx_len are stored. EXCLUSIVE for the duration (the RX fan-out
 *  pauses); the UART returns to the baud it had. */
esp_err_t obd_chip_probe(const char *cmd, int baud, uint32_t timeout_ms,
                         uint8_t *rx, size_t rx_len, size_t *rx_got,
                         bool *prompt);

/** The baud the chip answered at during bring-up (0 before, or never). */
int obd_chip_active_baud(void);

/** Baud detection by the UART hardware (issue #1023, 2026-10-28): the
 *  shortest low / high pulse and edge intervals seen on RX over a window,
 *  in ticks of @p clk_hz, and the bauds they imply (IDF's formula
 *  clk * 2 / period). 0 edges = the chip sent nothing. */
typedef struct
{
    uint32_t edges;    /* RX edges seen (a 10-bit counter)            */
    uint32_t clk_hz;   /* the tick the periods count in               */
    uint32_t low;      /* shortest low pulse, ticks                   */
    uint32_t high;     /* shortest high pulse, ticks                  */
    uint32_t pos;      /* shortest positive-edge interval, ticks      */
    uint32_t neg;      /* shortest negative-edge interval, ticks      */
    int      baud_lh;  /* from low + high (the ideal-signal formula)  */
    int      baud_pos; /* from the positive edges                     */
    int      baud_neg; /* from the negative edges                     */
} obd_chip_autobaud_t;

/** Measure what the chip talks at: @p stimulus (may be NULL) is sent at the
 *  current UART baud when the window opens (a quiet chip answers it; a chip
 *  talking on its own needs none), the detector runs for @p window_ms.
 *  EXCLUSIVE meanwhile; the UART's baud is not changed. */
esp_err_t obd_chip_autobaud(const char *stimulus, uint32_t window_ms,
                            obd_chip_autobaud_t *out);

/* ---- the wire tools as one runner (obd_chip_probe.c, 2026-10-28) ------------
 * The console command and the HTTP route share it: the console runs it in
 * place and prints, the route starts it as a job and serves the result. */

typedef enum
{
    OBD_PROBE_PROBE = 0, /* ATI + VTVERS at 2 Mbaud and 115200 (or one baud) */
    OBD_PROBE_SCAN,      /* ATI at every standard baud                         */
    OBD_PROBE_AUTO,      /* the hardware detector, then a listen where it says */
    OBD_PROBE_LISTEN,    /* send nothing for N s at a baud                      */
} obd_probe_kind_t;

#define OBD_PROBE_TEXT  32
#define OBD_PROBE_LINES 16
#define OBD_PROBE_RAW   512
#define OBD_PROBE_CMD   16 /* a stimulus, NUL included (VTDLMIC3422) */

typedef struct
{
    int      baud;
    char     cmd[OBD_PROBE_CMD];         /* "ATI", "VTVERS", "" = a listen  */
    bool     prompt;
    uint32_t bytes;                      /* every byte received             */
    char     text[OBD_PROBE_TEXT + 1];   /* the first of them, printable    */
    char     hex[3 * OBD_PROBE_TEXT + 1];
} obd_probe_line_t;

typedef struct
{
    bool             running;
    bool             done;      /* a run finished since boot (err valid) */
    obd_probe_kind_t kind;
    int              baud;      /* the request's baud, 0 = the default   */
    int              seconds;   /* the listen / detection window         */
    char             cmd[OBD_PROBE_CMD]; /* the stimulus asked for, "" = ATI */
    esp_err_t        err;
    size_t           n_lines;
    obd_probe_line_t lines[OBD_PROBE_LINES];
    uint32_t         edges;     /* auto: RX edges seen                   */
    int              measured;  /* auto: the bit time's baud, 0 = none   */
    int              detected;  /* auto: the nearest standard baud, 0    */
    uint8_t          raw[OBD_PROBE_RAW]; /* a listen's bytes             */
    size_t           raw_len;   /* stored                                */
    uint32_t         raw_total; /* received                              */
    int              raw_baud;
    bool             raw_prompt;
    int64_t          at_us;     /* when the run started                  */
} obd_probe_result_t;

/** Run one tool in the caller's task (seconds: listen / auto window, 1..60;
 *  baud 0 = the tool's default). Holds the chip EXCLUSIVE meanwhile. */
esp_err_t obd_chip_probe_run(obd_probe_kind_t kind, int baud, int seconds,
                             const char *cmd, obd_probe_result_t *out);

/** The same on a one-shot task (the HTTP route); ESP_ERR_INVALID_STATE
 *  while a run or a firmware update is in flight. */
esp_err_t obd_chip_probe_start(obd_probe_kind_t kind, int baud, int seconds,
                               const char *cmd);

/** The job's result (a copy), running or done. */
void obd_chip_probe_last(obd_probe_result_t *out);

/** The standard bauds the scan walks and the detector names. */
const int *obd_chip_probe_bauds(size_t *count);

/** What the last bring-up saw at each baud it tried and got no prompt from
 *  (issue #1023): up to two notes, none when the chip answered at once. */
typedef struct
{
    int      baud;
    uint32_t bytes;
    bool     prompt;
    char     hex[3 * 12 + 1];
} obd_wire_note_t;

size_t obd_chip_bringup_wire(obd_wire_note_t *out, size_t max);

/* ---- the chip configuration report (obd_chip_report.c, 2026-10-28) -----------
 * The read-only commands that show what the chip is and how it is set: ATI,
 * AT@1, AT@2, VTVERS, ATRV, ATDPN, ATDP, ATCS, ATPPS (the programmable
 * parameters: the EEPROM-backed settings the firmware provisions), STSLCS
 * (sleep and wake), VTPPSWS (the power pin switch). One block a user copies
 * into a bug report. Needs the chip up; about 1.5 s inside one transaction. */

#define OBD_REPORT_ITEMS 11

typedef struct
{
    char      cmd[12];
    char      what[36];
    char      resp[512];  /* the chip's lines, '\n' separated, prompt gone
                             (ATPPS alone is 12 lines of 32 parameters)   */
    esp_err_t err;        /* the request's verdict (a timeout keeps resp "") */
} obd_chip_report_item_t;

typedef struct
{
    size_t                 n;
    obd_chip_report_item_t items[OBD_REPORT_ITEMS];
    int64_t                at_us;
} obd_chip_report_t;

/** ESP_ERR_INVALID_STATE when the chip is down or an update runs. */
esp_err_t obd_chip_report_run(obd_chip_report_t *out);

/** `obd_chip` CLI; registered internally on the settings apply (§6b). */
esp_err_t obd_chip_register_cli(void);

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
    uint32_t    protocol_saves; /* obd_chip_protocol_save(): real ATSP
                                   writes this boot (0 or 1)             */
    uint32_t    memory_off;     /* ATM0 the driver sent and the chip took:
                                   once per bring-up, once ahead of every
                                   protocol switch on the request path,
                                   once behind every app reset below    */
    uint32_t    app_resets;     /* an app's ATZ / ATWS / ATD on the bridge
                                   path: ATZ sent as ATWS, a hidden ATM0
                                   behind the answer                     */
    uint32_t    hidden_bytes;   /* those ATM0 answers, kept from the apps */
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
