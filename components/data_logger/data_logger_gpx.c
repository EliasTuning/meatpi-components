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
 * @file data_logger_gpx.c
 * @brief GET /api/logger/gpx (2026-11-06, Ali: "let's also do the GPX GPS
 *        time series"): the GPS fix recorded in the params stream as the
 *        `gps_latitude` / `gps_longitude` / `gps_altitude` / `gps_speed` /
 *        `gps_heading` / `gps_satellites` rows (the fix is published as
 *        AutoPID parameters, so Store and forward records it) served back
 *        as a GPX 1.1 track: the time series of positions a map tool, a
 *        trip viewer or the Home Assistant integration's `save_gpx` service
 *        can take, which Home Assistant's own history cannot (a location
 *        has no statistics and a past state cannot be written).
 *
 * Shape: `?since=<cursor | epoch>&until=<epoch s>&limit=<points>`, the
 * same `since` as the export (a plain epoch = from that moment). The rows
 * of the same fix share a stamp to the millisecond (one publish per field);
 * the route gathers rows into points by stamp (a tolerance of
 * DL_GPX_SAME_MS covers a fix whose fields straddle a tick), keeps up to
 * DL_GPX_MAX_POINTS of them sorted in a static PSRAM table, then writes
 * the document: a new track segment where two points are more than
 * DL_GPX_GAP_MS apart (a new drive). A trailing XML comment
 * `<!-- cursor="<epoch>:<offset>" more="true|false" points="N" -->` names
 * where the scan stopped, so a client can continue in a second document.
 * One request at a time (409 for a second). Counted and reported as an
 * export read (the sleep activity hold, the export counters).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "data_logger.h"
#include "data_logger_private.h"

static const char *TAG = "data_logger";

#define DL_GPX_MAX_POINTS  4000
#define DL_GPX_POINTS_DEF  2000
#define DL_GPX_MAX_FILES   64
#define DL_GPX_SCAN_CAP    (3 * 1024 * 1024)   /* bytes read per request */
#define DL_GPX_SAME_MS     200                  /* rows of one fix: a 1 Hz
                                                   track must not merge
                                                   neighbours (run 4)    */
#define DL_GPX_GAP_MS      (10 * 60 * 1000)     /* a new drive            */

/* which fields a point holds */
#define GPX_LAT   0x01
#define GPX_LON   0x02
#define GPX_ELE   0x04
#define GPX_SPEED 0x08
#define GPX_HEAD  0x10
#define GPX_SATS  0x20

typedef struct
{
    int64_t  ts_ms;
    float    lat;
    float    lon;
    float    ele;
    float    speed_kmph;
    float    heading;
    uint8_t  sats;
    uint8_t  have;
} gpx_point_t;

static gpx_point_t s_pts[DL_GPX_MAX_POINTS] EXT_RAM_BSS_ATTR;
static int s_npts;
static volatile bool s_busy;

/* the point for a stamp: the one within DL_GPX_SAME_MS of it, else a new
   one inserted in order (rows arrive by time as a rule; a series replayed
   after another lands in between). NULL when the table is full. */
static gpx_point_t *gpx_point_for(int64_t ts_ms)
{
    int lo = 0;
    int hi = s_npts;

    while (lo < hi)
    {
        int mid = (lo + hi) / 2;

        if (s_pts[mid].ts_ms < ts_ms)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }

    /* lo = the first point at or after ts: it or the one before may be it */
    if (lo < s_npts && s_pts[lo].ts_ms - ts_ms <= DL_GPX_SAME_MS)
    {
        return &s_pts[lo];
    }

    if (lo > 0 && ts_ms - s_pts[lo - 1].ts_ms <= DL_GPX_SAME_MS)
    {
        return &s_pts[lo - 1];
    }

    if (s_npts >= DL_GPX_MAX_POINTS)
    {
        return NULL;
    }

    if (lo < s_npts)
    {
        memmove(&s_pts[lo + 1], &s_pts[lo],
                (size_t)(s_npts - lo) * sizeof(gpx_point_t));
    }

    memset(&s_pts[lo], 0, sizeof(gpx_point_t));
    s_pts[lo].ts_ms = ts_ms;
    s_npts++;
    return &s_pts[lo];
}

/* one row of the file into the table: true when it was a GPS row that fit */
static bool gpx_take_line(const char *line, size_t len, bool csv, int64_t until_ms,
                          bool *full)
{
    char buf[256];
    int64_t ts;
    const char *name;
    double value;

    if (len >= sizeof(buf))
    {
        return false;
    }

    memcpy(buf, line, len);
    buf[len] = '\0';

    if (csv)
    {
        /* ts_ms,source.name,value */
        char *c1 = strchr(buf, ',');
        char *c2 = c1 ? strchr(c1 + 1, ',') : NULL;

        if (c1 == NULL || c2 == NULL)
        {
            return false;
        }

        *c1 = '\0';
        *c2 = '\0';
        ts = strtoll(buf, NULL, 10);
        name = c1 + 1;
        value = strtod(c2 + 1, NULL);
    }
    else
    {
        const char *p = strstr(buf, "\"ts\":");
        const char *q = strstr(buf, "\"param\":\"");
        const char *v = strstr(buf, "\"value\":");

        if (p == NULL || q == NULL || v == NULL)
        {
            return false;
        }

        ts = strtoll(p + 5, NULL, 10);
        value = strtod(v + 8, NULL);
        name = q + 9;

        char *e = strchr((char *)name, '"');

        if (e == NULL)
        {
            return false;
        }

        *e = '\0';
    }

    const char *dot = strrchr(name, '.');

    if (dot != NULL)
    {
        name = dot + 1;
    }

    if (strncmp(name, "gps_", 4) != 0 || ts <= 0 || (until_ms > 0 && ts > until_ms))
    {
        return false;
    }

    uint8_t field;

    if (strcmp(name, "gps_latitude") == 0)        field = GPX_LAT;
    else if (strcmp(name, "gps_longitude") == 0)  field = GPX_LON;
    else if (strcmp(name, "gps_altitude") == 0)   field = GPX_ELE;
    else if (strcmp(name, "gps_speed") == 0)      field = GPX_SPEED;
    else if (strcmp(name, "gps_heading") == 0)    field = GPX_HEAD;
    else if (strcmp(name, "gps_satellites") == 0) field = GPX_SATS;
    else return false;

    gpx_point_t *pt = gpx_point_for(ts);

    if (pt == NULL)
    {
        *full = true;
        return false;
    }

    switch (field)
    {
        case GPX_LAT:   pt->lat = (float)value; break;
        case GPX_LON:   pt->lon = (float)value; break;
        case GPX_ELE:   pt->ele = (float)value; break;
        case GPX_SPEED: pt->speed_kmph = (float)value; break;
        case GPX_HEAD:  pt->heading = (float)value; break;
        default:        pt->sats = (uint8_t)value; break;
    }

    pt->have |= field;
    return true;
}

static void gpx_time(char *out, size_t cap, int64_t ts_ms)
{
    time_t t = (time_t)(ts_ms / 1000);
    struct tm tm;

    gmtime_r(&t, &tm);
    snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
             tm.tm_min, tm.tm_sec, (int)(ts_ms % 1000));
}

static esp_err_t gpx_send(httpd_req_t *req, const char *s)
{
    return httpd_resp_send_chunk(req, s, HTTPD_RESP_USE_STRLEN);
}

esp_err_t dl_gpx_get_handler(struct httpd_req *req)
{
    const dl_cfg_t *cfg = dl_settings_config();
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

    if (s_busy)
    {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"a track is being built\"}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    s_busy = true;

    char query[160] = "";
    char val[48];
    int64_t since_epoch = 0;
    long since_off = 0;
    bool exact = false;
    int64_t until_ms = 0;
    int limit = DL_GPX_POINTS_DEF;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
    {
        if (httpd_query_key_value(query, "since", val, sizeof(val)) == ESP_OK)
        {
            exact = strchr(val, ':') != NULL;
            sscanf(val, "%lld:%ld", (long long *)&since_epoch, &since_off);

            if (!exact && since_epoch > 100000000000LL)
            {
                since_epoch /= 1000;
            }
        }

        if (httpd_query_key_value(query, "until", val, sizeof(val)) == ESP_OK)
        {
            until_ms = strtoll(val, NULL, 10);

            if (until_ms > 0 && until_ms < 100000000000LL)
            {
                until_ms *= 1000; /* seconds given */
            }
        }

        if (httpd_query_key_value(query, "limit", val, sizeof(val)) == ESP_OK)
        {
            limit = atoi(val);

            if (limit < 1 || limit > DL_GPX_MAX_POINTS)
            {
                limit = DL_GPX_POINTS_DEF;
            }
        }
    }

    int64_t epochs[DL_GPX_MAX_FILES];
    int nfiles = dl_export_list_epochs(epochs, DL_GPX_MAX_FILES, ext);
    int idx = dl_export_start(epochs, nfiles, since_epoch, exact, &since_off);
    long off = since_off;
    int scan_left = DL_GPX_SCAN_CAP;
    bool more = false;
    bool full = false;
    bool gated_here = false;
    int64_t cur_epoch = (idx < nfiles) ? epochs[idx] : since_epoch;

    s_npts = 0;

    while (idx < nfiles && scan_left > 0 && !full)
    {
        char fname[40];
        char path[64];

        cur_epoch = epochs[idx];
        dl_files_make(fname, sizeof(fname), DL_PREFIX_PARAM, cur_epoch, ext);
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

        while (scan_left > 0 && off < fsize && !full)
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

            fseek(f, off, SEEK_SET);

            size_t n = fread(buf, 1, want, f);

            if (n == 0)
            {
                break;
            }

            size_t consumed = 0;

            for (size_t i = 0; i < n && !full; i++)
            {
                if (buf[i] != '\n')
                {
                    continue;
                }

                gpx_take_line(buf + consumed, i + 1 - consumed, csv, until_ms, &full);

                if (!full)
                {
                    consumed = i + 1;
                }
            }

            if (consumed == 0 && !full)
            {
                /* no line boundary in the window (a long foreign record or a
                   partial tail): skip it whole, or leave the tail */
                if (off + (long)n < fsize && n == sizeof(buf))
                {
                    consumed = n;
                }
                else
                {
                    break;
                }
            }

            off += (long)consumed;
            scan_left -= (int)consumed;

            if (s_npts >= limit)
            {
                full = true;
            }
        }

        bool file_done = (off >= fsize);

        fclose(f);

        if (!file_done)
        {
            more = true;
            break;
        }

        if (idx + 1 < nfiles)
        {
            idx++;
            off = 0;
        }
        else
        {
            break;
        }
    }

    if (gated_here)
    {
        dl_runtime_gate(true);
    }

    /* the document */
    char line[320];
    char when[32];
    /* httpd keeps a POINTER to a header's value until the headers go out
       with the first chunk: this buffer must outlive that moment (one
       request at a time, s_busy) */
    static char s_disposition[64];
    int points = 0;
    int64_t prev_ms = 0;
    bool seg_open = false;

    httpd_resp_set_type(req, "application/gpx+xml");
    gpx_time(when, sizeof(when), (s_npts > 0) ? s_pts[0].ts_ms : 0);
    snprintf(s_disposition, sizeof(s_disposition),
             "attachment; filename=\"wican_%.10s.gpx\"", when);
    httpd_resp_set_hdr(req, "Content-Disposition", s_disposition);
    snprintf(line, sizeof(line),
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<gpx version=\"1.1\" creator=\"WiCAN PRO\" "
             "xmlns=\"http://www.topografix.com/GPX/1/1\">\n"
             "<metadata><name>WiCAN track</name><time>%s</time></metadata>\n"
             "<trk><name>WiCAN track</name>\n", when);
    gpx_send(req, line);

    for (int i = 0; i < s_npts; i++)
    {
        const gpx_point_t *pt = &s_pts[i];

        if ((pt->have & (GPX_LAT | GPX_LON)) != (GPX_LAT | GPX_LON))
        {
            continue; /* no position: nothing to plot */
        }

        if (seg_open && pt->ts_ms - prev_ms > DL_GPX_GAP_MS)
        {
            gpx_send(req, "</trkseg>\n");
            seg_open = false;
        }

        if (!seg_open)
        {
            gpx_send(req, "<trkseg>\n");
            seg_open = true;
        }

        gpx_time(when, sizeof(when), pt->ts_ms);

        int n = snprintf(line, sizeof(line),
                         "<trkpt lat=\"%.6f\" lon=\"%.6f\">", pt->lat, pt->lon);

        if (pt->have & GPX_ELE)
        {
            n += snprintf(line + n, sizeof(line) - (size_t)n, "<ele>%.1f</ele>",
                          pt->ele);
        }

        n += snprintf(line + n, sizeof(line) - (size_t)n, "<time>%s</time>", when);

        if (pt->have & (GPX_SPEED | GPX_HEAD | GPX_SATS))
        {
            n += snprintf(line + n, sizeof(line) - (size_t)n, "<extensions>");

            if (pt->have & GPX_SPEED)
            {
                /* GPX speed is m/s; the parameter is km/h */
                n += snprintf(line + n, sizeof(line) - (size_t)n,
                              "<speed>%.2f</speed>", pt->speed_kmph / 3.6f);
            }

            if (pt->have & GPX_HEAD)
            {
                n += snprintf(line + n, sizeof(line) - (size_t)n,
                              "<course>%.1f</course>", pt->heading);
            }

            if (pt->have & GPX_SATS)
            {
                n += snprintf(line + n, sizeof(line) - (size_t)n,
                              "<sat>%u</sat>", (unsigned)pt->sats);
            }

            n += snprintf(line + n, sizeof(line) - (size_t)n, "</extensions>");
        }

        snprintf(line + n, sizeof(line) - (size_t)n, "</trkpt>\n");

        if (gpx_send(req, line) != ESP_OK)
        {
            break; /* the client went away */
        }

        prev_ms = pt->ts_ms;
        points++;
    }

    if (seg_open)
    {
        gpx_send(req, "</trkseg>\n");
    }

    snprintf(line, sizeof(line),
             "</trk>\n</gpx>\n<!-- cursor=\"%lld:%ld\" more=\"%s\" points=\"%d\" -->\n",
             (long long)cur_epoch, off, (more || full) ? "true" : "false", points);
    gpx_send(req, line);
    httpd_resp_send_chunk(req, NULL, 0);

    dl_export_note_read((uint32_t)points, 0, cur_epoch, off);
    ESP_LOGD(TAG, "gpx: %d points from %d row-points, %s more", points, s_npts,
             (more || full) ? "" : "no");
    s_busy = false;
    return ESP_OK;
}
