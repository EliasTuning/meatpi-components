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
 * @file data_logger.h
 * @brief WiCAN record logger (feature component): the generic
 *        successor of legacy `obd_logger`.
 *
 * Model (ownership inversion, Architecture §5): producers REGISTER a
 * named parameter once (source + name, e.g. "autopid"/"rpm") and push
 * timestamped numeric records with data_logger_write(); this component
 * owns everything storage: the sqlite files on the SD card
 * (`/sd/logs/dl_<epoch>.db`), write batching, size rotation and
 * retention. Producers never block on the card: records go into a
 * bounded PSRAM ring (drop-oldest + counter, the log_manager
 * backpressure rule) drained by one writer task, the only code that
 * touches sqlite (the port is compiled THREADSAFE=0).
 *
 * Three storage engines, chosen by the `format` setting (meatpi
 * 2026-07-07: sqlite REQUIRED for tooling compatibility, append logs
 * added for speed: BENCHMARKS.md, bench card):
 *   "sqlite" (default) dl_<epoch>.db:   ~700 rows/s tuned;
 *     params(id, source, name UNIQUE) +
 *     records(ts [epoch ms], param_id, value REAL) + index on ts
 *   "csv"              dl_<epoch>.csv (`ts_ms,source.name,value`
 *   "binary"           dl_<epoch>.wdl) packed frames w/ inline param
 *     dictionary (data_logger_append.c documents the framing); the
 *     fast option (raw append benched ~170× tuned sqlite) for
 *     CAN-frame-rate streams
 *
 * Files rotate at max_file_mb; retention deletes the oldest beyond
 * max_files (across engines, so switching format still ages out old
 * files). Browse/download/delete goes through the existing /api/fs
 * file manager routes; /api/logger only reports status.
 *
 * Rule-driven gating (event_manager): the `logger.enable` /
 * `logger.disable` actions pause/resume the writer at runtime ("log
 * only while driving"). While paused the ring keeps absorbing, so an
 * enable rule flushes the newest pre-trigger records too.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Parameter handle returned by registration. */
typedef int16_t dl_param_t;

typedef struct
{
    bool     enabled;      /**< settings.enabled                        */
    bool     running;      /**< writer task exists                      */
    bool     paused;       /**< gated off by a logger.disable rule      */
    bool     storage_ok;   /**< card mounted and active files open      */

    /* param stream (dl_<epoch>.<ext>, `format` setting) */
    char     file[48];     /**< current file name ("" when closed)      */
    uint32_t file_rows;    /**< rows in the current file                */
    uint32_t files;        /**< dl_* files on the card                  */
    uint32_t queued;       /**< records waiting in the ring right now   */
    uint32_t written;      /**< rows committed since boot               */
    uint32_t dropped;      /**< ring overflows since boot               */
    uint32_t errors;       /**< storage errors since boot (both streams)*/
    uint32_t rotations;    /**< file rotations since boot               */

    /* CAN stream (can_<epoch>.<ext>, `can_format`/`can_log` settings) */
    bool     can_enabled;        /**< can_log setting                   */
    char     can_file[48];       /**< current file ("" when closed)     */
    uint32_t can_file_rows;      /**< frames in the current file        */
    uint32_t can_files;          /**< can_* files on the card           */
    uint32_t can_queued;         /**< frames waiting (ring + sub queue) */
    uint32_t frames_written;     /**< frames committed since boot       */
    uint32_t frames_dropped;     /**< frame-ring overflows since boot   */
    uint32_t can_rotations;      /**< CAN-file rotations since boot     */
    /* robustness (2026-09-07, ROBUSTNESS.md) */
    uint32_t salvaged;           /**< records carried over a warm reset      */
    uint32_t corrupt;            /**< *.corrupt files set aside on the card   */
    /* the clock (2026-10-13, issue #984; README "Time") */
    bool     clock_valid;        /**< the system clock was set this boot     */
    char     clock_source[8];    /**< none / rtc / sntp / manual             */
    uint32_t clock_steps;        /**< sets of the system clock this boot     */
    uint32_t clock_rotations;    /**< files rotated because the clock moved  */
    uint32_t journal_bytes;      /**< /sd/logs/clock.jsonl as last written   */
    /* the params stream as configured, and what /api/logger/export served
     * (2026-11-06, store and forward: the Home Assistant backfill reads it) */
    char     format[8];          /**< the params stream's engine (jsonl, ...)  */
    char     autopid_log[8];     /**< off / changed / all                     */
    uint32_t export_reads;       /**< export requests since boot              */
    uint32_t export_rows;        /**< rows those requests carried             */
    uint32_t export_bytes;       /**< bytes those requests carried            */
    int32_t  export_age_s;       /**< seconds since the last read, -1 = never */
    char     export_cursor[32];  /**< where the last read ended               */
} data_logger_stats_t;

/** A consumer read rows through /api/logger/export (the Home Assistant
 *  backfill after a drive, the dashboard's history): the composition root
 *  hands this to sleep_manager so a counting device stays awake a little
 *  longer for the read to finish. httpd task context, after the response. */
typedef void (*data_logger_reader_cb_t)(uint32_t rows);

/** Register descriptors (settings/log/events). No storage access. */
esp_err_t data_logger_init(void);

/** Create the writer task if enabled. Storage may come and go later:
 *  the writer follows external_storage_is_mounted(). */
esp_err_t data_logger_start(void);

/** Flush and close the current file; the writer parks. */
esp_err_t data_logger_stop(void);

/** Register (or look up) parameter `source`.`name`. Cheap, RAM-only;
 *  callable before start. Same pair returns the same handle. */
esp_err_t data_logger_register_param(const char *source, const char *name,
                                     dl_param_t *out);

/** Queue one record stamped with the current system time (rtc_manager
 *  restores the clock at boot, so epochs are sane; a record queued
 *  before a clock step is re-based by the writer, README "Time").
 *  Never blocks; ESP_ERR_INVALID_STATE when the logger is disabled. */
esp_err_t data_logger_write(dl_param_t param, double value);

/** Same with an explicit timestamp (epoch milliseconds): the producer's
 *  word, never re-based after a clock step. */
esp_err_t data_logger_write_at(dl_param_t param, int64_t epoch_ms,
                               double value);

esp_err_t data_logger_stats(data_logger_stats_t *out);

/** autopid → params-stream glue (main wires it:
 *  `autopid_set_value_sink(data_logger_autopid_sink)`). Gated by the
 *  `autopid_log` setting (`off | changed | all`); registers params as
 *  `autopid.<name>` lazily. Poller-task context, never blocks. */
void data_logger_autopid_sink(const char *name, const char *unit,
                              double value, bool changed);

/** Optional /api/logger status route (§9.1: main wires it in HTTP
 *  compositions only). */
esp_err_t data_logger_register_http(void);

/** The export route tells @p cb about every read that carried rows
 *  (NULL = nobody listens). main wires it to sleep_manager_activity(). */
esp_err_t data_logger_set_reader_cb(data_logger_reader_cb_t cb);

/** `logger` CLI command; called internally on the settings apply when
 *  the `cli` field is true. */
esp_err_t data_logger_register_cli(void);

#ifdef __cplusplus
}
#endif
