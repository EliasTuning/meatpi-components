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
 * @file data_logger_export.c
 * @brief GET /api/logger/export: the incremental pull of the params stream
 *        (device-contract ask #8; moved out of data_logger_http.c on
 *        2026-11-06 when the route grew the store-and-forward pieces).
 *
 * The consumer is the Home Assistant integration backfilling a drive's
 * rows into its long-term statistics once the car is home (store and
 * forward), and the web dashboard's history charts. Shape (unchanged since
 * 2026-07-18): the rows verbatim (`{"ts":<epoch ms>,"param":"<source>.<name>",
 * "value":n}` for jsonl, `ts_ms,source.name,value` for csv), a
 * `{"_file":"<name>"}` line ahead of each file's rows, and a FINAL
 * `{"_cursor":"<file-epoch>:<offset>","more":bool}` line: the cursor goes
 * back as `since`. A plain epoch as `since` (no colon, 2026-11-06) starts
 * at the file that was open at that moment: a client that only remembers
 * the time of its last import reads from there without walking the whole
 * card. The route counts what it served (`/api/logger` `export{}`) and
 * tells the composition root about every read that carried rows
 * (data_logger_set_reader_cb: main hands it to sleep_manager, which keeps
 * a counting device awake a little longer so a backfill can finish).
 *
 * /sd reads are httpd-task-safe (only internal flash has the cache
 * corollary); the counters are plain RAM.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "data_logger.h"
#include "data_logger_private.h"

static const char *TAG = "data_logger";

#define DL_EXPORT_MAX_FILES  64
#define DL_EXPORT_BYTE_CAP   65536          /* emitted bytes per request  */
#define DL_EXPORT_SCAN_CAP   (512 * 1024)   /* bytes read per request     */
#define DL_EXPORT_LINE_CAP   2000
#define DL_EXPORT_LINE_DEF   500

/* what the route served since boot (data_logger_stats fills the public
 * fields from it; written on the httpd task, read from anywhere: plain
 * words, a torn read is a stale counter at worst) */
static struct
{
    uint32_t reads;
    uint32_t rows;
    uint32_t bytes;
    uint32_t last_mono_ms;              /* 0 = never                     */
    char     last_cursor[32];
} s_export;

static data_logger_reader_cb_t s_reader_cb;

/* the walker's sink: one HTTP chunk per line */
static bool export_send(void *ctx, const char *data, size_t len)
{
    return httpd_resp_send_chunk((httpd_req_t *)ctx, data, len) == ESP_OK;
}

int dl_export_list_epochs(int64_t *out, int cap, const char *ext)
{
    DIR *d = opendir(DL_DIR);

    if (d == NULL)
    {
        return 0;
    }

    int n = 0;
    struct dirent *e;

    while ((e = readdir(d)) != NULL && n < cap)
    {
        int64_t epoch;
        const char *dot = strrchr(e->d_name, '.');

        if (dot != NULL && strcmp(dot, ext) == 0 &&
            strncmp(e->d_name, DL_PREFIX_PARAM,
                    strlen(DL_PREFIX_PARAM)) == 0 &&
            dl_files_parse(e->d_name, &epoch))
        {
            out[n++] = epoch;
        }
    }

    closedir(d);

    /* ascending (oldest first): insertion sort, n is small */
    for (int i = 1; i < n; i++)
    {
        int64_t v = out[i];
        int j = i - 1;

        while (j >= 0 && out[j] > v)
        {
            out[j + 1] = out[j];
            j--;
        }

        out[j + 1] = v;
    }

    return n;
}

/* the ACTIVE file is exclusively held by the writer (FATFS FS_LOCK): pause
 * the gate; the writer closes its files within one loop. Returns the file
 * once it opens, NULL when it never did. */
FILE *dl_export_open_locked(const char *path, bool *gated_here)
{
    data_logger_stats_t st;
    FILE *f = NULL;

    if (data_logger_stats(&st) != ESP_OK || st.paused)
    {
        return NULL;
    }

    dl_runtime_gate(false);
    *gated_here = true;

    /* the writer is notified and closes within its loop; the retry window
     * still covers a writer busy in a batch */
    for (int t = 0; t < 25 && f == NULL; t++)
    {
        vTaskDelay(pdMS_TO_TICKS(60));
        f = fopen(path, "rb");
    }

    return f;
}

esp_err_t dl_export_get_handler(struct httpd_req *req)
{
    const dl_cfg_t *cfg = dl_settings_config();

    /* the two line-oriented engines stream through here (jsonl since the
     * device contract, csv since 2026-09-06 for the dashboard history);
     * binary and sqlite files are fetched whole via /api/fs/download */
    const char *ext = NULL;
    bool csv = false;

    if (cfg != NULL && strcmp(cfg->format, "jsonl") == 0)
    {
        ext = ".jsonl";
    }
    else if (cfg != NULL && strcmp(cfg->format, "csv") == 0)
    {
        ext = ".csv";
        csv = true;
    }

    if (ext == NULL)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "params stream is not jsonl or csv");
        return ESP_FAIL;
    }

    char query[160] = "";
    char val[48];
    char name[48] = "";
    int64_t since_epoch = 0;
    long since_off = 0;
    bool exact = false;
    int limit = DL_EXPORT_LINE_DEF;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
    {
        if (httpd_query_key_value(query, "stream", val, sizeof(val)) ==
                ESP_OK && strcmp(val, "params") != 0)
        {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "only stream=params");
            return ESP_FAIL;
        }

        if (httpd_query_key_value(query, "since", val, sizeof(val)) ==
                ESP_OK)
        {
            /* "<epoch>:<offset>" = a cursor of ours (exact); a bare number
             * = a moment in time (seconds, or milliseconds: a client's
             * watermark), the rows from then on */
            exact = strchr(val, ':') != NULL;
            sscanf(val, "%lld:%ld", (long long *)&since_epoch, &since_off);

            if (!exact && since_epoch > 100000000000LL)
            {
                since_epoch /= 1000; /* milliseconds given */
            }
        }

        if (httpd_query_key_value(query, "limit", val, sizeof(val)) ==
                ESP_OK)
        {
            limit = atoi(val);

            if (limit < 1 || limit > DL_EXPORT_LINE_CAP)
            {
                limit = DL_EXPORT_LINE_DEF;
            }
        }

        if (httpd_query_key_value(query, "name", name, sizeof(name)) !=
                ESP_OK)
        {
            name[0] = '\0';
        }
    }

    int64_t epochs[DL_EXPORT_MAX_FILES];
    int nfiles = dl_export_list_epochs(epochs, DL_EXPORT_MAX_FILES, ext);
    int idx = dl_export_start(epochs, nfiles, since_epoch, exact, &since_off);

    httpd_resp_set_type(req, csv ? "text/plain" : "application/x-ndjson");

    long off = since_off;
    int lines_left = limit;
    int bytes_left = DL_EXPORT_BYTE_CAP;
    int scan_left = DL_EXPORT_SCAN_CAP;
    bool more = false;
    bool gated_here = false;
    bool aborted = false;   /* the client went away: stop reading the card */
    int64_t cur_epoch = (idx < nfiles) ? epochs[idx] : since_epoch;

    while (idx < nfiles && lines_left > 0 && bytes_left > 0 && scan_left > 0 &&
           !aborted)
    {
        char fname[40];
        char path[64];

        cur_epoch = epochs[idx];
        dl_files_make(fname, sizeof(fname), DL_PREFIX_PARAM, cur_epoch,
                      ext);
        snprintf(path, sizeof(path), DL_DIR "/%s", fname);

        FILE *f = fopen(path, "rb");

        if (f == NULL && !gated_here)
        {
            f = dl_export_open_locked(path, &gated_here);
        }

        if (f == NULL)
        {
            idx++;
            off = 0;
            continue;
        }

        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);

        /* which file the rows that follow come from (2026-10-13): a
           consumer with the clock journal re-bases them per file */
        char flin[64];
        int fn = snprintf(flin, sizeof(flin), "{\"_file\":\"%s\"}\n", fname);

        if (httpd_resp_send_chunk(req, flin, fn) != ESP_OK)
        {
            aborted = true;
        }

        bytes_left -= fn;

        while (!aborted && lines_left > 0 && bytes_left > 0 && scan_left > 0 &&
               off < fsize)
        {
            char buf[1024];
            size_t want = sizeof(buf);

            if ((long)want > fsize - off)
            {
                want = (size_t)(fsize - off);
            }

            if ((int)want > scan_left)
            {
                want = (size_t)scan_left;
            }

            /* every window starts exactly at the cursor: the partial tail
               of the previous window is re-read, never skipped */
            fseek(f, off, SEEK_SET);

            size_t n = fread(buf, 1, want, f);

            if (n == 0)
            {
                break;
            }

            /* walk the complete lines: forward the matching ones, keep the
               offsets exact, stop once a budget is spent (the pure
               dl_export_walk, host-tested) */
            bool stop = false;
            size_t consumed = dl_export_walk(buf, n, name, csv, limit,
                                             &lines_left, &bytes_left,
                                             export_send, req, &stop,
                                             &aborted);

            /* a window whose FIRST complete line did not fit the byte
               budget consumed nothing too, but it is not this case: the
               page ends here and that line opens the next one (until run
               5, 2026-11-06, such a window went out raw, the cut row at
               its end glued to the cursor line, and the client stopped
               paging there) */
            if (consumed == 0 && !stop)
            {
                /* no boundary in this window: a >1 KB record mid-file is
                   forwarded raw when unfiltered (offsets stay exact) or
                   skipped when filtered; a trailing partial line (active
                   file mid-append) is left for next time */
                if (off + (long)n < fsize && n == sizeof(buf))
                {
                    if (name[0] == '\0')
                    {
                        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK)
                        {
                            aborted = true;
                        }

                        bytes_left -= (int)n;
                    }

                    consumed = n;
                }
                else
                {
                    break;
                }
            }

            off += (long)consumed;
            scan_left -= (int)consumed;

            if (stop)
            {
                break;
            }
        }

        bool file_done = (off >= fsize);

        fclose(f);

        if (!file_done)
        {
            more = true; /* budget hit or partial tail pending */
            break;
        }

        if (idx + 1 < nfiles)
        {
            idx++;
            off = 0;
        }
        else
        {
            break; /* fully caught up on the newest file */
        }
    }

    if (gated_here)
    {
        dl_runtime_gate(true);
    }

    char meta[96];

    snprintf(meta, sizeof(meta),
             "{\"_cursor\":\"%lld:%ld\",\"more\":%s}\n",
             (long long)cur_epoch, off, more ? "true" : "false");

    if (!aborted)
    {
        httpd_resp_send_chunk(req, meta, HTTPD_RESP_USE_STRLEN);
        httpd_resp_send_chunk(req, NULL, 0);
    }

    /* the counters, and the word to the composition root: a read that
     * carried rows is a consumer catching up (the dashboard's idle poll
     * at the tail carries none and keeps nobody awake) */
    uint32_t rows = (uint32_t)(limit - lines_left);

    dl_export_note_read(aborted ? 0 : rows,
                        (uint32_t)(DL_EXPORT_BYTE_CAP - bytes_left), cur_epoch,
                        off);
    ESP_LOGD(TAG, "export: %lu rows, %s more, cursor %s%s",
             (unsigned long)rows, more ? "" : "no", s_export.last_cursor,
             aborted ? " (client gone)" : "");
    return aborted ? ESP_FAIL : ESP_OK;
}

void dl_export_note_read(uint32_t rows, uint32_t bytes, int64_t epoch, long off)
{
    s_export.reads++;
    s_export.rows += rows;
    s_export.bytes += bytes;
    s_export.last_mono_ms = (uint32_t)(esp_timer_get_time() / 1000) | 1u;
    snprintf(s_export.last_cursor, sizeof(s_export.last_cursor), "%lld:%ld",
             (long long)epoch, off);

    if (rows > 0 && s_reader_cb != NULL)
    {
        s_reader_cb(rows);
    }
}

void dl_export_stats(data_logger_stats_t *out)
{
    out->export_reads = s_export.reads;
    out->export_rows = s_export.rows;
    out->export_bytes = s_export.bytes;
    out->export_age_s = -1;

    if (s_export.last_mono_ms != 0)
    {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

        out->export_age_s = (int32_t)((now - s_export.last_mono_ms) / 1000u);
    }

    snprintf(out->export_cursor, sizeof(out->export_cursor), "%s",
             s_export.last_cursor);
}

esp_err_t data_logger_set_reader_cb(data_logger_reader_cb_t cb)
{
    s_reader_cb = cb;
    return ESP_OK;
}
