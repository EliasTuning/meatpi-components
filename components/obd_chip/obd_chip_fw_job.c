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
 * @file obd_chip_fw_job.c
 * @brief The entry points the console and the HTTP route use around the
 *        update engine (obd_chip_fw.c), split out 2026-10-28 (700-line
 *        rule): the update as a one-shot job task, and the bare-wire probe
 *        (issue #1023: a chip that will not come up could neither be
 *        diagnosed nor reflashed from the device).
 */
#include "obd_chip.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "obd_chip_private.h"

static const char *TAG = "obd_chip"; /* one TAG per component (§10) */

/* ---- the job (console / HTTP) ------------------------------------------------- */

typedef struct
{
    char path[72]; /* "" = the packaged image */
} fw_job_t;

static fw_job_t      s_job;
static volatile bool s_job_running;

static void fw_job_task(void *arg)
{
    (void)arg;

    /* the user's rule: the chip is written only when the image is newer
       than what it runs (2026-11-01, Ali: no forced rewrite) */
    esp_err_t err = (s_job.path[0] != '\0')
                        ? obd_chip_firmware_update(s_job.path)
                        : obd_chip_firmware_update_builtin(OBD_FW_RULE_NEWER);

    if (err == ESP_OK && obd_fw_last_flashed())
    {
        /* reflashed and hardware-reset: a fresh bring-up restores the
           echo-off state, the provisioning and the RX task (a chip that
           was down comes back the same way, issue #1023) */
        ESP_LOGI(TAG, "update job done: bringing the chip up again");
        (void)obd_chip_stop();
        (void)obd_chip_start();
    }
    else
    {
        ESP_LOGI(TAG, "update job ended: %s", esp_err_to_name(err));
    }

    /* ephemeral task escapes System Monitor: the stack-audit convention */
    ESP_LOGI(TAG, "update job stack_hw=%u B",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    s_job_running = false;
    vTaskDelete(NULL);
}

esp_err_t obd_chip_firmware_update_start(const char *fs_path)
{
    if (s_job_running || obd_chip_update_in_progress())
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (fs_path != NULL)
    {
        unsigned v[3];

        /* the file's name carries the image's version: refused here, in
           the caller's context, rather than by the job after a 202 */
        if (strlen(fs_path) >= sizeof(s_job.path) ||
            !obd_fw_version_parse(fs_path, v))
        {
            return ESP_ERR_INVALID_ARG;
        }
    }

    s_job.path[0] = '\0';

    if (fs_path != NULL)
    {
        strlcpy(s_job.path, fs_path, sizeof(s_job.path));
    }

    s_job_running = true;

    /* internal stack: UART driver path (the bring-up task's rule) */
    if (xTaskCreate(fw_job_task, "obd_fwjob", 4096, NULL, 5, NULL) != pdPASS)
    {
        s_job_running = false;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/* ---- the bare-wire probe (obd_chip probe / listen) ----------------------------- */

esp_err_t obd_chip_probe(const char *cmd, int baud, uint32_t timeout_ms,
                         uint8_t *rx, size_t rx_len, size_t *rx_got,
                         bool *prompt)
{
    if (cmd == NULL || rx == NULL || rx_got == NULL || prompt == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!obd_core_bringup_wait(OBD_BRINGUP_WAIT_MS) ||
        obd_chip_update_in_progress())
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = obd_core_claim(3 /* CLAIM_EXCLUSIVE */, 5000);

    if (err != ESP_OK)
    {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    const int found = obd_uart_get_baud();

    if (baud > 0 && baud != found)
    {
        obd_uart_set_baud(baud);
    }

    obd_uart_flush_input();
    *rx_got = 0;
    *prompt = false;

    /* an empty command = a passive listen (the obd_chip listen command) */
    if (cmd[0] != '\0')
    {
        err = obd_uart_write((const uint8_t *)cmd, strlen(cmd));
    }

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (err == ESP_OK && xTaskGetTickCount() < deadline)
    {
        uint8_t byte;

        if (obd_uart_read(&byte, 1, 50) != 1)
        {
            continue;
        }

        if (*rx_got < rx_len)
        {
            rx[*rx_got] = byte;
        }

        (*rx_got)++;

        if (byte == '>')
        {
            *prompt = true;
            break;
        }
    }

    obd_uart_flush_input();

    if (baud > 0 && baud != found)
    {
        obd_uart_set_baud(found);
    }

    obd_core_release();
    return err;
}
