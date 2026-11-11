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

/** Internals shared across the bridge_endpoints .c files. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- usb_obd jack (bridge_endpoints_usb.c): USB port-B UART ---------------- */

esp_err_t bep_usb_send(const uint8_t *d, size_t l);
esp_err_t bep_usb_subscribe(QueueHandle_t q);
esp_err_t bep_usb_unsubscribe(QueueHandle_t q);

/** Install the UART driver + start the RX task (start phase). */
esp_err_t bep_usb_start(void);

/* ---- can jack (bridge_endpoints_can.c): frame <-> chunk pump --------------- */

esp_err_t bep_can_send(const uint8_t *d, size_t l);
esp_err_t bep_can_subscribe(QueueHandle_t q);
esp_err_t bep_can_unsubscribe(QueueHandle_t q);
void bep_can_bind_restart(void);

#ifdef __cplusplus
}
#endif
