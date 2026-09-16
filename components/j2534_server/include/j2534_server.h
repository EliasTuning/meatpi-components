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
 * @file j2534_server.h
 * @brief SAE J2534 PassThru device — session + channel server. See
 *        TASK_j2534_server.md. Groundwork phase: the transport-agnostic
 *        frame pump + a TCP listener doing the session/channel HANDSHAKE
 *        (HELLO/OPEN/CONNECT/DISCONNECT/CLOSE). Vehicle I/O (CAN +
 *        ISO15765), filters, periodics, ioctl, and the USB-CDC transport
 *        are later phases; data ops answer ERR_NOT_SUPPORTED for now.
 *
 * Settings ("j2534_server", reboot-to-apply): enabled (default false),
 * port (TCP, default 6809), cli.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define J2534_MAX_CHANNELS 4

esp_err_t j2534_server_init(void);
esp_err_t j2534_server_start(void);
esp_err_t j2534_server_stop(void);

typedef struct {
    bool     enabled;
    uint16_t port;
    bool     listening;
    bool     client_connected;   /* a tester is attached                 */
    bool     device_open;        /* PassThruOpen done                    */
    uint8_t  channel_count;      /* active PassThruConnect channels      */
    uint32_t frames_rx;          /* wire frames received                 */
    uint32_t frames_tx;          /* wire frames sent                     */
    bool     allow_reflash;      /* ECU-flashing gate open (unsafe)      */
    bool     allow_lan;          /* reachable on STA/USB-eth uplinks     */
    bool     exclusive;          /* runtime: autopid off the bus while a  */
                                 /* tester is attached (obd_gate hold)   */
    bool     autopid_paused;     /* the pollers acknowledged the hold    */
} j2534_server_status_t;

esp_err_t j2534_server_status(j2534_server_status_t *out);

/** Runtime "exclusive" (boot default = the `exclusive` setting): while a
 *  tester is attached the background pollers (autopid) stay off the bus.
 *  Applies at once, also to a tester already attached. */
void j2534_server_set_exclusive(bool on);
bool j2534_server_exclusive(void);

esp_err_t j2534_server_register_http(void); /* GET /api/j2534 */

/* ---- serial transport (device_class=cdc) ------------------------------
 * The same framed wire protocol can run over a CDC-ACM serial link instead
 * of TCP. A CDC-device component provides these two blocking byte ops and
 * registers them, then calls j2534_server_serve_serial() from its own task
 * once the host has opened the port. The single-tester guard makes a
 * serial and a TCP session mutually exclusive (second is refused). */
typedef struct {
    /* read up to n bytes, blocking up to timeout_ms; return bytes read
     * (>0), 0 on timeout, <0 on link error (session ends) */
    int (*read)(uint8_t *buf, size_t n, uint32_t timeout_ms);
    /* write n bytes; return n on success, <0 on error */
    int (*write)(const uint8_t *buf, size_t n);
} j2534_serial_transport_t;

void j2534_server_set_serial_transport(const j2534_serial_transport_t *t);

/* Run one serial tester session (blocks until the link drops or the server
 * stops). Refused immediately if a tester is already attached on another
 * transport. Caller loops to accept the next session. */
void j2534_server_serve_serial(void);

#ifdef __cplusplus
}
#endif
