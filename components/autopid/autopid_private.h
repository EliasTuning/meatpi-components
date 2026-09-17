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
 * @file autopid_private.h
 * @brief Internal types + pure-module contracts shared by the autopid
 *        .c files and the host suite. Not part of the public API.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- pool bounds (generous by design — meatpi 2026-07-06: PSRAM is
        plentiful; caps exist for static allocation, not rationing) -------- */
#define AP_MAX_GROUPS   32
#define AP_MAX_PIDS     512
#define AP_MAX_FILTERS  128
#define AP_MAX_PARAMS   2048   /* pooled across all PIDs + filters          */
#define AP_PARAMS_PER   256    /* per PID/filter — published vehicle
                                  profiles decode up to 192 values from ONE
                                  DID (Xpeng cell voltages; Hyundai/Kia BMS
                                  DIDs carry 20–32), so 16 rejected every
                                  Hyundai/Kia/Genesis/Xpeng profile
                                  (2026-09-16). Sizes the poller's PSRAM
                                  copy + the filter frame slots, never a
                                  stack frame.                              */

#define AP_NAME_LEN     32
#define AP_CMD_LEN      24
#define AP_INIT_LEN     96
#define AP_HDR_LEN      16
#define AP_EXPR_LEN     96
#define AP_MUX_EXPR_LEN 48     /* raw multiplexer-switch slice              */
#define AP_UNIT_LEN     16
#define AP_CLASS_LEN    24

#define AP_PAYLOAD_MAX  128    /* assembled response payload bytes          */
#define AP_RESP_MAX     1024   /* raw chip response text                    */

typedef enum
{
    AP_PID_STD = 0,
    AP_PID_CUSTOM,
    AP_PID_SPECIFIC,
} ap_pid_type_t;

typedef struct
{
    char   name[AP_NAME_LEN];
    char   expression[AP_EXPR_LEN];
    char   mux_expr[AP_MUX_EXPR_LEN]; /* "" = unconditional; else raw
                                         switch slice that must equal
                                         mux_val for expression to apply */
    char   unit[AP_UNIT_LEN];
    char   class[AP_CLASS_LEN];
    bool   enabled;
    float  min;                /* plausibility clamp; NAN = none            */
    float  max;
    float  mux_val;
} ap_param_t;

typedef struct
{
    char          name[AP_NAME_LEN];
    ap_pid_type_t type;
    char          cmd[AP_CMD_LEN];
    char          init[AP_INIT_LEN];      /* per-PID extra init, ';'-sep    */
    char          rxheader[AP_HDR_LEN];   /* -> ATCRA filter when set       */
    int           group;                  /* index into groups              */
    uint32_t      period_ms;              /* 0 = inherit group              */
    bool          enabled;
    uint16_t      param_start;            /* slice of the param pool        */
    uint16_t      param_count;
} ap_pid_t;

typedef struct
{
    uint32_t frame_id;
    bool     is_extended;
    uint32_t monitor_ms;                  /* ATMA window length             */
    int      group;
    uint32_t period_ms;                   /* 0 = inherit group              */
    bool     enabled;
    uint16_t param_start;
    uint16_t param_count;
} ap_filter_t;

typedef struct
{
    char     name[AP_NAME_LEN];
    bool     enabled_default;
    uint32_t period_ms;
} ap_group_t;

/** The loaded config tables (static PSRAM pools; autopid_config.c fills). */
typedef struct
{
    ap_group_t  groups[AP_MAX_GROUPS];
    ap_pid_t    pids[AP_MAX_PIDS];
    ap_filter_t filters[AP_MAX_FILTERS];
    ap_param_t  params[AP_MAX_PARAMS];
    uint16_t    n_groups;
    uint16_t    n_pids;
    uint16_t    n_filters;
    uint16_t    n_params;
} ap_config_t;

/* ---- pure: scheduler (autopid_sched.c — host-tested) ----------------------
 * The PID is the scheduling unit; entries cover pids then filters
 * (entry index i: i < n_pids -> pid i; else filter i - n_pids).          */

typedef struct
{
    int64_t  due_us;
    int64_t  last_run_us;      /* round-robin tiebreak for period 0        */
    uint16_t fail_streak;
} ap_sched_slot_t;

typedef struct
{
    ap_sched_slot_t slots[AP_MAX_PIDS + AP_MAX_FILTERS];
    /* runtime (EPHEMERAL) group state — autopid_group_set() */
    bool    group_enabled[AP_MAX_GROUPS];
    int32_t group_period_override[AP_MAX_GROUPS]; /* <0 = none             */
    /* per-type enables from settings knobs */
    bool    type_enabled[3];
} ap_sched_t;

#define AP_SCHED_BACKOFF_STREAK 3  /* fails before backing off             */
#define AP_SCHED_BACKOFF_MULT   4
#define AP_SCHED_STAGGER_US     10000

/** Measured chip+ECU round-trip floor (Phase 1b bench: 53 ms/request,
 *  ~19 req/s ceiling). Configured periods 1..floor-1 ms are accepted but
 *  flagged in /api/autopid; period 0 = deliberate max-rate mode. */
#define AP_PERIOD_FLOOR_MS      50

/** Yield-to-app window: the poller stays off the chip until an external
 *  ELM client (TCP/BLE/USB/WS app) has been silent this long. Legacy
 *  parity: main.c cleared DEV_AUTOPID_ELM327_APP_BIT on every app
 *  command and a 10 s timer set it back. Bench 2026-09-08 with the two
 *  interleaved: 80 of 200 app requests answered with autopid's lines,
 *  STOPPED or NO DATA (the "choppy RPM dial" report). */
#define AP_CLIENT_YIELD_MS      10000

/** True while the poller must stay off the chip for an external client
 *  (@p idle_ms = ms since the client's last write, UINT32_MAX = never). */
bool ap_sched_client_hold(uint32_t idle_ms);

/** Reset slots (stagger initial due times) + runtime state from config. */
void ap_sched_reset(ap_sched_t *st, const ap_config_t *cfg, int64_t now_us);

/** Effective period of entry @p i (µs; group inheritance + runtime
 *  override + fail backoff). 0 = high-fidelity. */
int64_t ap_sched_period_us(const ap_sched_t *st, const ap_config_t *cfg,
                           int i);

/** Next runnable entry: smallest due (ties -> least recently run).
 *  @return entry index or -1 when nothing is enabled.
 *  @param[out] due_us when to run it (may be in the past = now). */
int ap_sched_next(const ap_sched_t *st, const ap_config_t *cfg,
                  int64_t *due_us);

/** Record a run of entry @p i: reschedule + fail-streak bookkeeping. */
/** Runtime group control (pure): set = the §5b override (period < 0 keeps
 *  the current override), restore = back to the configured defaults (the
 *  undo of a while-rule). @p g is a valid group index. */
void ap_sched_group_set(ap_sched_t *st, int g, bool enabled,
                        int32_t period_override_ms);
void ap_sched_group_restore(ap_sched_t *st, const ap_config_t *cfg, int g);

void ap_sched_ran(ap_sched_t *st, const ap_config_t *cfg, int i,
                  int64_t now_us, bool ok);

/** True when entry @p i is enabled (entry + group + type gates). */
bool ap_sched_entry_enabled(const ap_sched_t *st, const ap_config_t *cfg,
                            int i);

/* ---- cache (autopid_cache.c — target; slot index == param pool index) ----- */
#ifndef AUTOPID_HOST_TEST
#include "cJSON.h"
void  ap_cache_init(void);
void  ap_cache_clear(void);
void  ap_cache_put(uint16_t slot, double value, int64_t ts_us);
bool  ap_cache_get(uint16_t slot, double *value, int64_t *ts_us);
cJSON *ap_cache_snapshot(const ap_config_t *cfg);
cJSON *ap_cache_detail(const ap_config_t *cfg);
int   ap_cache_find(const ap_config_t *cfg, const char *param);

/* External (injected) values — named samples that are NOT config-slot
 * bound (GPS from the ESPNetLink dongle). Merged into snapshot/get so
 * they ride the same autopid_data push, ${autopid.*} pulls, value sink,
 * and autopid.param events as polled parameters. */
int   ap_ext_put(const char *name, const char *unit, double value,
                 int64_t ts_us, bool *out_changed);
bool  ap_ext_get(const char *name, double *value, int64_t *ts_us);

/* config file IO (autopid_config.c, target half) */
esp_err_t autopid_config_load(ap_config_t *cfg);
esp_err_t autopid_config_save(const char *json, size_t len);
const char *autopid_config_path(void);

/* core internals shared with the http/cli surfaces (autopid.c) */
const ap_config_t *ap_core_config(void);
/* autopid_group.c (runtime group control + its JSON) borrows the core's
   lock, scheduler state and poller wake-up through these */
void ap_core_lock(void);
void ap_core_unlock(void);
ap_sched_t *ap_core_sched(void);
void ap_core_wake(void);
esp_err_t ap_core_group_json(cJSON *arr);   /* append group states       */
uint16_t ap_core_sub_floor_count(void);     /* PIDs configured 1..49 ms  */
void ap_core_scan_pause(bool on);           /* std scan owns the chip    */

/* settings (autopid_settings.c — standard §4.1) */
esp_err_t ap_settings_register(void);       /* the "autopid" descriptor  */
bool ap_settings_is_configured(void);       /* boot apply ran (§4.3)     */
int  ap_settings_pause_below_mv(void);      /* 0 = never pause           */
bool ap_settings_pause_follow_sleep(void);  /* legacy parity: pause
                                               requests below sleep_mv   */
const char *ap_core_std_protocol(void);     /* std_protocol setting      */
uint32_t ap_core_min_event_interval_ms(void);

/* on_apply -> runtime state owned by autopid.c */
void ap_core_set_enabled(bool enabled);
void ap_core_set_type_enabled(int type, bool enabled);

/* event_manager glue (autopid_events.c — Phase 3) */
void ap_events_register(void);              /* sources/action/values     */
void ap_events_reset(void);                 /* clear emission memory     */
void ap_events_param(const ap_param_t *prm, uint16_t slot, int group,
                     double value);         /* on-change + min interval  */
void ap_events_external(const char *name, const char *unit, double value,
                        bool changed);      /* injected value → sink+event */
void ap_events_pid_failed(const char *name, uint16_t streak);
void ap_events_scan_done(uint16_t found);

/* chip-facing runner (autopid_runner.c — poller-task context) */
bool ap_runner_run(const ap_pid_t *pid, int pid_index,
                   const ap_param_t *params);
void ap_runner_set_type_init(int type, const char *init);
void ap_runner_reset(void);        /* replay inits on the next poll     */
/** After an external ELM app had the chip: re-send the protocol prelude
 *  (spaces on, headers off, timeout, protocol, header, mask) and replay
 *  every type/PID init on the next poll. Poller-task context. */
void ap_runner_restore_baseline(void);

/* ATMA filter window (autopid_filter.c — poller-task context) */
bool ap_runner_run_filter(const ap_filter_t *f, const ap_param_t *params);

/** One-shot test-a-PID through the real runner choreography (§11):
 *  type init chain, per-PID init, ATCRA, request, ATCRA off — exactly
 *  what a poll of that PID sends. Caller pauses the poller around it
 *  (ap_core_scan_pause). `transcript` (may be NULL) receives one line
 *  per exchange, "> cmd" then "< reply", so the UI can show what went
 *  out and what came back (2026-09-16). */
esp_err_t ap_runner_test(const char *type_init, const char *init,
                         const char *rxheader, const char *cmd, char *raw,
                         size_t raw_len, int64_t *elapsed_us,
                         char *transcript, size_t transcript_len);
/** The init chain the poller sends when it switches to this PID type
 *  (what autopid_settings composed from the protocol + *_init). */
const char *ap_runner_type_init(int type);

/* standard-PID scan (autopid_std.c, target half) */
esp_err_t autopid_std_scan_start(void);     /* INVALID_STATE if running  */
cJSON *ap_std_scan_status_json(void);       /* {status,found,error,ts}   */
const char *ap_std_prelude(void);           /* ATS1;ATH0;ATST96;ATTP<p>.. */
cJSON *ap_std_table_json(void);             /* the full SAE table for UI */
const char *autopid_std_scan_path(void);    /* /data/autopid/std_scan.json */
#endif

/* ---- pure: standard-PID helpers (autopid_std.c — host-tested) -------------
 * ap_std_expression maps a legacy table row (bit_start counts within the
 * headers-on buffer [PCI, mode, PID, A, B, ...]) to a v6 expression over
 * our payload (echo included, no PCI): byte = bit_start/8 - 1.
 * Placeholder rows (bit_length 0) return ESP_ERR_NOT_SUPPORTED.        */
esp_err_t ap_std_expression(uint8_t bit_start, uint8_t bit_length,
                            double scale, double offset, char *buf,
                            size_t buf_len);

/** OR-merge every "41 <pid> A B C D" line of @p resp (headers on or off,
 *  multi-ECU) into @p bitmap. True when at least one row matched. PURE. */
bool ap_std_scan_parse(const char *resp, uint8_t expect_pid,
                       uint32_t *bitmap);

/* config parse (PURE half of autopid_config.c) */
esp_err_t ap_config_parse(const char *json, ap_config_t *cfg, char *err,
                          size_t err_len);

/** In-place neutralization of EEPROM-writing user init commands
 *  (case-insensitive, whitespace-tolerant: "atsp6", "AT SP 6"):
 *  ATSP -> ATTP (same protocol switch, RAM only) and ATM1 -> ATM0
 *  (memory-on would make the chip store every subsequent protocol
 *  change). Init strings replay on every type/PID transition, so
 *  letting either through would wear the chip's EEPROM out (legacy
 *  semantics). PURE. Since 2026-09-16 a thin wrapper over the chip
 *  driver's guard (obd_chip_guard.h), which also names the commands
 *  with no RAM twin (ATPP/ATSD/ATCV/STWBR): config parse refuses those. */
void ap_init_sanitize(char *str);

/* ---- pure: ELM response text -> payload bytes (autopid_resp.c) ------------
 * Handles: echo/blank/SEARCHING lines, error markers (NO DATA, ERROR,
 * STOPPED, ?), headers-off single line ("41 0C 1A F8"), headers-off
 * ISO-TP multi-line ("014" + "0: 49 02 .." + "1: .."), headers-on frames
 * ("7E8 06 41 00 ..") incl. ISO-TP single/first/consecutive reassembly
 * from the lowest-ID responder. Payload INCLUDES the service/PID echo
 * bytes — user expressions index from B0 = 0x41 (legacy semantics).   */
esp_err_t ap_resp_to_payload(const char *resp, uint8_t *payload,
                             size_t payload_max, size_t *out_len);

#define AP_RESP_ECUS_MAX 8      /* 7E8..7EF — the functional-response set */

typedef struct
{
    uint32_t header;            /* CAN id; UINT32_MAX = headers were off */
    uint8_t  payload[AP_PAYLOAD_MAX];
    size_t   len;
} ap_resp_ecu_t;

/** Like ap_resp_to_payload but keeps EVERY responder when headers are
 *  on: one entry per distinct CAN id (ascending), each ISO-TP
 *  reassembled independently. Headers off = single entry (identical to
 *  ap_resp_to_payload). @return entry count, 0 = no payload,
 *  -1 = chip/bus error line seen. */
int ap_resp_to_payloads(const char *resp, ap_resp_ecu_t *out,
                        int max_ecus);

/** One ATMA monitor line -> frame data bytes when it carries @p frame_id
 *  (legacy-compatible header shapes: contiguous "7E8"/"18DAF110" or the
 *  id split into 2-hex byte tokens). Expressions index from B0 = first
 *  DATA byte (no id echo — the legacy filter frame-of-reference). PURE. */
bool ap_filter_frame(const char *line, size_t len, uint32_t frame_id,
                     uint8_t *payload, size_t payload_max,
                     size_t *out_len);

/** Incremental ATMA stream collector: feed raw monitor bytes in ANY
 *  chunking; returns true the moment a completed line carries
 *  @p frame_id (first match wins — repeats of the id in the same
 *  window are simply never reached). Overlong lines are truncated
 *  safely; non-matching/noise lines are skipped. PURE. */
typedef struct
{
    char   line[160];
    size_t n;
    bool   overflow;            /* current line exceeded the buffer      */
} ap_flt_stream_t;

void ap_flt_stream_init(ap_flt_stream_t *st);
bool ap_flt_stream_feed(ap_flt_stream_t *st, const uint8_t *bytes,
                        size_t len, uint32_t frame_id, uint8_t *payload,
                        size_t payload_max, size_t *out_len);

/** ap_flt_stream_feed that reports how many input bytes were consumed
 *  when a frame matched (up to and including its line terminator), so
 *  the caller can resume mid-chunk and capture EVERY matching frame —
 *  a multiplexed message needs more than the first one. On false the
 *  whole chunk was consumed. */
bool ap_flt_stream_feed_ex(ap_flt_stream_t *st, const uint8_t *bytes,
                           size_t len, uint32_t frame_id,
                           uint8_t *payload, size_t payload_max,
                           size_t *out_len, size_t *consumed);

/** True when @p payload plausibly answers @p cmd: hex service commands
 *  must echo (service | 0x40) + the identifier byte. Rejects cross-talk
 *  when another chip master's response lands in our request window
 *  (bench-proven under WS-OBD contention, Phase 1b). AT/ST/VT commands
 *  return true (nothing to verify). PURE. */
bool ap_payload_matches_cmd(const char *cmd, const uint8_t *payload,
                            size_t payload_len);

/* ---- pure: DTC codec (autopid_dtc_codec.c — host-tested; TASK_dtc.md §4) --
 * Mode 04 clears EVERYTHING (codes + readiness + MIL) — OBD2 has no
 * per-code clear, so "clear specific DTCs" is a CONDITION on the whole
 * present set (always / if_any / if_only).                              */

#define AP_DTC_MAX       32     /* codes kept per category               */
#define AP_DTC_CODE_LEN  10     /* "P0420-08" + NUL — UDS failure-type
                                   suffix (TASK_dtc §12; was 6/"P0420"
                                   until 2026-07-22; FTB 0 omits the
                                   suffix so OBD- and UDS-sourced codes
                                   stay string-identical)               */

typedef enum
{
    AP_DTC_CLEAR_INVALID = 0,
    AP_DTC_CLEAR_ALWAYS,
    AP_DTC_CLEAR_IF_ANY,        /* >=1 listed code present               */
    AP_DTC_CLEAR_IF_ONLY,       /* every present code is listed          */
} ap_dtc_clear_mode_t;

/** 2-byte DTC -> "P0420" (bits 15-14 letter, 13-12 first digit). */
void ap_dtc_format(uint8_t hi, uint8_t lo, char out[AP_DTC_CODE_LEN]);

/** "P0420" -> 2 bytes; false on malformed input. Case-insensitive. */
bool ap_dtc_unformat(const char *code, uint8_t *hi, uint8_t *lo);

/** Parse a 43/47/4A payload (ap_resp_to_payload output, service echo
 *  first) into formatted codes. Skips 0x0000 padding pairs, tolerates a
 *  stripped count byte, keeps what fits in @p out_max.
 *  @return code count, or -1 when the service byte doesn't match. */
int ap_dtc_parse_codes(const uint8_t *payload, size_t len,
                       uint8_t service_resp,
                       char out[][AP_DTC_CODE_LEN], size_t out_max);

/** Union-merge @p src into @p dst (skip duplicates, keep order).
 *  @return the new dst count (<= dst_max). */
uint8_t ap_dtc_merge_codes(char dst[][AP_DTC_CODE_LEN], uint8_t n_dst,
                           size_t dst_max,
                           const char src[][AP_DTC_CODE_LEN],
                           uint8_t n_src);

/** Parse "41 01 AA .." -> MIL bit (A7) + stored count (A6..A0). */
bool ap_dtc_parse_mil(const uint8_t *payload, size_t len, bool *mil,
                      uint8_t *count);

/** Map a mode string (+ code list, for the default) to the enum;
 *  NULL/"" mode = always without codes, if_any with them. */
ap_dtc_clear_mode_t ap_dtc_clear_mode_parse(const char *mode,
                                            const char *codes);

/** Evaluate the clear condition against the present stored codes. */
bool ap_dtc_clear_allowed(const char present[][AP_DTC_CODE_LEN],
                          size_t n_present, const char *codes,
                          ap_dtc_clear_mode_t mode);

/* ---- pure: freeze-frame codec (TASK_dtc §14 — OBD mode 02, frame 0) ------
 * Payload shapes are ap_resp_to_payloads output with the echo kept:
 * `42 <pid> <frame> <data…>`. Decode rides the standard-PID table
 * (autopid_std.c), byte semantics identical to the mode-01 expressions. */

#define AP_FRZ_MAX      24      /* decoded values kept per report        */
#define AP_FRZ_NAME_LEN 40      /* std-table param names                 */

typedef struct
{
    char  name[AP_FRZ_NAME_LEN];
    char  unit[AP_UNIT_LEN];
    float value;
} ap_frz_val_t;

/** Parse a `42 02 <frame> hi lo` payload -> the DTC that froze the frame
 *  (DTCFRZF). @return true only for a non-zero DTC (0x0000 = no frame
 *  stored — some ECUs answer zeros instead of NO DATA). */
bool ap_frz_dtc(const uint8_t *payload, size_t len,
                char out[AP_DTC_CODE_LEN]);

/** Parse a `42 <base> <frame> b0..b3` supported-PID bitmap payload for
 *  bitmap base @p pid (0x00/0x20/0x40…). MSB of b0 = PID base+1. */
bool ap_frz_bitmap(const uint8_t *payload, size_t len, uint8_t pid,
                   uint32_t *bitmap);

/** Decode a mode-02 value payload `[0x42, pid, frame, A, B, …]` via the
 *  standard-PID table (autopid_std.c owns the table; same byte indexing
 *  as ap_std_expression, value = raw*scale+offset). Appends decoded
 *  params to @p out starting at @p n. @return the new count (<= max). */
int ap_frz_decode(const uint8_t *payload, size_t len, ap_frz_val_t *out,
                  int n, int max);

/* ---- pure: DTC-database importer (autopid_dtc_db_codec.c — host-tested;
 * TASK_dtc_db.md §2). Canonical stored form: "#dtcdb1 <n>\n" then sorted
 * "CODE\tDESC\n" lines.                                                 */

#define AP_DTC_DB_MAX        8      /* databases                        */
#define AP_DTC_DB_NAME_LEN   25     /* cert-set-style names             */
#define AP_DTC_DB_FILE_MAX   (1024 * 1024)  /* upload cap               */
#define AP_DTC_DB_ENTRIES_MAX 20000
#define AP_DTC_DESC_MAX      95     /* description chars kept           */

typedef struct
{
    char     code[AP_DTC_CODE_LEN];
    uint32_t off;                   /* desc offset into scratch/buffer  */
    uint32_t seq;                   /* original order (dedup tiebreak)  */
    uint16_t len;                   /* desc length                      */
} ap_dtc_db_item_t;

/** Sniff + parse @p in (CSV/TSV/;-CSV/JSON-map/JSON-array/plain text)
 *  into scratch + a SORTED deduped item index (last duplicate wins).
 *  CONTRACT: @p scratch_cap must be >= in_len + AP_DTC_DESC_MAX + 16 —
 *  the emitter checks per-entry headroom BEFORE writing (a smaller cap
 *  silently rejects the tail; bench-bitten 2026-07-08).
 *  @return entry count, or -1 (err filled; fmt_out = sniffed format). */
int ap_dtc_db_import(const char *in, size_t in_len, char *scratch,
                     size_t scratch_cap, ap_dtc_db_item_t *items,
                     size_t items_cap, char fmt_out[12], char *err,
                     size_t err_len);

/** Emit the canonical form. @return bytes written, 0 = out too small. */
size_t ap_dtc_db_serialize(const char *scratch,
                           const ap_dtc_db_item_t *items, int n,
                           char *out, size_t out_cap);

/** Index a canonical buffer (validates header + count).
 *  @return entry count or -1. Items' off/len point into @p buf. */
int ap_dtc_db_index(const char *buf, size_t len,
                    ap_dtc_db_item_t *items, size_t items_cap);

/** Picker query: code-prefix OR description-substring, both
 *  case-insensitive; empty/NULL q matches everything. */
bool ap_dtc_db_match(const char *code, const char *desc, size_t desc_len,
                     const char *q);

/* ---- pure: DBC codec (autopid_dbc_codec.c — host-tested; TASK_dbc.md) ----
 * Parse the BO_/SG_/SIG_VALTYPE_ subset; compile signals into
 * expression_parser expressions over filter payloads (B0 = first frame
 * data byte). Unsupported signals stay LISTED with a reason.           */

#define AP_DBC_MAX        4     /* stored .dbc files                    */
#define AP_DBC_NAME_LEN   33    /* message/signal identifiers           */
#define AP_DBC_FILE_MAX   (1024 * 1024)
#define AP_DBC_MSGS_MAX   400   /* per file                             */
#define AP_DBC_SIGS_MAX   3000  /* per file                             */

typedef struct
{
    uint32_t id;                /* 29-bit masked                        */
    bool     ext;
    bool     mux_complex;       /* extended multiplexing (SG_MUL_VAL_,
                                   m<N>M, or >1 switch) — unsupported   */
    uint8_t  dlc;
    char     name[AP_DBC_NAME_LEN];
} ap_dbc_msg_t;

typedef struct
{
    uint16_t msg;               /* index into the message table         */
    uint16_t start;             /* DBC start bit (raw numbering)        */
    uint8_t  len;
    bool     intel;             /* @1 = little endian                   */
    bool     is_signed;
    uint8_t  mux;               /* 0 plain, 1 m<N>, 2 M switch, 3 ext   */
    uint8_t  valtype;           /* 0 int, 1 float, 2 double             */
    uint16_t mux_val;           /* the N of m<N> (mux == 1)             */
    double   factor, offset, min, max;
    char     name[AP_DBC_NAME_LEN];
    char     unit[AP_UNIT_LEN];
} ap_dbc_sig_t;

/** Parse @p text. @return signal count (>=1) or -1 (err filled).
 *  Over-cap messages/signals are dropped silently (keep what fits). */
int ap_dbc_parse(const char *text, size_t len, ap_dbc_msg_t *msgs,
                 size_t msgs_cap, int *n_msgs, ap_dbc_sig_t *sigs,
                 size_t sigs_cap, char *err, size_t err_len);

/** Compile @p s into an expression (<= AP_EXPR_LEN incl. NUL).
 *  ESP_ERR_NOT_SUPPORTED sets @p reason (static string). */
esp_err_t ap_dbc_expr(const ap_dbc_sig_t *s, char *out, size_t out_cap,
                      const char **reason);

/** Multiplex precondition for @p s. Plain / M-switch signals: ESP_OK
 *  with expr_out = "" (no condition). m<N> signals: compiles the
 *  message's M switch as a RAW unsigned slice into @p expr_out and sets
 *  @p val_out = N — the runner evaluates the slice per frame and only
 *  applies the signal expression when it equals N. Extended
 *  multiplexing (mux 3 / msg.mux_complex) and switchless m<N> signals
 *  are ESP_ERR_NOT_SUPPORTED with @p reason set. */
esp_err_t ap_dbc_mux_cond(const ap_dbc_sig_t *s, const ap_dbc_msg_t *msgs,
                          const ap_dbc_sig_t *sigs, int n_sigs,
                          char *expr_out, size_t expr_cap,
                          float *val_out, const char **reason);

/** Reference decoder — host cross-check ONLY. */
double ap_dbc_decode_ref(const ap_dbc_sig_t *s, const uint8_t data[8]);

/** The scan report (RAM-only; guarded by the dtc module's own lock). */
typedef struct
{
    int64_t  ts_us;             /* completion time (esp_timer clock)     */
    int64_t  ts_epoch;          /* wall clock, 0 when time isn't valid   */
    bool     valid;
    bool     mil;
    uint8_t  mil_count;         /* summed across responding ECUs         */
    uint8_t  n_ecus;            /* 0101 responders (1 when headers off)  */
    char     protocol[5];       /* "obd" | "uds" — which path answered   */
    uint8_t  n_stored, n_pending, n_permanent, n_new;
    char     stored[AP_DTC_MAX][AP_DTC_CODE_LEN];
    char     pending[AP_DTC_MAX][AP_DTC_CODE_LEN];
    char     permanent[AP_DTC_MAX][AP_DTC_CODE_LEN];
    char     new_codes[AP_DTC_MAX][AP_DTC_CODE_LEN];
    /* freeze frame (mode 02 frame 0, OBD scans only — TASK_dtc §14) */
    bool     frz_present;
    char     frz_dtc[AP_DTC_CODE_LEN];  /* DTCFRZF                       */
    uint32_t frz_ecu;           /* responder CAN id; UINT32_MAX = hdr off */
    uint8_t  n_frz;
    ap_frz_val_t frz[AP_FRZ_MAX];
    char     error[48];         /* last scan error, "" = ok              */
} ap_dtc_report_t;

#ifndef AUTOPID_HOST_TEST
/* DTC engine (autopid_dtc.c — target half; TASK_dtc.md §5) */
void ap_dtc_init(void);                      /* autopid_init context     */
void ap_dtc_apply_settings(const cJSON *settings); /* on_apply context   */
bool ap_dtc_enabled(void);
bool ap_dtc_clear_gate_open(void);           /* dtc_allow_clear          */
bool ap_dtc_busy(void);
esp_err_t ap_dtc_scan_start(void);           /* 409-equiv INVALID_STATE  */
void ap_dtc_periodic_check(void);            /* poller loop: start scan
                                                when the period is due   */
esp_err_t ap_dtc_report_get(ap_dtc_report_t *out);
cJSON *ap_dtc_report_json(void);             /* the §8 GET "report" obj  */

/** Conditional clear (sync, seconds of bus I/O — NEVER the event
 *  dispatcher; HTTP/CLI/scan-job/script contexts only). Re-reads mode 03
 *  first, evaluates, sends 04, confirms with a second 03.
 *  @param[out] cleared condition held + 44 confirmed
 *  @param[out] before/after stored-code counts around the clear. */
esp_err_t ap_dtc_clear(const char *codes, const char *mode, bool *cleared,
                       uint8_t *before, uint8_t *after, char *err,
                       size_t err_len);

/** Fire-and-forget clear for the event action (dispatcher context must
 *  not block on the bus) — spawns the job task, result travels by the
 *  autopid.dtc_clear event. */
esp_err_t ap_dtc_clear_queue(const char *codes, const char *mode);

/** One-shot chip-job serialization (std scan / test-a-PID / dtc jobs
 *  never interleave; each still pauses the poller via
 *  ap_core_scan_pause). autopid.c owns the flag. */
bool ap_core_job_acquire(void);
void ap_core_job_release(void);

/* DTC-database store + PSRAM cache (autopid_dtc_db.c — TASK_dtc_db §3) */
void ap_dtc_db_init(void);                   /* autopid_init context     */
void ap_dtc_db_load_all(void);               /* internal-stack ONLY (fs) */
esp_err_t ap_dtc_db_store(const char *name, const char *raw,
                          size_t raw_len, int *entries_out,
                          char fmt_out[12], char *err, size_t err_len);
esp_err_t ap_dtc_db_delete(const char *name);
bool ap_dtc_db_lookup(const char *code, char *out, size_t out_cap);
cJSON *ap_dtc_db_list_json(void);
cJSON *ap_dtc_db_search_json(const char *q, const char *db_name,
                             int offset, int limit);
void ap_dtc_db_desc_map(cJSON *parent, const char *key,
                        const ap_dtc_report_t *r);

/* DBC store + cache + add-to-filters (autopid_dbc.c — TASK_dbc.md §4-5) */
void ap_dbc_init(void);                      /* autopid_init context     */
void ap_dbc_load_all(void);                  /* internal-stack ONLY (fs) */
esp_err_t ap_dbc_store(const char *name, const char *raw, size_t raw_len,
                       int *msgs_out, int *sigs_out, char *err,
                       size_t err_len);
esp_err_t ap_dbc_delete(const char *name);
cJSON *ap_dbc_list_json(void);
cJSON *ap_dbc_signals_json(const char *db, const char *q, int offset,
                           int limit);
esp_err_t ap_dbc_add(const char *db, const cJSON *signals,
                     const char *group, int monitor_ms, int period_ms,
                     cJSON **result, char *err, size_t err_len);

/* DTC event glue (autopid_events.c) */
void ap_events_dtc_register(void);           /* sources+actions+values   */
void ap_events_dtc_new_code(const char *code, const char *status,
                            bool mil);
void ap_events_dtc_scan(const ap_dtc_report_t *r, bool ok);
void ap_events_dtc_clear(bool ok, bool cleared, uint8_t before,
                         uint8_t after);
#endif

#ifdef __cplusplus
}
#endif
