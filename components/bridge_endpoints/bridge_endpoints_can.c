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
 * @file bridge_endpoints_can.c
 * @brief The `can` jack (internal CAN-frame endpoint for the translators).
 *
 * Wraps can_manager: send() decodes a can_frame_wire chunk and injects it
 * on the bus; a pump task subscribes to every RX frame, encodes each into
 * a chunk, and feeds the subscribed bridges' queues.
 *
 * multi_consumer since 2026-07-26 (mqtt_can v2 backlog): up to
 * BEP_CAN_MAX_SUBS bridges may sit on the jack at once (slcan + mqtt
 * simultaneously), every subscriber gets a full copy of each RX chunk,
 * TX from any bridge serializes through can_manager_send. ONE ingress
 * filter for the whole jack (bep_can_set_filter, mqtt_can's setting):
 * with several consumers attached the filter restricts what ALL of them
 * see, per-consumer filters are out of scope (documented in the README).
 * The pump runs only while at least one bridge is subscribed.
 */
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "bridge_manager.h"
#include "can_frame_wire.h"
#include "can_manager.h"
#include "can_core.h"

#include "bridge_endpoints_private.h"

#define BEP_CAN_MAX_SUBS 4

static const char *TAG = "bridge_endpoints";

static QueueHandle_t s_can_rx_q;        /* can_core_frame_t from can_manager */
static QueueHandle_t s_can_qs[BEP_CAN_MAX_SUBS]; /* bridge queues (fan-out)   */
static int s_can_nsubs;
static portMUX_TYPE s_can_lock = portMUX_INITIALIZER_UNLOCKED; /* internal: spinlock */
static int s_can_sub_idx = -1;
static uint32_t s_can_filter;           /* mask==0 -> monitor all (default)    */
static uint32_t s_can_mask;
static bool s_can_filter_ext;
static TaskHandle_t s_can_pump;
static StaticTask_t s_can_pump_tcb;                          /* internal: FreeRTOS */
static StackType_t s_can_pump_stack[3072] EXT_RAM_BSS_ATTR;  /* PSRAM: no flash    */
static volatile bool s_can_pump_run;
static uint32_t s_can_ep_drops;

void bep_can_set_filter(uint32_t filter, uint32_t mask, bool ext)
{
    /* pre-bridge-start config (mqtt_can's 1:1 filter mapping, meatpi
       2026-07-22). mask==0 keeps the monitor-all default. Applies at
       the FIRST subscribe: set before bridge_manager_start(). Jack-wide:
       every consumer sees the same (filtered) stream. */
    s_can_filter = filter;
    s_can_mask = mask;
    s_can_filter_ext = ext;
}

esp_err_t bep_can_send(const uint8_t *d, size_t l)
{
    can_core_frame_t f;
    size_t off = 0;
    size_t n;

    while ((n = can_wire_decode_next(d + off, l - off, &f)) > 0)
    {
        esp_err_t err = can_manager_send(f.id, f.ext, f.rtr, f.data, f.dlc);

        if (err != ESP_OK)
        {
            return err;
        }

        off += n;
    }

    if (l == 0 || off < l)
    {
        s_can_ep_drops++;
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static void can_pump_task(void *arg)
{
    can_core_frame_t f;
    bridge_chunk_t c;

    (void)arg;

    while (s_can_pump_run)
    {
        if (xQueueReceive(s_can_rx_q, &f, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            c.len = (uint16_t)can_wire_encode(&f, c.data);

            /* COALESCE: pack whatever the bus already queued into the
             * same chunk (a chunk is a plain concatenation of wire
             * frames; every consumer loops via can_wire_decode_next).
             * Lifts frame-bound transports (WS was 1 frame = 1 WS
             * frame = ~676 fps) toward bus rate at zero added latency
             * (only frames ALREADY waiting are packed; an idle bus
             * still ships 1-frame chunks immediately). */
            while (c.len + CAN_WIRE_MAX <= BRIDGE_MANAGER_CHUNK_SIZE &&
                   xQueueReceive(s_can_rx_q, &f, 0) == pdTRUE)
            {
                c.len += (uint16_t)can_wire_encode(&f, c.data + c.len);
            }

            /* fan-out: every subscribed bridge gets a full copy.
             * Snapshot under the lock, send OUTSIDE it (queue API is
             * forbidden inside an IDF critical section). */
            QueueHandle_t qs[BEP_CAN_MAX_SUBS];

            portENTER_CRITICAL(&s_can_lock);
            memcpy(qs, (const void *)s_can_qs, sizeof(qs));
            portEXIT_CRITICAL(&s_can_lock);

            for (int i = 0; i < BEP_CAN_MAX_SUBS; i++)
            {
                if (qs[i] != NULL && xQueueSend(qs[i], &c, 0) != pdTRUE)
                {
                    s_can_ep_drops++; /* that bridge's queue is full */
                }
            }
        }
    }

    s_can_pump = NULL;
    vTaskDelete(NULL);
}

static void bep_can_rebind(void)
{
    esp_err_t err;

    if (s_can_nsubs <= 0 || s_can_rx_q == NULL)
    {
        return;
    }

    err = can_manager_subscribe_queue(
        s_can_rx_q, s_can_filter, s_can_mask, s_can_filter_ext,
        s_can_mask == 0, &s_can_sub_idx);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "can bridge resubscribe failed: %s", esp_err_to_name(err));
        return;
    }

    (void)can_manager_subscriber_name(s_can_sub_idx, "bridge");
    ESP_LOGI(TAG, "can bridge resubscribed after the bus came back");
}

void bep_can_bind_restart(void)
{
    can_manager_on_bus_up(bep_can_rebind);
}

esp_err_t bep_can_subscribe(QueueHandle_t q)
{
    int slot = -1;

    portENTER_CRITICAL(&s_can_lock);

    for (int i = 0; i < BEP_CAN_MAX_SUBS; i++)
    {
        if (s_can_qs[i] == NULL && slot < 0)
        {
            slot = i;
        }
    }

    portEXIT_CRITICAL(&s_can_lock);

    if (slot < 0)
    {
        return ESP_ERR_NO_MEM; /* BEP_CAN_MAX_SUBS bridges already on */
    }

    if (s_can_nsubs > 0)
    {
        /* pump + can_manager subscription already live: just join */
        portENTER_CRITICAL(&s_can_lock);
        s_can_qs[slot] = q;
        s_can_nsubs++;
        portEXIT_CRITICAL(&s_can_lock);
        return ESP_OK;
    }

    if (s_can_rx_q == NULL)
    {
        s_can_rx_q = xQueueCreate(32, sizeof(can_core_frame_t));

        if (s_can_rx_q == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = can_manager_subscribe_queue(
        s_can_rx_q, s_can_filter, s_can_mask, s_can_filter_ext,
        s_can_mask == 0, &s_can_sub_idx);
    if (err != ESP_OK)
    {
        return err;
    }

    (void)can_manager_subscriber_name(s_can_sub_idx, "bridge");

    s_can_qs[slot] = q;
    s_can_nsubs = 1;
    s_can_pump_run = true;
    s_can_pump = xTaskCreateStatic(can_pump_task, "can_bridge",
                                   sizeof(s_can_pump_stack) /
                                       sizeof(s_can_pump_stack[0]),
                                   NULL, 9, s_can_pump_stack, &s_can_pump_tcb);
    if (s_can_pump == NULL)
    {
        can_manager_unsubscribe_queue(s_can_sub_idx);
        s_can_sub_idx = -1;
        s_can_qs[slot] = NULL;
        s_can_nsubs = 0;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t bep_can_unsubscribe(QueueHandle_t q)
{
    portENTER_CRITICAL(&s_can_lock);

    for (int i = 0; i < BEP_CAN_MAX_SUBS; i++)
    {
        if (s_can_qs[i] == q)
        {
            s_can_qs[i] = NULL;
            s_can_nsubs--;
        }
    }

    int left = s_can_nsubs;

    portEXIT_CRITICAL(&s_can_lock);

    if (left > 0)
    {
        return ESP_OK; /* other bridges still on the jack */
    }

    s_can_pump_run = false; /* last one out: the pump self-deletes */

    if (s_can_sub_idx >= 0)
    {
        can_manager_unsubscribe_queue(s_can_sub_idx);
        s_can_sub_idx = -1;
    }

    return ESP_OK;
}
