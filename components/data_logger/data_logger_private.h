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
 * @file data_logger_private.h
 * @brief Internals shared between the lifecycle/writer, the storage
 *        engines, the pure helpers, and the glue files.
 *
 * Two record STREAMS since 2026-07-09 (TASK addendum): numeric params
 * (`dl_<epoch>.<ext>`) and raw CAN frames (`can_<epoch>.<ext>`), each
 * with its own engine choice, rotation and retention. Engines take a
 * context pointer so two files can be open at once.
 *
 * The clock (2026-10-13, issue #984): every record carries the monotonic
 * time it was taken at beside its wall stamp. The writer re-bases the
 * wall stamp of records queued before a clock step (rtc_manager counts
 * the steps), the relative formats take their time from the monotonic
 * clock, and the journal `/sd/logs/clock.jsonl` says what the clock did
 * and which files were open. data_logger_clock.c holds the pure rules,
 * data_logger_journal.c the writer-side glue.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

#include "data_logger.h"

#define DL_MAX_PARAMS   256  /* PSRAM registry; a real autopid profile
                                carries hundreds of params */
#define DL_SOURCE_MAX   16
#define DL_NAME_MAX     32
#define DL_RING_LEN     2048  /* param ring (PSRAM)                     */
#define DL_CAN_RING_MAX 8192 /* frame ring cap; ring_len setting picks
                                the live size (static PSRAM, ~192 KB
                                at cap: caps not rationing)           */
#define DL_DIR          "/sd/logs"
#define DL_PREFIX_PARAM "dl_"
#define DL_PREFIX_CAN   "can_"
/* ROBUSTNESS.md (2026-09-07) */
#define DL_CORRUPT_SUFFIX ".corrupt"            /* set-aside file: <name>.corrupt   */
#define DL_CORRUPT_KEEP   2                     /* set-aside files kept per stream  */
#define DL_QUICKCHECK_MAX (8u * 1024u * 1024u)  /* resume-time quick_check bound (B) */
#define DL_BYTES_PER_ROW   26 /* measured: 100 KB / 4000 rows          */
#define DL_BYTES_PER_FRAME 44 /* sqlite frames-table size estimate     */
/* the PSRAM .noinit envelope's layout version: 2 since the record grew
 * its monotonic stamp (2026-10-13); a version-1 ring is discarded once */
#define DL_PS_VERSION 2u

/* record kinds */
#define DL_REC_PARAM 0
#define DL_REC_FRAME 1

/* record flags (dl_record_t.flags) */
#define DL_REC_ABS 0x01  /* an explicit stamp (data_logger_write_at, a
                            record salvaged from before a reset): never
                            re-based after a clock step */

/* frame flag bits (dl_record_t.u.f.flags and the .wdl 0x03 id word) */
#define DL_FRAME_EXT 0x01
#define DL_FRAME_RTR 0x02

/* one queued record (PSRAM rings; 32 bytes on the S3 and on the host:
 * the stamp and the flags sit in what used to be padding) */
typedef struct
{
    int64_t ts_ms;               /* wall clock, epoch ms, at production */
    union
    {
        struct
        {
            double  value;
            int16_t param;
        } p;
        struct
        {
            uint32_t id;
            uint8_t  data[8];
            uint8_t  dlc;
            uint8_t  flags;      /* DL_FRAME_EXT / DL_FRAME_RTR       */
        } f;
    } u;
    uint8_t  kind;               /* DL_REC_PARAM / DL_REC_FRAME       */
    uint8_t  flags;              /* DL_REC_*                          */
    uint32_t mono_ms;            /* esp_timer ms at production (wraps
                                    at 49.7 days: compared modulo)    */
} dl_record_t;

_Static_assert(sizeof(dl_record_t) == 32, "dl_record_t must stay 32 bytes");

/* RAM param registry entry; db_id is per-open-file (re-interned lazily
 * after every open/rotate, 0 = not yet interned into this file) */
typedef struct
{
    bool used;
    char source[DL_SOURCE_MAX];
    char name[DL_NAME_MAX];
    int  db_id;
} dl_param_entry_t;

/* ---- the clock (data_logger_clock.c: PURE, host-tested) --------------------- */

#define DL_CLOCK_ROTATE_MS  60000   /* a step this big rotates the files */
#define DL_CLOCK_STEPS_KEEP 8       /* steps kept for re-basing          */
#define DL_JOURNAL_NAME     "clock.jsonl"
#define DL_JOURNAL_MAX      (64u * 1024u)  /* above this ...             */
#define DL_JOURNAL_KEEP     (32u * 1024u)  /* ... the newest part stays  */

/* one set of the system clock as rtc_manager measured it (its source
 * numbering: 0 none, 1 rtc, 2 sntp, 3 manual) */
typedef struct
{
    uint32_t generation;
    uint32_t mono_ms;            /* when it happened (esp_timer ms)   */
    int64_t  delta_ms;           /* new wall - old wall               */
    int64_t  wall_ms;            /* the wall clock right after it     */
    uint8_t  source;
} dl_clock_step_t;

/* add the delta of every step that happened AFTER the record was taken
 * (modulo-32 comparison of the monotonic stamps); DL_REC_ABS records are
 * left alone. Returns the number of steps applied. */
int dl_clock_rebase(dl_record_t *rec, const dl_clock_step_t *steps, int n);
/* a record's time relative to a file's open, for the single-use formats:
 * from the monotonic stamps (a wall step cannot make it negative or
 * non-monotonic), from the wall stamps for DL_REC_ABS records */
int64_t dl_clock_rel_ms(const dl_record_t *rec, int64_t start_ms,
                        uint32_t mono_open_ms);
/* the open time of a file as the wall clock says NOW (the header start
 * of mf4/blf is re-patched with it at every commit) */
int64_t dl_clock_start_ms(int64_t wall_now_ms, uint32_t mono_now_ms,
                          uint32_t mono_open_ms);
/* the first valid clock of a boot and any step of DL_CLOCK_ROTATE_MS or
 * more rotate the files; smaller steps are journaled only */
bool dl_clock_rotates(const dl_clock_step_t *step, bool first_valid);
const char *dl_clock_source_name(uint8_t source);

/* journal lines (one JSON object per line, byte-exact in the host test) */
int dl_journal_boot_line(char *out, size_t cap, uint32_t boot,
                         int64_t ts_ms, uint32_t up_ms, bool valid,
                         uint8_t source);
int dl_journal_step_line(char *out, size_t cap, const char *ev,
                         uint32_t boot, const dl_clock_step_t *step,
                         const char *const *files, int nfiles);
int dl_journal_rotate_line(char *out, size_t cap, uint32_t boot,
                           int64_t ts_ms, const char *stream,
                           const char *from, const char *to);
/* the byte offset inside the kept tail of an oversized journal where the
 * first whole line starts (the tail is cut there) */
size_t dl_journal_trim_offset(const char *tail, size_t len);

/* in-band markers where a format has room for one */
int    dl_jsonl_step_row(char *out, size_t cap, const dl_clock_step_t *step);
int    dl_csv_step_row(char *out, size_t cap, const dl_clock_step_t *step);
#define DL_WDL_STEP_LEN 18   /* u8 0x04 | i64 ts_ms | i64 delta_ms | u8 source */
size_t dl_wdl_encode_step(uint8_t *buf, const dl_clock_step_t *step);

/* ---- the journal and the writer-side clock glue (data_logger_journal.c) ---
   Writer task only. The first lap takes the clock's generation as found
   (the boot restore predates every file of this boot); every later step
   is journaled, marked in the open files, and rotates them when it is the
   first valid clock of the boot or 60 s or more. */

typedef struct
{
    bool     valid;
    uint8_t  source;
    uint32_t steps_this_boot;    /* rtc_manager's count                */
    uint32_t rotations_by_clock;
    uint32_t journal_bytes;
} dl_clock_stats_t;

void dl_journal_set_boot(uint32_t boot);   /* the restart tracker's count */
void dl_journal_lap(void);                 /* before every batch         */
void dl_journal_rebase(dl_record_t *rec);  /* on every popped record     */
void dl_journal_stats(dl_clock_stats_t *out);

/* what the journal needs from the writer (data_logger.c) */
typedef struct
{
    const char *stream;          /* "params" / "can"                   */
    char        from[48];
    char        to[48];          /* "" when the new file did not open  */
} dl_rotated_t;

int  dl_writer_open_files(const char **names, int cap);
int  dl_writer_clock_rotate(const dl_clock_step_t *step, dl_rotated_t out[2]);
void dl_writer_clock_mark(const dl_clock_step_t *step);

/* ---- storage engines (writer task ONLY) ----------------------------------

   Each stream binds one engine + one context at boot (`format` /
   `can_format` settings). All engines handle BOTH record kinds; a
   stream's files stay homogeneous because the streams are split by
   kind upstream. sqlite is the compatibility engine (~700 rows/s,
   BENCHMARKS.md); csv and binary are append logs. */
typedef struct
{
    const char *ext;                 /* ".db" / ".csv" / ".wdl" / …      */
    /* single_use: relative-timestamp / patched-header formats (mf4,
     * blf, asc) can't be appended across boots, open_current always
     * starts a fresh file for them */
    bool single_use;
    esp_err_t (*open)(void *ctx, const char *path, bool frames);
    void      (*close)(void *ctx);
    bool      (*is_open)(void *ctx);
    esp_err_t (*begin)(void *ctx);   /* transaction / no-op              */
    /* p = param registry entry for DL_REC_PARAM, NULL for frames */
    esp_err_t (*write)(void *ctx, const dl_record_t *rec,
                       dl_param_entry_t *p);
    esp_err_t (*commit)(void *ctx);  /* commit / flush                   */
    uint64_t  (*bytes)(void *ctx);   /* approx bytes in the current file */
    /* optional (ROBUSTNESS.md): "the last failure means the FILE is bad",
     * the writer sets such a file aside instead of retrying it forever */
    bool      (*corrupt)(void *ctx);
    /* optional: never resume a file bigger than this (0 = no limit); the
     * .wdl torn-tail check is a full read, so huge CAN logs start fresh */
    uint64_t  resume_max;
    /* optional (2026-10-13): an in-band clock-step marker, made durable
     * by the call itself (outside any batch). mark_close_only = only as
     * the LAST record before a close (.wdl: a reader that does not know
     * the record type stops there as at a torn tail, nothing lost) */
    esp_err_t (*mark)(void *ctx, const dl_clock_step_t *step);
    bool      mark_close_only;
} dl_engine_t;

extern const dl_engine_t dl_engine_sqlite;   /* data_logger_db.c     */
extern const dl_engine_t dl_engine_csv;      /* data_logger_append.c */
extern const dl_engine_t dl_engine_binary;   /* data_logger_append.c */
extern const dl_engine_t dl_engine_candump;  /* data_logger_append.c */
extern const dl_engine_t dl_engine_asc;      /* data_logger_append.c */
extern const dl_engine_t dl_engine_jsonl;    /* data_logger_append.c */
extern const dl_engine_t dl_engine_mf4;      /* data_logger_mf4.c    */
extern const dl_engine_t dl_engine_blf;      /* data_logger_blf.c    */

/* text/append format discriminator (dl_append_ctx_t.fmt) */
#define DL_AP_CSV     0
#define DL_AP_WDL     1
#define DL_AP_CANDUMP 2
#define DL_AP_ASC     3
#define DL_AP_JSONL   4

/* engine contexts: one static instance per (engine kind × stream) is
 * owned by data_logger.c; the structs live here so it can size them */
typedef struct
{
    void    *f;                  /* FILE*; void* keeps stdio out of
                                    the host-test build               */
    uint64_t bytes;
    char    *buf;                /* 4 KB DMA-capable stdio buffer:
                                    lazy INTERNAL heap, open→close     */
    uint16_t next_id;            /* .wdl dictionary ids               */
    uint8_t  fmt;                /* DL_AP_*                           */
    bool     frames;             /* header/row shape at open          */
    int64_t  start_ms;           /* asc: the header date (wall)       */
    uint32_t mono_open;          /* asc: rows are relative to this    */
} dl_append_ctx_t;

typedef struct                   /* MDF 4.10 (data_logger_mf4.c)      */
{
    void    *f;
    uint64_t bytes;
    uint64_t records;
    int64_t  start_ms;           /* the header start as last patched  */
    uint32_t mono_open;          /* records are relative to this      */
    uint32_t dt_len_off;         /* patch targets (commit)            */
    uint32_t cg_cycle_off;
    char    *buf;                /* lazy 4 KB DMA stdio buffer        */
} dl_mf4_ctx_t;

typedef struct                   /* Vector BLF (data_logger_blf.c)    */
{
    void    *f;
    uint64_t bytes;              /* file bytes incl. the 144 B header */
    uint64_t uncomp;             /* sum of container payload bytes    */
    uint32_t objects;            /* CAN_MESSAGE count                 */
    int64_t  start_ms;           /* the header start as last patched  */
    uint32_t mono_open;          /* messages are relative to this     */
    int64_t  last_rel_ms;        /* the newest message's offset       */
    uint32_t cfill;              /* container accumulation fill       */
    char    *buf;                /* lazy 4 KB DMA stdio buffer        */
} dl_blf_ctx_t;

typedef struct
{
    void    *db;                 /* sqlite3*                          */
    void    *ins_rec;            /* prepared INSERT (records)         */
    void    *ins_frame;          /* prepared INSERT (frames)          */
    uint64_t rows;
    uint64_t frame_rows;
    bool     corrupt;            /* last error was SQLITE_CORRUPT/NOTADB */
} dl_sq_ctx_t;

/* rule-driven gate (logger.enable / logger.disable actions). While the
 * gate is off the writer parks but producers keep filling the rings, so
 * re-enabling flushes the newest records = pre-trigger context. */
void dl_runtime_gate(bool on);

/* frame-ring producers (data_logger_can.c drain task + CLI frametest) */
esp_err_t dl_frame_push(const dl_record_t *rec);

/* CAN stream lifecycle (data_logger_can.c): settings snapshot applied
 * by data_logger.c's on_apply, task created by start when can_log */
typedef struct
{
    bool     log;
    uint32_t filter;
    uint32_t mask;
    bool     ext;
    bool     monitor_all;        /* can_filter == ""                  */
} dl_can_cfg_t;

void      dl_can_configure(const dl_can_cfg_t *cfg);
esp_err_t dl_can_start(void);    /* creates the drain task            */
uint32_t  dl_can_queued(void);   /* frames waiting in the sub queue   */
esp_err_t dl_can_test_push(int n); /* synthetic frames (CLI/bench)    */

/* ---- settings (data_logger_settings.c) ------------------------------------ */

/** Boot-applied configuration (from the settings descriptor). */
typedef struct
{
    bool         enabled;
    char         format[8];       /* param stream engine name          */
    char         can_format[8];   /* CAN stream engine name            */
    uint32_t     max_file_mb;     /* param stream rotation/retention   */
    uint32_t     max_files;
    uint32_t     can_max_file_mb; /* CAN stream rotation/retention     */
    uint32_t     can_max_files;
    uint32_t     batch_rows;
    uint32_t     flush_ms;
    uint32_t     ring_len;        /* frame ring live length (<= cap)   */
    uint8_t      autopid_log;     /* 0 off / 1 changed / 2 all         */
    dl_can_cfg_t can;
} dl_cfg_t;

/** Register the "data_logger" descriptor with settings_manager. */
esp_err_t dl_settings_register(void);

/** Boot-applied config; valid once dl_settings_is_configured(). */
const dl_cfg_t *dl_settings_config(void);
bool dl_settings_is_configured(void); /* boot apply ran (standard 4.3) */

/** Apply-time engine/stream re-bind: data_logger.c owns the stream
 *  table and the rings; on_apply calls this after parsing. */
void dl_core_apply(const dl_cfg_t *cfg);

/* ---- the writer's streams (data_logger_writer.c) and the card-side file
   operations on them (data_logger_store.c) --------------------------------
   One stream per record kind, bound to an engine + context at boot
   (`format` / `can_format`) and fed by the core's ring of that kind. The
   writer task drains the rings into the open files in batches; the store
   opens or resumes a stream's file, rotates it, ages the old ones out,
   sets a corrupt one aside and latches the faults. Writer task only. */

enum { DL_ST_PARAM = 0, DL_ST_CAN = 1, DL_ST_COUNT = 2 };

typedef struct
{
    const dl_engine_t *eng;
    void              *ctx;
    const char        *prefix;    /* DL_PREFIX_PARAM / DL_PREFIX_CAN  */
    bool               frames;    /* record kind this stream carries  */
    bool               active;    /* logging this stream this boot    */
    uint32_t           max_mb;
    uint32_t           max_files;
    char               cur_path[80];
    /* per-stream stats */
    char               file[48];
    uint32_t           file_rows;
    uint32_t           files;
    uint32_t           rotations;
    uint32_t           written;
    /* ROBUSTNESS.md */
    uint32_t           open_fails;    /* consecutive open failures        */
    uint32_t           corrupt_files; /* *.corrupt set aside for this stream */
    char               last_try[48];  /* file name of the last open attempt */
} dl_stream_t;

/* the storage counters the writer and the store share */
typedef struct
{
    bool     storage_ok;         /* card mounted and the active files open */
    uint32_t errors;             /* storage errors since boot (both streams) */
    uint32_t corrupt;            /* *.corrupt files set aside on the card  */
} dl_store_stats_t;

/* data_logger_store.c */
#define DL_OPEN_FAILS_FAULT 3   /* consecutive open failures that latch a fault */
esp_err_t dl_store_open(dl_stream_t *st, bool force_new); /* the newest, or fresh */
void      dl_store_close(dl_stream_t *st);
void      dl_store_rotate(dl_stream_t *st);   /* close, a fresh file, the event */
bool      dl_store_ensure_open(dl_stream_t *st); /* a corrupt file is set aside on
                                                    the spot; repeated failures
                                                    latch a fault */
void      dl_store_quarantine(dl_stream_t *st);
void      dl_store_fault(const char *code, const char *detail); /* an internal-
                                                    stack task latches it */
dl_store_stats_t *dl_store_stats(void);

/* data_logger_writer.c */
void        dl_writer_bind(const dl_cfg_t *cfg);  /* engines, limits, active streams */
esp_err_t   dl_writer_start(void);                /* the task (+ the CAN front end) */
void        dl_writer_stop(void);                 /* synchronous: files flushed, closed */
void        dl_writer_set_gate(bool on);          /* the runtime gate; wakes the writer */
bool        dl_writer_gated(void);
bool        dl_writer_stream_active(int st);
const char *dl_writer_engine_name(int st);        /* "jsonl", "mf4", ...            */
void        dl_writer_stats(data_logger_stats_t *out); /* the stream + storage fields */

/* the core's rings and registry as the writer sees them (data_logger.c) */
bool              dl_core_pop(int st, dl_record_t *rec);
uint32_t          dl_core_fill(int st);
dl_param_entry_t *dl_core_param(int idx);
void              dl_core_forget_db_ids(void);  /* after every open of the params file */

/* ---- recovery helpers (data_logger_recover.c: PURE, host-tested) ---------- */
size_t   dl_recover_text_keep(const char *buf, size_t len);
size_t   dl_recover_wdl_scan(const uint8_t *buf, size_t len, bool *bad);
bool     dl_recover_corrupt_name(const char *fname, char *out, size_t cap);
bool     dl_recover_is_corrupt_name(const char *fname, const char *prefix,
                                    int64_t *epoch_out);
bool     dl_recover_record_sane(const dl_record_t *r);
bool     dl_recover_ring_sane(uint32_t cap, uint32_t cap_limit, uint32_t head,
                              uint32_t tail, uint32_t *fill);
uint32_t dl_recover_crc32_update(uint32_t crc, const void *data, size_t len);
/* sqlite salvage (data_logger_db.c): copy what can still be read from @p src
 * into a fresh @p dst with the stream's schema; rows copied in *rows_out */
esp_err_t dl_sq_salvage(const char *src, const char *dst, bool frames,
                        uint32_t budget_ms, uint32_t *rows_out);

/* ---- data_logger_files.c: PURE (host-tested) ---------------------------- */

/* strict parse: <prefix><10 digits> + a known engine extension; any
 * known prefix (dl_/can_) is accepted */
bool dl_files_parse(const char *fname, int64_t *epoch_out);
/* writes "<prefix>%010lld<ext>" (zero-padded: lexical order = age) */
void dl_files_make(char *out, size_t len, const char *prefix,
                   int64_t epoch, const char *ext);
/* where an export starts in the ascending list of a stream's file epochs
 * for a `since` value (2026-11-06): an EXACT cursor ("<epoch>:<offset>")
 * resumes at that file with its offset, a retired epoch at the next newer
 * file from its start; a PLAIN epoch ("the rows from that moment": a
 * client that remembers the time of its last import) starts at the newest
 * file opened at or before it, from its start, since a file holds the rows
 * written while it was open; nothing or 0 starts at the oldest file.
 * Returns the index (n when every file is older than an exact cursor) and
 * leaves *off alone only for an exact hit. */
int dl_export_start(const int64_t *epochs, int n, int64_t since_epoch,
                    bool exact, long *off);

/* ---- data_logger_export.c: GET /api/logger/export ------------------------ */
struct httpd_req;
/* the export's pure line rules (data_logger_cursor.c, host-tested): the
   `?name=` filter, the clock-step marker, and the walk of one file window
   against a page's line and byte budgets through a sink (one call per
   line; false = the client went away). The walk returns the bytes
   consumed: 0 with `stop` set when the first complete line did not fit. */
bool dl_export_line_matches(const char *line, size_t len, const char *name,
                            bool csv);
bool dl_export_line_is_marker(const char *line, size_t len, bool csv);
typedef bool (*dl_export_send_fn)(void *ctx, const char *data, size_t len);
size_t dl_export_walk(const char *buf, size_t n, const char *name, bool csv,
                      int limit, int *lines_left, int *bytes_left,
                      dl_export_send_fn send, void *ctx, bool *stop,
                      bool *aborted);
esp_err_t dl_export_get_handler(struct httpd_req *req);
void dl_export_stats(data_logger_stats_t *out);   /* the export_* fields */
/* the params files' epochs, ascending (the walk both routes share) */
int dl_export_list_epochs(int64_t *out, int cap, const char *ext);
/* the ACTIVE file is held by the writer: pause the gate and open it
   (sets *gated_here; the caller restores the gate); NULL when it never
   opened. The caller gates once per request. */
FILE *dl_export_open_locked(const char *path, bool *gated_here);
/* a read happened: the counters, the last cursor, the reader hook (rows > 0) */
void dl_export_note_read(uint32_t rows, uint32_t bytes, int64_t epoch, long off);

/* ---- data_logger_gpx.c: GET /api/logger/gpx (the GPS track) -------------- */
esp_err_t dl_gpx_get_handler(struct httpd_req *req);

typedef struct
{
    char        oldest[48];
    char        newest[48];
    int         count;
    const char *prefix;          /* only names with this prefix fold  */
} dl_scan_t;

void dl_scan_init(dl_scan_t *s, const char *prefix);
/* fold one directory entry into the scan; ignores foreign names */
void dl_scan_add(dl_scan_t *s, const char *fname);

/* "7E8"/"0x7E8" → id; false on empty/garbage/overflow */
bool dl_parse_hex_u32(const char *s, uint32_t *out);

/* pure encoders (host-tested golden vectors) */
/* .wdl 0x03 frame: returns bytes written into buf (>= 22 cap) */
size_t dl_wdl_encode_frame(uint8_t *buf, int64_t ts_ms, uint32_t id,
                           uint8_t flags, const uint8_t *data,
                           uint8_t dlc);
/* csv frame row "ts,id_hex,ext,rtr,dlc,data_hex\n"; returns strlen */
int dl_csv_frame_row(char *out, size_t cap, int64_t ts_ms, uint32_t id,
                     uint8_t flags, const uint8_t *data, uint8_t dlc);
/* candump "(sec.usec) can0 ID#DATA\n" (can-utils / SavvyCAN) */
int dl_candump_row(char *out, size_t cap, int64_t ts_ms, uint32_t id,
                   uint8_t flags, const uint8_t *data, uint8_t dlc);
/* Vector .asc data row (rel_ms = ms since the file's open, channel 1) */
int dl_asc_row(char *out, size_t cap, int64_t rel_ms, uint32_t id,
               uint8_t flags, const uint8_t *data, uint8_t dlc);
/* JSON-lines rows */
int dl_jsonl_param_row(char *out, size_t cap, int64_t ts_ms,
                       const char *source, const char *name,
                       double value);
int dl_jsonl_frame_row(char *out, size_t cap, int64_t ts_ms,
                       uint32_t id, uint8_t flags, const uint8_t *data,
                       uint8_t dlc);

/* ---- MDF 4.10 builders (pure, little-endian) ------------------------------ */

#define DL_MF4_PRELUDE_MAX 2048
#define DL_MF4_REC_SIZE    22
/* where the prelude keeps the start time (ns since the epoch, u64 LE):
 * the HD block's and the FH block's; commit() re-patches both when the
 * wall clock moved after the open */
#define DL_MF4_HD_START_OFF 136
#define DL_MF4_FH_START_OFF 208

/* ID..CN..DT-header prelude; returns total length and the two file
 * offsets commit() patches (DT block length u64, CG cycle_count u64) */
size_t dl_mf4_prelude(uint8_t *buf, size_t cap, int64_t start_ms,
                      uint32_t *dt_len_off, uint32_t *cg_cycle_off);
/* one 22-byte record: t f64 (s since start) | id u32 | flags u8 |
 * dlc u8 | data[8]; rel_ms = the record's ms since the file's open */
size_t dl_mf4_record(uint8_t *buf, int64_t rel_ms, const dl_record_t *rec);

/* ---- Vector BLF builders (pure, little-endian) ----------------------------- */

#define DL_BLF_HDR_SIZE  144
#define DL_BLF_MSG_SIZE  48
#define DL_BLF_CONT_HDR  32

/* the 144-byte "LOGG" file header (re-written on every commit) */
size_t dl_blf_file_header(uint8_t *buf, int64_t start_ms, int64_t end_ms,
                          uint64_t file_size, uint64_t uncompressed,
                          uint32_t objects);
/* LOG_CONTAINER header for an UNCOMPRESSED payload of payload_len */
size_t dl_blf_container_header(uint8_t *buf, uint32_t payload_len);
/* one 48-byte CAN_MESSAGE object (ns timestamp = rel_ms since the start,
 * clamped at 0: the format's timestamp is unsigned) */
size_t dl_blf_message(uint8_t *buf, int64_t rel_ms, const dl_record_t *rec);

/* ---- glue ----------------------------------------------------------------- */

void dl_events_register(void);                       /* declare sources  */
void dl_events_rotated(const char *file);            /* publish          */
void dl_events_error(const char *what, int code);    /* publish (rare)   */
