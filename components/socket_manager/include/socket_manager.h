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
 * @file socket_manager.h
 * @brief WiCAN TCP/UDP server owner (service component).
 *
 * Runs multiple listeners on different ports simultaneously (settings-defined,
 * up to SOCKET_MANAGER_MAX_SERVERS), each robust across client churn, Wi-Fi
 * drops and interface restarts. Every server presents the firmware's standard
 * chunk-stream face, queue-based RX (subscribe) + send() TX, so a server
 * plugs into bridge_manager as an endpoint with three-line glue.
 *
 * Content-agnostic: no protocol logic lives here (that is translator
 * territory, see bridge_manager).
 *
 * Semantics (see README):
 *  - TCP multi-client AGGREGATE: send() fans out to all connected clients;
 *    RX from any client merges into the one subscriber queue (transparent-
 *    bridge behavior; per-client addressing is out of scope v1).
 *  - UDP: RX from anyone on the port; TX to the last peer heard from.
 *  - Reboot-to-apply (standard §4.2): servers are built once at start() from
 *    the boot-applied settings; changing them = persist + reboot.
 *
 * Lifecycle:
 *   socket_manager_init();   // registers settings + log descriptors
 *   settings_manager_start();
 *   socket_manager_start();  // opens the enabled listeners
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

#define SOCKET_MANAGER_MAX_SERVERS 4
#define SOCKET_MANAGER_MAX_CLIENTS 4   /* per TCP server */

/* Chunk layout shared with the firmware-wide endpoint convention
 * (identical to obd_chip's obd_chunk_t: bridges pump these by value). */
#define SOCKET_MANAGER_CHUNK_SIZE 128

typedef struct
{
    uint16_t len;
    uint8_t  data[SOCKET_MANAGER_CHUNK_SIZE];
} socket_chunk_t;

typedef struct
{
    uint16_t clients;      /* currently connected (TCP) / 0|1 peer (UDP) */
    uint32_t bytes_in;
    uint32_t bytes_out;
    uint32_t rx_drops;     /* subscriber queue full: chunk dropped        */
    uint32_t tx_drops;     /* stalled send: chunk dropped, client kept   */
    uint32_t reconnects;   /* listener (re)creations after failure         */
    uint32_t refused;      /* connections refused at max_clients           */
} socket_stats_t;

/** Register settings ("socket_manager") + log descriptors. No sockets yet. */
esp_err_t socket_manager_init(void);

/** Open the enabled listeners per boot-applied settings and start the net
 *  task. ESP_ERR_INVALID_STATE if the settings boot pass left the component
 *  unconfigured (standard §4.3 step 5). */
esp_err_t socket_manager_start(void);

/** Close all sockets and stop the net task. */
esp_err_t socket_manager_stop(void);

/**
 * Attach the ONE subscriber queue of @p server (item type socket_chunk_t).
 * RX from the server's clients starts flowing into it. One subscriber per
 * server (a server is one bridge endpoint): ESP_ERR_INVALID_STATE if taken.
 */
esp_err_t socket_manager_subscribe(const char *server, QueueHandle_t q);
esp_err_t socket_manager_unsubscribe(const char *server, QueueHandle_t q);

/** TX: TCP = fan-out to every connected client; UDP = to the last peer
 *  (ESP_ERR_INVALID_STATE before any datagram arrived). */
esp_err_t socket_manager_send(const char *server, const uint8_t *data,
                              size_t len);

/** Per-server counters snapshot. ESP_ERR_NOT_FOUND for unknown names. */
esp_err_t socket_manager_stats(const char *server, socket_stats_t *out);

/** Configured server name for slot @p idx; NULL past the configured
 *  count. Valid after the settings boot pass (bridge_endpoints registers
 *  a jack per configured server, whatever its name); the pointer stays
 *  valid for the boot (reboot-to-apply config). */
const char *socket_manager_server_name(int idx);

#ifdef __cplusplus
}
#endif
