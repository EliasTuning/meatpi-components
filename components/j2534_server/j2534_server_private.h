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

/** Internals shared across the j2534_server .c files. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "j2534_proto.h"
#include "j2534_server.h"

esp_err_t j2534_server_register_cli(void);

/* ---- core (j2534_server.c) <-> transport layer (j2534_server_transport.c) --- */

/* core -> transport */
esp_err_t j2534_srv_transport_init(void);           /* mutexes            */
esp_err_t j2534_srv_listener_start(uint16_t port);  /* the TCP task       */
bool      j2534_srv_listening(void);
const j2534_transport_t *j2534_srv_active(void);    /* NULL = idle        */
bool      j2534_srv_send_frame(const j2534_transport_t *t, uint8_t type,
                               uint16_t seq, uint16_t channel,
                               const uint8_t *payload, uint32_t len);
bool      j2534_srv_send_ack(const j2534_transport_t *t, uint16_t seq,
                             uint16_t channel, uint32_t status,
                             const uint32_t *result);
/** A run of ready-made frames (header + payload each) in ONE transport
 *  write, under the send lock; counts every frame as sent. The RX pump's
 *  batch (2026-11-06). */
bool      j2534_srv_send_raw(const j2534_transport_t *t, const uint8_t *buf,
                             size_t len);

/* transport -> core */
bool      j2534_srv_running(void);
volatile const bool *j2534_srv_run_flag(void);
uint8_t  *j2534_srv_req_payload(size_t *cap);       /* PSRAM, one session */
void      j2534_srv_count_rx(void);
void      j2534_srv_count_tx(void);
void      j2534_srv_count_tx_refused(void); /* a write the CAN node refused */
void      j2534_srv_session_begin(const char *transport_name);
void      j2534_srv_session_end(void);
void      j2534_srv_handle_frame(const j2534_transport_t *t,
                                 const j2534_hdr_t *h,
                                 const uint8_t *payload);

/** A UDS memory-transfer service (0x34..0x37) in a tester's request:
 *  counted on the status, never gated (2026-10-27). */
void j2534_srv_count_reflash_seen(void);

/* ---- settings (j2534_server_settings.c) -------------------------------- */

/** Boot-applied config (filled by on_apply). */
typedef struct
{
    bool     enabled;
    bool     allow_lan;        /* accept on STA / USB-eth uplinks too */
    bool     exclusive;        /* boot default: pause autopid while attached */
    uint16_t port;
} j2534_config_t;

/** Register the "j2534_server" descriptor with settings_manager. */
esp_err_t j2534_settings_register(void);

const j2534_config_t *j2534_settings_config(void);

bool j2534_settings_is_configured(void); /* boot apply ran (standard §4.3) */
