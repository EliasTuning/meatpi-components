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
 * @file data_destinations.h
 * @brief Data destinations — cyclic delivery of the live AutoPID
 *        parameter set to MQTT topics, HTTP/HTTPS endpoints and the ABRP
 *        (Iternio) telemetry API (feature component).
 *
 * The v6 home of the legacy autopid "destinations" (v4.51p
 * `group_destination_t`): up to DATA_DESTINATIONS_MAX entries, each with
 * its own type, cycle, auth (bearer / API key header or query / basic),
 * extra query parameters, certificate set (cert_manager) and enable
 * switch. One poster task (PSRAM stack) walks the table every second,
 * publishes what is due, tracks per-destination success/failure counters
 * and applies exponential backoff to a failing endpoint so a dead server
 * or a lost link never turns into a request storm.
 *
 * Payloads (legacy on-wire shapes, so existing receivers keep working):
 *  - MQTT: the flat `{"Name": value, ..., "timestamp": epoch}` snapshot
 *    (retained by default), `~/topic` expands to `<mqtt prefix>/topic`;
 *  - HTTP/HTTPS: the FIRST successful push carries
 *    `{"config": <PID tables>, "status": {...}, "autopid_data": {...}}`
 *    (a receiver learns what the device polls), every later push
 *    `{"autopid_data": {...}}`; `full_first=false` sends the short form
 *    always;
 *  - ABRP: `token=<user token>&tlm=<url-encoded JSON>` form POST with
 *    the API key as a query parameter or `Authorization: APIKEY` header,
 *    the snapshot mapped to ABRP's telemetry keys (SOC->soc, HV_W->power,
 *    the dongle GPS fix -> lat/lon/elevation, ...).
 *
 * Settings: `data_destinations` (reboot-to-apply). Status + a one-shot
 * test: `GET /api/destinations`, `POST /api/destinations/test`.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DATA_DESTINATIONS_MAX 8

/** Result of a one-shot delivery (the Test button / CLI -t). */
typedef struct
{
    bool     ok;
    int      status;          /* HTTP status (0 for MQTT / transport)   */
    uint32_t elapsed_ms;
    char     error[96];       /* "" when ok                              */
} data_destinations_result_t;

/** Register settings ("data_destinations") + log descriptors. No I/O. */
esp_err_t data_destinations_init(void);

/** Start the poster task (idle while disabled / no destinations).
 *  ESP_ERR_INVALID_STATE when unconfigured (Standard §4.3 step 5). */
esp_err_t data_destinations_start(void);
esp_err_t data_destinations_stop(void);

/**
 * Status document (the `GET /api/destinations` body): master enable,
 * link state and one entry per configured destination with its counters
 * (`success`, `fail`, `skipped_offline`, `consecutive_failures`,
 * `backoff_s`, `next_in_s`, `last_status`, `last_error[_time]`,
 * `last_ok_time`) — secrets are never included. Caller frees.
 */
esp_err_t data_destinations_status(cJSON **out);

/**
 * Deliver destination @p name ONCE, now, regardless of its cycle or
 * backoff (SYNCHRONOUS: seconds under a bad network — never call from
 * the event dispatcher). The delivery runs on the poster task's stack
 * (TLS needs it); the caller blocks up to ~20 s. ESP_ERR_NOT_FOUND for
 * an unknown name, ESP_ERR_INVALID_STATE while another test is in
 * flight or the poster is not running, ESP_ERR_TIMEOUT when the poster
 * did not pick the job up.
 */
esp_err_t data_destinations_test(const char *name,
                                 data_destinations_result_t *out);

/** Register the /api/destinations routes (main wires it in HTTP builds). */
esp_err_t data_destinations_register_http(void);

/** Register the `destinations` console command (called internally on
 *  the settings boot apply when the `cli` setting is true — Standard §6b). */
esp_err_t data_destinations_register_cli(void);

#ifdef __cplusplus
}
#endif
