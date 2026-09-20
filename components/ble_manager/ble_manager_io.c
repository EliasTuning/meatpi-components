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
 * @file ble_manager_io.c
 * @brief The TX path: a queue-fed task that packs chunks to the negotiated
 *        MTU and notifies FFF1 using the legacy free-packets + congestion
 *        pacing (pure math in ble_manager_pack.c), plus the MTU-chunked CLI
 *        OUT writer.
 */
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "ble_manager.h"
#include "ble_manager_private.h"

static const char *TAG = "ble_manager";

static QueueHandle_t s_tx_q;
static StaticQueue_t s_tx_q_buf; /* internal: FreeRTOS object */
static uint8_t s_tx_store[BLM_TX_QUEUE_DEPTH * sizeof(ble_chunk_t)]
    EXT_RAM_BSS_ATTR;

static TaskHandle_t s_task;
static StaticTask_t s_tcb; /* internal: FreeRTOS object */
static StackType_t s_stack[4096] EXT_RAM_BSS_ATTR;
static volatile bool s_running;
static uint32_t s_tx_drops;

static void tx_task(void *arg)
{
    static uint8_t send_buf[BLM_SEND_BUF_SIZE] EXT_RAM_BSS_ATTR;
    static ble_chunk_t chunk;
    size_t send_len = 0;

    (void)arg;
    ESP_LOGD(TAG, "tx task up");

    while (s_running)
    {
        if (xQueuePeek(s_tx_q, &chunk, pdMS_TO_TICKS(100)) != pdTRUE)
        {
            continue;
        }

        if (!blm_gatt_connected())
        {
            /* connection lost: drain and drop (legacy behavior) */
            xQueueReceive(s_tx_q, &chunk, 0);
            send_len = 0;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int free_packets = blm_gatt_free_packets();

        if (free_packets <= 0 || blm_gatt_congested())
        {
            vTaskDelay(pdMS_TO_TICKS(1)); /* legacy 1 ms pacing */
            continue;
        }

        size_t max_data = blm_gatt_max_data();

        /* pull as many chunks as the free packets allow (legacy loop) */
        while (xQueuePeek(s_tx_q, &chunk, 0) == pdTRUE)
        {
            int needed = blm_pack_packets_needed(send_len, chunk.len,
                                                 max_data);

            if (free_packets < needed)
            {
                break; /* not enough radio credits for this chunk */
            }

            xQueueReceive(s_tx_q, &chunk, 0);

            size_t consumed = 0;

            while (consumed < chunk.len)
            {
                if (blm_pack_fill(send_buf, &send_len, max_data, chunk.data,
                                  chunk.len, &consumed))
                {
                    blm_gatt_notify_data(send_buf, (uint16_t)send_len);
                    send_len = 0;

                    if (--free_packets == 0 && consumed < chunk.len)
                    {
                        ESP_LOGE(TAG, "ran out of free packets too soon");
                        break;
                    }
                }
            }
        }

        /* flush the partial packet (legacy tail flush) */
        if (free_packets != 0 && send_len != 0)
        {
            blm_gatt_notify_data(send_buf, (uint16_t)send_len);
            send_len = 0;
        }
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t blm_io_start(void)
{
    if (s_tx_q == NULL)
    {
        s_tx_q = xQueueCreateStatic(BLM_TX_QUEUE_DEPTH, sizeof(ble_chunk_t),
                                    s_tx_store, &s_tx_q_buf);
    }

    xQueueReset(s_tx_q);
    s_running = true;
    s_task = xTaskCreateStatic(tx_task, "ble_tx",
                               sizeof(s_stack) / sizeof(s_stack[0]), NULL, 5,
                               s_stack, &s_tcb);
    return (s_task != NULL) ? ESP_OK : ESP_FAIL;
}

void blm_io_stop(void)
{
    s_running = false;

    for (int i = 0; i < 25 && s_task != NULL; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t blm_io_queue_tx(const uint8_t *data, size_t len)
{
    ble_chunk_t chunk;

    while (len > 0)
    {
        size_t n = (len > BLE_MANAGER_CHUNK_SIZE) ? BLE_MANAGER_CHUNK_SIZE
                                                  : len;

        chunk.len = (uint16_t)n;
        memcpy(chunk.data, data, n);

        /* Brief wait instead of a 0-tick drop: a 22-byte SLCAN line that
           arrives while FFF1 is congested is the first TP20 data PDU, and
           skipping it breaks the session. 50 ms matches the host-TX wait
           used on the legacy SLCAN path. */
        if (xQueueSend(s_tx_q, &chunk, pdMS_TO_TICKS(50)) != pdTRUE)
        {
            s_tx_drops++;

            if ((s_tx_drops % 100) == 1)
            {
                ESP_LOGW(TAG, "tx queue full (%lu drops)",
                         (unsigned long)s_tx_drops);
            }

            return ESP_FAIL;
        }

        data += n;
        len -= n;
    }

    return ESP_OK;
}

esp_err_t blm_io_cli_write(const char *data, size_t len)
{
    if (!blm_gatt_connected())
    {
        return ESP_ERR_INVALID_STATE;
    }

    /* legacy ble_cmdline_output: MTU-chunked, bounded wait for credits */
    size_t offset = 0;

    while (offset < len)
    {
        int tries = 0;

        while (blm_gatt_free_packets() <= 0 && tries++ < 100)
        {
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        size_t chunk = len - offset;
        size_t max_data = blm_gatt_max_data();

        if (chunk > max_data)
        {
            chunk = max_data;
        }

        blm_gatt_notify_cli((const uint8_t *)(data + offset),
                            (uint16_t)chunk);
        offset += chunk;
    }

    return ESP_OK;
}
