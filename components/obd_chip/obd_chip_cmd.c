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
 * @file obd_chip_cmd.c
 * @brief The request->response engine. It is an ordinary fan-out subscriber
 *        (broadcast contract): it receives EVERYTHING the chip prints and,
 *        per design, whatever arrives inside its transaction window up to the
 *        '>' prompt IS the response — interleaved unsolicited noise included.
 *        Framing/matching logic is pure (obd_chip_parse.c) and host-tested,
 *        including chunk-boundary splits and noise interleave.
 */
#include "obd_chip.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/task.h"

#include "obd_gate.h"

#include "obd_chip_private.h"

static const char *TAG = "obd_chip"; /* one TAG per component (§10) */

#define CMD_QUEUE_DEPTH 64

/* engine's subscriber queue: PSRAM static (§2) */
static uint8_t s_q_storage[CMD_QUEUE_DEPTH * sizeof(obd_chunk_t)] EXT_RAM_BSS_ATTR;
static StaticQueue_t s_q_buf; /* internal: FreeRTOS object */
static QueueHandle_t s_q;

static obd_resp_acc_t s_acc EXT_RAM_BSS_ATTR; /* one in-flight transaction */

esp_err_t obd_cmd_engine_init(void)
{
    if (s_q == NULL)
    {
        s_q = xQueueCreateStatic(CMD_QUEUE_DEPTH, sizeof(obd_chunk_t),
                                 s_q_storage, &s_q_buf);

        if (s_q == NULL)
        {
            return ESP_ERR_NO_MEM;
        }

        /* NOT subscribed here: the engine subscribes per transaction
           (below). A standing subscription filled the 64-slot queue with
           every app line while no request was in flight — an ELM app
           streaming through a bridge for minutes (autopid yielded) made
           the fan-out drop-count this queue and WARN every 100 chunks
           for nothing (2026-09-08). */
    }

    return ESP_OK;
}

static void cmd_engine_done(void)
{
    obd_chip_unsubscribe(s_q);
    obd_gate_release(obd_chip_gate_owner);
    obd_core_release();
}

esp_err_t obd_chip_request(const char *cmd, char *resp, size_t resp_len,
                           TickType_t timeout)
{
    if (cmd == NULL || resp == NULL || resp_len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_q == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (obd_parse_is_monitor_cmd(cmd))
    {
        /* monitor-class commands never end with a prompt — callers must use
           claim(MONITOR) + send() + subscribe instead (README) */
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* EEPROM guard (obd_chip_guard.h): ATSP/ATM1 go out as their RAM
       twins, ATPP/ATSD/ATCV/STWBR never go out. Runs on a copy of the
       command head (the API takes a const string); the rare command
       longer than the copy is guarded on its head and sent whole. */
    size_t cmd_len = strlen(cmd);
    char guarded[256];
    size_t glen = (cmd_len < sizeof(guarded)) ? cmd_len : sizeof(guarded) - 1;

    memcpy(guarded, cmd, glen);
    guarded[glen] = '\0';

    obd_guard_t gv = obd_chip_guard_cmd(guarded, glen);

    if (gv != OBD_GUARD_PASS)
    {
        obd_core_guard_note(gv, cmd, cmd_len);
    }

    if (gv == OBD_GUARD_BLOCKED)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const char *head = (gv == OBD_GUARD_REWRITTEN) ? guarded : cmd;

    /* async bring-up: never write mid-negotiation — wait within the
       caller's own budget, then time out without touching the wire
       (boot-window callers like the autopid poller just retry) */
    if (!obd_core_bringup_wait(pdTICKS_TO_MS(timeout)))
    {
        return ESP_ERR_TIMEOUT;
    }

    /* COMMAND claim: serializes concurrent requesters; fails fast when a
       MONITOR session holds the chip (policy "manual", task §5) */
    esp_err_t err = obd_core_claim(1 /* CLAIM_COMMAND */, 2000);

    if (err != ESP_OK)
    {
        return err;
    }

    /* obd_gate: this transaction is one bus conversation — serialize
       against the ESP-side ELM engines (released again on every exit
       path below; the fan-out's '>' detection may beat us to it) */
    (void)obd_gate_acquire(obd_chip_gate_owner, OBD_GATE_WAIT_MS);

    /* start clean: drop stale chunks from before our transaction, then
       join the fan-out for exactly this transaction (subscribe BEFORE
       the write so the response's first chunk cannot be missed) */
    obd_chunk_t chunk;

    while (xQueueReceive(s_q, &chunk, 0) == pdTRUE)
    {
    }

    err = obd_chip_subscribe(s_q, "cmd_engine");

    if (err != ESP_OK)
    {
        obd_gate_release(obd_chip_gate_owner);
        obd_core_release();
        return err;
    }

    obd_parse_reset(&s_acc);

    /* send the (guarded) command, appending the CR if the caller omitted it */
    bool needs_cr = (cmd_len == 0 || cmd[cmd_len - 1] != '\r');

    err = obd_uart_write((const uint8_t *)head, glen);

    if (err == ESP_OK && cmd_len > glen)
    {
        err = obd_uart_write((const uint8_t *)cmd + glen, cmd_len - glen);
    }

    if (err == ESP_OK && needs_cr)
    {
        err = obd_uart_write((const uint8_t *)"\r", 1);
    }

    if (err != ESP_OK)
    {
        cmd_engine_done();
        return err;
    }

    /* collect until the prompt or the caller's deadline */
    TickType_t deadline = xTaskGetTickCount() + timeout;

    while (!s_acc.done)
    {
        TickType_t now = xTaskGetTickCount();

        if (now >= deadline)
        {
            cmd_engine_done();
            ESP_LOGD(TAG, "request '%.8s' timed out (%u bytes so far)", cmd,
                     (unsigned)s_acc.len);
            return ESP_ERR_TIMEOUT;
        }

        if (xQueueReceive(s_q, &chunk, deadline - now) == pdTRUE)
        {
            obd_parse_feed(&s_acc, (const char *)chunk.data, chunk.len);
        }
    }

    obd_parse_extract(&s_acc, cmd, resp, resp_len);
    cmd_engine_done();

    if (s_acc.overflow)
    {
        ESP_LOGW(TAG, "response to '%.8s' exceeded %d bytes (truncated)",
                 cmd, OBD_RESP_MAX);
    }

    return ESP_OK;
}

esp_err_t obd_chip_get_version(char *buf, size_t buf_len, TickType_t timeout)
{
    return obd_chip_request("VTVERS", buf, buf_len, timeout);
}
