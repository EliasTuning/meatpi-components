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
 * @file data_logger_http.c
 * @brief The optional /api/logger routes (§9.1): the status, the runtime
 *        gate, and the registration of the export route that lives in
 *        data_logger_export.c (moved there 2026-11-06). File
 *        browse/download/delete deliberately NOT duplicated here: the
 *        files live under /sd/logs and the existing /api/fs file
 *        manager routes cover them.
 */
#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "http_server_manager.h"

#include "data_logger.h"
#include "data_logger_private.h"

static const char *TAG = "data_logger";

static esp_err_t logger_handler(httpd_req_t *req)
{
    data_logger_stats_t st;
    char body[1152];

    (void)data_logger_stats(&st);
    snprintf(body, sizeof(body),
             "{\"enabled\":%s,\"running\":%s,\"paused\":%s,"
             "\"storage_ok\":%s,"
             "\"format\":\"%s\",\"autopid_log\":\"%s\","
             "\"file\":\"%s\",\"file_rows\":%lu,\"files\":%lu,"
             "\"queued\":%lu,"
             "\"written\":%lu,\"dropped\":%lu,\"errors\":%lu,"
             "\"rotations\":%lu,"
             "\"can\":{\"enabled\":%s,\"file\":\"%s\","
             "\"file_rows\":%lu,\"files\":%lu,\"queued\":%lu,"
             "\"frames_written\":%lu,\"frames_dropped\":%lu,"
             "\"rotations\":%lu},"
             "\"salvaged\":%lu,\"corrupt\":%lu,"
             "\"clock\":{\"valid\":%s,\"source\":\"%s\","
             "\"steps_this_boot\":%lu,\"rotations_by_clock\":%lu,"
             "\"journal_bytes\":%lu,\"journal\":\"/sd/logs/"
             DL_JOURNAL_NAME "\"},"
             "\"export\":{\"reads\":%lu,\"rows\":%lu,\"bytes\":%lu,"
             "\"age_s\":%ld,\"cursor\":\"%s\"},"
             "\"dir\":\"/sd/logs\"}",
             st.enabled ? "true" : "false",
             st.running ? "true" : "false",
             st.paused ? "true" : "false",
             st.storage_ok ? "true" : "false",
             st.format, st.autopid_log,
             st.file,
             (unsigned long)st.file_rows, (unsigned long)st.files,
             (unsigned long)st.queued,
             (unsigned long)st.written, (unsigned long)st.dropped,
             (unsigned long)st.errors, (unsigned long)st.rotations,
             st.can_enabled ? "true" : "false",
             st.can_file,
             (unsigned long)st.can_file_rows,
             (unsigned long)st.can_files,
             (unsigned long)st.can_queued,
             (unsigned long)st.frames_written,
             (unsigned long)st.frames_dropped,
             (unsigned long)st.can_rotations,
             (unsigned long)st.salvaged, (unsigned long)st.corrupt,
             st.clock_valid ? "true" : "false", st.clock_source,
             (unsigned long)st.clock_steps,
             (unsigned long)st.clock_rotations,
             (unsigned long)st.journal_bytes,
             (unsigned long)st.export_reads, (unsigned long)st.export_rows,
             (unsigned long)st.export_bytes, (long)st.export_age_s,
             st.export_cursor);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

/* POST /api/logger/gate {"enabled":bool}: runtime (non-persisted)
 * logging gate, the HTTP twin of the logger.enable/logger.disable event
 * actions (API-first §1b: anything a rule can do, a client can do). The
 * observable state is GET /api/logger's "paused" field. */
static esp_err_t gate_post_handler(httpd_req_t *req)
{
    char body[128];
    size_t len = req->content_len;

    if (len == 0 || len >= sizeof(body))
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "missing/oversized body");
        return ESP_FAIL;
    }

    size_t got = 0;

    while (got < len)
    {
        int r = httpd_req_recv(req, body + got, len - got);

        if (r <= 0)
        {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "body read failed");
            return ESP_FAIL;
        }

        got += (size_t)r;
    }

    body[len] = '\0';

    /* one bool field: a full cJSON parse is overkill */
    bool on;

    if (strstr(body, "\"enabled\"") == NULL)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "need bool 'enabled'");
        return ESP_FAIL;
    }
    else if (strstr(body, "true") != NULL)
    {
        on = true;
    }
    else if (strstr(body, "false") != NULL)
    {
        on = false;
    }
    else
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "need bool 'enabled'");
        return ESP_FAIL;
    }

    dl_runtime_gate(on);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

esp_err_t data_logger_register_http(void)
{
    static const httpd_uri_t URIS[] =
    {
        { .uri = "/api/logger", .method = HTTP_GET,
          .handler = logger_handler },
        { .uri = "/api/logger/gate", .method = HTTP_POST,
          .handler = gate_post_handler },
        { .uri = "/api/logger/export", .method = HTTP_GET,
          .handler = dl_export_get_handler },
        { .uri = "/api/logger/gpx", .method = HTTP_GET,
          .handler = dl_gpx_get_handler },
    };

    esp_err_t err = http_server_manager_register_handlers(
        URIS, sizeof(URIS) / sizeof(URIS[0]));

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "/api/logger registered");
    }

    return err;
}
