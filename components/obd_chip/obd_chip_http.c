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
 *        cJSON dependency needed here.
 */
#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "http_server_manager.h"

#include "obd_chip.h"

static const char *TAG = "obd_chip";

#define OC_HTTP_SUBS 8

static esp_err_t obd_chip_get_handler(httpd_req_t *req)
{
    obd_chip_stats_t st;
    obd_chip_sub_stats_t subs[OC_HTTP_SUBS];
    char body[768];
    size_t n = 0;

    obd_chip_get_stats(&st);

    size_t n_subs = obd_chip_get_subscribers(subs, OC_HTTP_SUBS);

    n += (size_t)snprintf(body + n, sizeof(body) - n,
                          "{\"ready\":%s,\"claim\":\"%s\","
                          "\"client_idle_ms\":%lu,"
                          "\"eeprom_guard\":{\"rewrites\":%lu,\"blocked\":%lu},"
                          "\"uart\":{\"rx_bytes\":%lu,\"rx_chunks\":%lu,"
                          "\"rx_max_chunk\":%u,\"rx_overflows\":%lu,"
                          "\"rx_buffered\":%lu,\"tx_bytes\":%lu},"
                          "\"subscribers\":[",
                          st.ready ? "true" : "false", st.claim,
                          (unsigned long)st.client_idle_ms,
                          (unsigned long)st.guard_rewrites,
                          (unsigned long)st.guard_blocked,
                          (unsigned long)st.rx_bytes,
                          (unsigned long)st.rx_chunks,
                          (unsigned)st.rx_max_chunk,
                          (unsigned long)st.rx_overflows,
                          (unsigned long)st.rx_buffered,
                          (unsigned long)st.tx_bytes);

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

esp_err_t obd_chip_register_http(void)
{
    static const httpd_uri_t URIS[] =
    {
        { .uri = "/api/obd_chip", .method = HTTP_GET,
          .handler = obd_chip_get_handler },
    };

    esp_err_t err = http_server_manager_register_handlers(
        URIS, sizeof(URIS) / sizeof(URIS[0]));

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "/api/obd_chip route registered");
    }

    return err;
}
