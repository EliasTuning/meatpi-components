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
 * @file obd_chip_http.c
 * @brief GET /api/obd_chip - the chip's wire and fan-out counters (own-routes
 *        pattern, §9.1; main calls obd_chip_register_http() in HTTP
 *        compositions). Read it before and after a stream to place a loss:
 *        chip -> UART ring (rx_overflows) -> fan-out (per-subscriber
 *        dropped) -> bridge (/api/bridges) -> socket (/api/sockets).
 *        Hand-formatted JSON: the document is small and fixed-shape, no
 *        cJSON dependency needed for it. Since 2026-10-28 (issue #1023) it
 *        also carries the chip firmware update's state (`fw`), and
 *        POST /api/obd_chip/update starts one (the packaged image or a
 *        file; since 2026-11-01 there is no `force`: the chip is written
 *        only when the image is newer than what it runs, Ali). The GET
 *        never touches the wire.
 */
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "filesystem.h"
#include "http_server_manager.h"

#include "obd_chip.h"
#include "obd_chip_private.h"

static const char *TAG = "obd_chip";

#define OC_HTTP_SUBS 8
#define OC_HTTP_BODY_MAX 256

static esp_err_t obd_chip_get_handler(httpd_req_t *req)
{
    obd_chip_stats_t st;
    obd_chip_fw_status_t fw;
    obd_chip_sub_stats_t subs[OC_HTTP_SUBS];
    obd_wire_note_t wire[2];
    char wire_json[200];
    char body[1600];
    size_t n = 0;
    size_t w = 0;

    obd_chip_get_stats(&st);
    obd_chip_fw_status(&fw);

    /* the bring-up's wire verdicts (issue #1023): what a chip that is down
       did at each baud, for the page's banner and a bug report */
    size_t n_wire = obd_chip_bringup_wire(wire, 2);

    wire_json[0] = '\0';

    for (size_t i = 0; i < n_wire; i++)
    {
        w += (size_t)snprintf(wire_json + w, sizeof(wire_json) - w,
                              "%s{\"baud\":%d,\"bytes\":%lu,\"prompt\":%s,"
                              "\"hex\":\"%s\"}",
                              (i > 0) ? "," : "", wire[i].baud,
                              (unsigned long)wire[i].bytes,
                              wire[i].prompt ? "true" : "false", wire[i].hex);
    }

    size_t n_subs = obd_chip_get_subscribers(subs, OC_HTTP_SUBS);

    n += (size_t)snprintf(body + n, sizeof(body) - n,
                          "{\"ready\":%s,\"claim\":\"%s\","
                          "\"client_idle_ms\":%lu,\"baud\":%d,"
                          "\"eeprom_guard\":{\"rewrites\":%lu,\"blocked\":%lu,"
                          "\"protocol_saves\":%lu,\"memory_off\":%lu,"
                          "\"app_resets\":%lu,\"hidden_bytes\":%lu},"
                          "\"uart\":{\"rx_bytes\":%lu,\"rx_chunks\":%lu,"
                          "\"rx_max_chunk\":%u,\"rx_overflows\":%lu,"
                          "\"rx_buffered\":%lu,\"tx_bytes\":%lu},"
                          "\"fw\":{\"builtin\":\"%s\",\"current\":\"%s\","
                          "\"chip\":\"%s\",\"outdated\":%s,"
                          "\"update\":{\"running\":%s,\"lines\":%lu,"
                          "\"total\":%lu,\"source\":\"%s\",\"done\":%s,"
                          "\"last\":\"%s\"}},"
                          "\"bringup\":{\"wire\":[%s]},"
                          "\"subscribers\":[",
                          st.ready ? "true" : "false", st.claim,
                          (unsigned long)st.client_idle_ms,
                          obd_chip_active_baud(),
                          (unsigned long)st.guard_rewrites,
                          (unsigned long)st.guard_blocked,
                          (unsigned long)st.protocol_saves,
                          (unsigned long)st.memory_off,
                          (unsigned long)st.app_resets,
                          (unsigned long)st.hidden_bytes,
                          (unsigned long)st.rx_bytes,
                          (unsigned long)st.rx_chunks,
                          (unsigned)st.rx_max_chunk,
                          (unsigned long)st.rx_overflows,
                          (unsigned long)st.rx_buffered,
                          (unsigned long)st.tx_bytes,
                          obd_chip_builtin_fw_version(),
                          obd_fw_current_versions(),
                          fw.chip ? fw.chip : "",
                          obd_fw_version_is_outdated(
                              fw.chip, obd_chip_builtin_fw_version())
                              ? "true" : "false",
                          fw.running ? "true" : "false",
                          (unsigned long)fw.lines, (unsigned long)fw.total,
                          fw.source, fw.done_once ? "true" : "false",
                          fw.done_once ? esp_err_to_name(fw.last_err) : "",
                          wire_json);

    for (size_t i = 0; i < n_subs && n < sizeof(body) - 96; i++)
    {
        n += (size_t)snprintf(body + n, sizeof(body) - n,
                              "%s{\"idx\":%u,\"name\":\"%s\",\"dropped\":%lu,"
                              "\"queued\":%lu,\"depth\":%lu}",
                              (i > 0) ? "," : "", (unsigned)i, subs[i].name,
                              (unsigned long)subs[i].dropped,
                              (unsigned long)subs[i].queued,
                              (unsigned long)subs[i].depth);
    }

    n += (size_t)snprintf(body + n, sizeof(body) - n, "]}");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, (ssize_t)n);
}

static esp_err_t reply(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

/** POST /api/obd_chip/update  {"path":"/data/V2.3.24.txt"} (optional; no
 *  body = the packaged image). The chip is written only when the image's
 *  version is above the one it reports: a `force` key is refused (400),
 *  the forced rewrite is gone (2026-11-01, Ali). 202 when the job started,
 *  409 while one runs, 404 for a missing file, 400 otherwise (a file whose
 *  name carries no version among them). */
static esp_err_t obd_chip_update_handler(httpd_req_t *req)
{
    char body[OC_HTTP_BODY_MAX + 1];
    int len = 0;

    if (req->content_len > OC_HTTP_BODY_MAX)
    {
        return reply(req, "400 Bad Request", "{\"error\":\"body too large\"}");
    }

    while (len < (int)req->content_len)
    {
        int got = httpd_req_recv(req, body + len, req->content_len - (size_t)len);

        if (got <= 0)
        {
            return reply(req, "400 Bad Request", "{\"error\":\"body read\"}");
        }

        len += got;
    }

    body[len] = '\0';

    char path[72] = "";

    if (len > 0)
    {
        cJSON *root = cJSON_Parse(body);

        if (root == NULL)
        {
            return reply(req, "400 Bad Request", "{\"error\":\"invalid JSON\"}");
        }

        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "force")))
        {
            cJSON_Delete(root);
            return reply(req, "400 Bad Request",
                         "{\"error\":\"force is not supported: the chip is "
                         "written only when the image is newer than its "
                         "firmware\"}");
        }

        const cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "path");

        if (cJSON_IsString(p) && p->valuestring != NULL)
        {
            if (strlen(p->valuestring) >= sizeof(path))
            {
                cJSON_Delete(root);
                return reply(req, "400 Bad Request", "{\"error\":\"path too long\"}");
            }

            strlcpy(path, p->valuestring, sizeof(path));
        }

        cJSON_Delete(root);
    }

    if (path[0] != '\0')
    {
        unsigned v[3];

        /* the file's name carries the image's version: a name without one
           is refused before the file is even looked up */
        if (!obd_fw_version_parse(path, v))
        {
            return reply(req, "400 Bad Request",
                         "{\"error\":\"the file's name must carry its version "
                         "(V2.3.24.txt)\"}");
        }

        if (!filesystem_exists(path))
        {
            return reply(req, "404 Not Found", "{\"error\":\"no such file\"}");
        }
    }

    esp_err_t err = obd_chip_firmware_update_start(
        (path[0] != '\0') ? path : NULL);

    if (err == ESP_ERR_INVALID_STATE)
    {
        return reply(req, "409 Conflict", "{\"error\":\"update already running\"}");
    }

    if (err == ESP_ERR_INVALID_ARG)
    {
        return reply(req, "400 Bad Request",
                     "{\"error\":\"the file's name must carry its version "
                     "(V2.3.24.txt)\"}");
    }

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "update start refused (%s)", esp_err_to_name(err));
        return reply(req, "500 Internal Server Error",
                     "{\"error\":\"could not start the update\"}");
    }

    char out[160];

    snprintf(out, sizeof(out), "{\"started\":true,\"source\":\"%s\"}",
             (path[0] != '\0') ? path : "builtin");
    return reply(req, "202 Accepted", out);
}

/* ---- the wire tools (issue #1023): POST starts a run on the job task, GET
 * serves the result; the body of a GET can reach 5 KB (a listen's 512 bytes
 * as text and hex), so it is built in PSRAM, not on the httpd stack ---- */

static const char *const PROBE_KINDS[] = { "probe", "scan", "auto", "listen" };

/** JSON-safe printable text: anything outside 0x20..0x7E, the quote and
 *  the backslash become a dot. */
static void json_text(char *out, size_t out_len, const uint8_t *in, size_t n)
{
    size_t i;

    for (i = 0; i < n && i + 1 < out_len; i++)
    {
        uint8_t c = in[i];

        out[i] = (c >= 32 && c < 127 && c != '"' && c != '\\') ? (char)c : '.';
    }

    out[i] = '\0';
}

static esp_err_t obd_chip_probe_get_handler(httpd_req_t *req)
{
    static obd_probe_result_t s_copy EXT_RAM_BSS_ATTR;
    const size_t cap = 6144;
    char *body = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (body == NULL)
    {
        return reply(req, "500 Internal Server Error", "{\"error\":\"no memory\"}");
    }

    obd_chip_probe_last(&s_copy);

    size_t n = 0;

    n += (size_t)snprintf(body + n, cap - n,
                          "{\"running\":%s,\"done\":%s,\"kind\":\"%s\","
                          "\"baud\":%d,\"seconds\":%d,\"cmd\":\"%s\","
                          "\"err\":\"%s\","
                          "\"auto\":{\"edges\":%lu,\"measured\":%d,"
                          "\"detected\":%d},\"lines\":[",
                          s_copy.running ? "true" : "false",
                          s_copy.done ? "true" : "false",
                          PROBE_KINDS[(s_copy.kind <= OBD_PROBE_LISTEN) ? s_copy.kind : 0],
                          s_copy.baud, s_copy.seconds, s_copy.cmd,
                          s_copy.done ? esp_err_to_name(s_copy.err) : "",
                          (unsigned long)s_copy.edges, s_copy.measured,
                          s_copy.detected);

    for (size_t i = 0; i < s_copy.n_lines && n + 256 < cap; i++)
    {
        const obd_probe_line_t *ln = &s_copy.lines[i];
        char text[OBD_PROBE_TEXT + 1];

        json_text(text, sizeof(text), (const uint8_t *)ln->text, strlen(ln->text));
        n += (size_t)snprintf(body + n, cap - n,
                              "%s{\"baud\":%d,\"cmd\":\"%s\",\"prompt\":%s,"
                              "\"bytes\":%lu,\"text\":\"%s\",\"hex\":\"%s\"}",
                              (i > 0) ? "," : "", ln->baud, ln->cmd,
                              ln->prompt ? "true" : "false",
                              (unsigned long)ln->bytes, text, ln->hex);
    }

    n += (size_t)snprintf(body + n, cap - n, "],\"raw\":{\"baud\":%d,"
                          "\"total\":%lu,\"prompt\":%s,\"text\":\"",
                          s_copy.raw_baud, (unsigned long)s_copy.raw_total,
                          s_copy.raw_prompt ? "true" : "false");

    /* the raw bytes: printable text, then hex, each bounded by the cap */
    {
        char text[OBD_PROBE_RAW + 1];

        json_text(text, sizeof(text), s_copy.raw, s_copy.raw_len);

        if (n + strlen(text) + 2048 < cap)
        {
            n += (size_t)snprintf(body + n, cap - n, "%s", text);
        }

        n += (size_t)snprintf(body + n, cap - n, "\",\"hex\":\"");

        for (size_t i = 0; i < s_copy.raw_len && n + 8 < cap; i++)
        {
            n += (size_t)snprintf(body + n, cap - n, "%s%02X",
                                  (i > 0) ? " " : "", s_copy.raw[i]);
        }
    }

    n += (size_t)snprintf(body + n, cap - n, "\"}}");

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, body, (ssize_t)n);

    heap_caps_free(body);
    return err;
}

/** POST /api/obd_chip/probe {"kind":"probe|scan|auto|listen","baud":N,
 *  "seconds":N}: 202 when the run started, 409 while one (or a firmware
 *  update) runs, 400 otherwise. */
static esp_err_t obd_chip_probe_post_handler(httpd_req_t *req)
{
    char body[OC_HTTP_BODY_MAX + 1];
    int len = 0;

    if (req->content_len > OC_HTTP_BODY_MAX)
    {
        return reply(req, "400 Bad Request", "{\"error\":\"body too large\"}");
    }

    while (len < (int)req->content_len)
    {
        int got = httpd_req_recv(req, body + len, req->content_len - (size_t)len);

        if (got <= 0)
        {
            return reply(req, "400 Bad Request", "{\"error\":\"body read\"}");
        }

        len += got;
    }

    body[len] = '\0';

    cJSON *root = (len > 0) ? cJSON_Parse(body) : NULL;

    if (root == NULL)
    {
        return reply(req, "400 Bad Request", "{\"error\":\"invalid JSON\"}");
    }

    const cJSON *k = cJSON_GetObjectItemCaseSensitive(root, "kind");
    const cJSON *b = cJSON_GetObjectItemCaseSensitive(root, "baud");
    const cJSON *s = cJSON_GetObjectItemCaseSensitive(root, "seconds");
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    char cmd[OBD_PROBE_CMD] = "";
    int kind = -1;

    if (cJSON_IsString(c) && c->valuestring != NULL)
    {
        if (strlen(c->valuestring) >= sizeof(cmd))
        {
            cJSON_Delete(root);
            return reply(req, "400 Bad Request",
                         "{\"error\":\"cmd: one word under 16 characters\"}");
        }

        strlcpy(cmd, c->valuestring, sizeof(cmd));
    }

    for (int i = 0; cJSON_IsString(k) && k->valuestring != NULL && i < 4; i++)
    {
        if (strcmp(k->valuestring, PROBE_KINDS[i]) == 0)
        {
            kind = i;
        }
    }

    int baud = cJSON_IsNumber(b) ? b->valueint : 0;
    int seconds = cJSON_IsNumber(s) ? s->valueint : 0;

    cJSON_Delete(root);

    if (kind < 0)
    {
        return reply(req, "400 Bad Request",
                     "{\"error\":\"kind: probe, scan, auto or listen\"}");
    }

    esp_err_t err = obd_chip_probe_start((obd_probe_kind_t)kind, baud, seconds,
                                         cmd);

    if (err == ESP_ERR_INVALID_STATE)
    {
        return reply(req, "409 Conflict",
                     "{\"error\":\"a wire tool or a firmware update is running\"}");
    }

    if (err == ESP_ERR_INVALID_ARG)
    {
        return reply(req, "400 Bad Request",
                     "{\"error\":\"baud >= 0, seconds 0..60\"}");
    }

    if (err != ESP_OK)
    {
        return reply(req, "500 Internal Server Error",
                     "{\"error\":\"could not start the tool\"}");
    }

    char out[96];

    snprintf(out, sizeof(out), "{\"started\":true,\"kind\":\"%s\"}",
             PROBE_KINDS[kind]);
    return reply(req, "202 Accepted", out);
}

/** GET /api/obd_chip/report: the chip configuration block (Ali, 2026-10-28),
 *  read through the request engine in one transaction, about 1.5 s: 409
 *  when the chip is down or an update runs. The lines keep their breaks
 *  (`\n`) so the page shows them as the chip printed them. */
static esp_err_t obd_chip_report_handler(httpd_req_t *req)
{
    static obd_chip_report_t s_report EXT_RAM_BSS_ATTR;
    esp_err_t err = obd_chip_report_run(&s_report);

    if (err == ESP_ERR_INVALID_STATE)
    {
        return reply(req, "409 Conflict",
                     "{\"error\":\"the chip is not up or an update runs\"}");
    }

    if (err != ESP_OK)
    {
        return reply(req, "500 Internal Server Error",
                     "{\"error\":\"could not read the chip\"}");
    }

    const size_t cap = 6144;
    char *body = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (body == NULL)
    {
        return reply(req, "500 Internal Server Error", "{\"error\":\"no memory\"}");
    }

    size_t n = (size_t)snprintf(body, cap, "{\"items\":[");

    for (size_t i = 0; i < s_report.n && n + 560 < cap; i++)
    {
        const obd_chip_report_item_t *it = &s_report.items[i];

        n += (size_t)snprintf(body + n, cap - n,
                              "%s{\"cmd\":\"%s\",\"what\":\"%s\",\"err\":\"%s\","
                              "\"resp\":\"", (i > 0) ? "," : "", it->cmd,
                              it->what, (it->err == ESP_OK) ? "" : esp_err_to_name(it->err));

        for (const char *p = it->resp; *p != '\0' && n + 8 < cap; p++)
        {
            if (*p == '\n')
            {
                body[n++] = '\\';
                body[n++] = 'n';
            }
            else if (*p == '"' || *p == '\\' || (unsigned char)*p < 32)
            {
                body[n++] = '.';
            }
            else
            {
                body[n++] = *p;
            }
        }

        n += (size_t)snprintf(body + n, cap - n, "\"}");
    }

    n += (size_t)snprintf(body + n, cap - n, "]}");

    httpd_resp_set_type(req, "application/json");
    err = httpd_resp_send(req, body, (ssize_t)n);
    heap_caps_free(body);
    return err;
}

esp_err_t obd_chip_register_http(void)
{
    static const httpd_uri_t URIS[] =
    {
        { .uri = "/api/obd_chip", .method = HTTP_GET,
          .handler = obd_chip_get_handler },
        { .uri = "/api/obd_chip/update", .method = HTTP_POST,
          .handler = obd_chip_update_handler },
        { .uri = "/api/obd_chip/probe", .method = HTTP_GET,
          .handler = obd_chip_probe_get_handler },
        { .uri = "/api/obd_chip/probe", .method = HTTP_POST,
          .handler = obd_chip_probe_post_handler },
        { .uri = "/api/obd_chip/report", .method = HTTP_GET,
          .handler = obd_chip_report_handler },
    };

    esp_err_t err = http_server_manager_register_handlers(
        URIS, sizeof(URIS) / sizeof(URIS[0]));

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "/api/obd_chip routes registered");
    }

    return err;
}
