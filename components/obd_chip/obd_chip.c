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
 * @file obd_chip.c
 * @brief Lifecycle, the bring-up task and its gate, the client clock and
 *        the stats. The subscriber registry, the RX fan-out and the raw TX
 *        path live in obd_chip_fanout.c (2026-11-01, 700-line rule), the
 *        claim model in obd_chip_claim.c, the boot sequence in
 *        obd_chip_bringup.c.
 */
#include "obd_chip.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "log_manager.h"

#include "obd_chip_private.h"

static const char *TAG = "obd_chip";

/* obd_gate owner identity for the MIC chip (address only; shared with
 * obd_chip_cmd.c via obd_chip_private.h) */
const char obd_chip_gate_owner[1];

/* the claim model (COMMAND / MONITOR / EXCLUSIVE, the transaction hold)
 * lives in obd_chip_claim.c since 2026-10-28 (700-line rule) */

/* external-client activity clock (obd_chip_client_touch): the last time a
 * bridge (TCP/BLE/USB/WS app) wrote to the chip; 0 = never. Read by
 * autopid to yield the chip while an app drives it (legacy
 * DEV_AUTOPID_ELM327_APP_BIT parity, bench 2026-09-08). */
static volatile int64_t s_client_touch_us;

static bool s_inited;
static bool s_started;
static volatile uint32_t s_settings_gen; /* obd_chip_settings_generation() */
static int  s_active_baud;

/* async bring-up (2026-07-26, meatpi: get the ~2 s ATZ/baud negotiation
 * off the boot path): start() launches a one-shot task; the DONE bit
 * flips when the sequence finishes (chip up OR given up). Wire-touching
 * APIs gate on DONE so nothing interleaves with the bare-UART probes. */
#define BRINGUP_DONE_BIT BIT0

static EventGroupHandle_t s_bringup_evt;
static StaticEventGroup_t s_bringup_evt_buf; /* internal: FreeRTOS object */
static volatile bool s_bringup_active;

/* ---- bring-up gate -------------------------------------------------------------- */

bool obd_core_bringup_wait(uint32_t timeout_ms)
{
    if (!s_bringup_active || s_bringup_evt == NULL)
    {
        return true; /* finished, or start() never launched (degraded) */
    }

    EventBits_t bits = xEventGroupWaitBits(s_bringup_evt, BRINGUP_DONE_BIT,
                                           pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));

    return (bits & BRINGUP_DONE_BIT) != 0;
}

/* ---- claims: obd_chip_claim.c --------------------------------------------- */

bool obd_chip_is_monitor_cmd(const char *cmd)
{
    return obd_parse_is_monitor_cmd(cmd);
}

esp_err_t obd_chip_monitor_stop(void)
{
    /* SPACE, never CR: CR repeats the last command and can re-enter ATMA */
    static const uint8_t stop = ' ';

    if (!obd_core_bringup_wait(OBD_BRINGUP_WAIT_MS))
    {
        return ESP_ERR_INVALID_STATE;
    }

    return obd_uart_write(&stop, 1);
}

/* ---- observability (GET /api/obd_chip) ----------------------------------------- */

esp_err_t obd_chip_get_stats(obd_chip_stats_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    static const char *CLAIMS[] = { "none", "command", "monitor",
                                    "exclusive" };

    memset(out, 0, sizeof(*out));
    out->ready = obd_chip_ready();
    int claim = obd_core_claim_state();

    out->claim = CLAIMS[(claim >= 0 && claim <= 3) ? claim : 0];
    obd_fanout_stats_t fs;

    obd_core_fanout_stats(&fs);
    out->rx_bytes = fs.rx_bytes;
    out->rx_chunks = fs.rx_chunks;
    out->rx_max_chunk = fs.rx_max_chunk;
    out->client_idle_ms = obd_chip_client_idle_ms();
    out->guard_rewrites = fs.guard_rewrites;
    out->guard_blocked = fs.guard_blocked;
    out->protocol_saves = obd_cmd_protocol_saves();
    out->memory_off = fs.memory_off;
    out->app_resets = fs.app_resets;
    out->hidden_bytes = fs.hidden_bytes;
    out->pp_restores = fs.pp_restores;
    obd_uart_get_stats(&out->tx_bytes, &out->rx_overflows,
                       &out->rx_buffered);
    return ESP_OK;
}

/* ---- external-client activity clock ------------------------------------------- */

void obd_chip_client_touch(void)
{
    s_client_touch_us = esp_timer_get_time();
}

uint32_t obd_chip_client_idle_ms(void)
{
    int64_t t = s_client_touch_us;

    if (t == 0)
    {
        return UINT32_MAX; /* never */
    }

    int64_t idle = (esp_timer_get_time() - t) / 1000;

    return (idle < 0) ? 0 : (idle > UINT32_MAX) ? UINT32_MAX
                                                : (uint32_t)idle;
}

/* ---- chip management ------------------------------------------------------------- */

esp_err_t obd_chip_sleep(bool sleep)
{
    if (sleep)
    {
        /* don't yank the chip asleep mid-bring-up (sleep entry already
           takes seconds; after DONE this is a flag test). Proceed on a
           wedged bring-up: sleeping is the stronger intent. */
        (void)obd_core_bringup_wait(OBD_BRINGUP_WAIT_MS);
        obd_pin_sleep();
    }
    else
    {
        obd_pin_wake();
    }

    return ESP_OK;
}

esp_err_t obd_chip_hard_reset(void)
{
    (void)obd_core_bringup_wait(OBD_BRINGUP_WAIT_MS);
    obd_pin_reset_pulse();
    return ESP_OK;
}

bool obd_chip_status_ok(void)
{
    return obd_pin_ready();
}

/* ---- lifecycle -------------------------------------------------------------------- */

esp_err_t obd_chip_init(void)
{
    if (s_inited)
    {
        return ESP_OK;
    }

    static const log_descriptor_t LOG_DESC = { "obd_chip", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC); /* per-TAG level control (§9.2) */

    /* first: the bring-up gate must exist even after a partial init
       (start() can still be reached, §4.3 degrade paths) */
    s_bringup_evt = xEventGroupCreateStatic(&s_bringup_evt_buf);

    esp_err_t err = obd_settings_register();

    if (err != ESP_OK)
    {
        return err;
    }

    obd_pins_init();
    err = obd_uart_init(115200); /* real baud negotiated in start() */

    if (err != ESP_OK)
    {
        return err;
    }

    err = obd_cmd_engine_init(); /* the engine is just another subscriber */

    if (err != ESP_OK)
    {
        return err;
    }

    s_inited = true;
    oc_events_register();
    return ESP_OK;
}

uint32_t obd_chip_settings_generation(void)
{
    return s_settings_gen;
}

void obd_chip_settings_changed(void)
{
    s_settings_gen++;
}

/* The sequence itself (wake, reset, negotiate, provision) lives in
 * obd_chip_bringup.c. */
static esp_err_t bringup_run(void)
{
    esp_err_t err = obd_bringup_run(&s_active_baud);

    if (err == ESP_OK)
    {
        s_started = true;
        s_settings_gen++; /* a fresh chip: every holder's prelude is stale */
    }

    return err;
}

/** Mark bring-up finished (success OR give-up) and release the gate.
 *  Bit before flag: a gate that read the flag as active must find the
 *  bit set when it reaches the wait. */
static void bringup_finish(void)
{
    xEventGroupSetBits(s_bringup_evt, BRINGUP_DONE_BIT);
    s_bringup_active = false;
}

static void bringup_task(void *arg)
{
    (void)arg;

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = bringup_run();

    bringup_finish();

    if (err != ESP_OK)
    {
        /* same degrade-never-halt outcome the boot line used to report:
           the chip stays down, every consumer sees timeouts/errors */
        ESP_LOGE(TAG, "bring-up failed: %s (device degraded)",
                 esp_err_to_name(err));
    }

    /* legacy-parity auto-update (Ali 2026-07-26): a chip below the
       accepted versions (V2.3.22, V2.3.24: the v4.52p list) is flashed
       to the packaged image, never the same version or a newer chip.
       Runs AFTER the DONE bit (the update path waits on it, ordering
       matters) and ALSO after a FAILED bring-up: a chip stuck in download
       mode fails bring-up and completing the download is its recovery
       path. A dead wire aborts on the VTVERS timeout inside. At an
       accepted version this costs one VTVERS round-trip. */
    if (obd_settings_config()->auto_update)
    {
        esp_err_t up = obd_chip_firmware_update_builtin(OBD_FW_RULE_STARTUP);

        if (up != ESP_OK && up != ESP_ERR_TIMEOUT)
        {
            ESP_LOGW(TAG, "auto fw update did not complete (%s): will "
                     "re-check next boot", esp_err_to_name(up));
        }
        else if (up == ESP_OK && obd_fw_last_flashed())
        {
            /* the chip was reflashed and hardware-reset: echo back on,
               provisioning gone, and after a recovery (issue #1023) no
               RX task at all. A fresh bring-up on a new task restores
               all of it; this task is done. */
            ESP_LOGI(TAG, "chip reflashed: bringing it up again");
            (void)obd_chip_stop();
            (void)obd_chip_start();
        }
    }

    /* ephemeral task escapes System Monitor: surface the watermark for
       the stack-audit bench (the 2026-07-22 convention) */
    ESP_LOGI(TAG, "bring-up done in %lu ms, stack_hw=%u B",
             (unsigned long)((esp_timer_get_time() - t0) / 1000),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    vTaskDelete(NULL);
}

esp_err_t obd_chip_start(void)
{
    if (!obd_settings_is_configured())
    {
        ESP_LOGE(TAG, "unconfigured (settings boot pass failed); not starting");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_started || s_bringup_active)
    {
        return ESP_OK;
    }

    /* async (2026-07-26): the ~2 s ATZ + baud negotiation was half the
       boot, launch it and let boot continue; wire-touching APIs gate
       on the DONE bit. Internal stack: same rule as the RX task (UART
       driver path). Clear-before-arm: a retry after a failed round must
       not leave round 1's DONE bit satisfying round 2's gate. */
    if (s_bringup_evt != NULL)
    {
        xEventGroupClearBits(s_bringup_evt, BRINGUP_DONE_BIT);
    }

    s_bringup_active = true;

    if (xTaskCreate(bringup_task, "obd_bringup", 4096, NULL, 5, NULL)
        != pdPASS)
    {
        /* no task: fall back to the legacy synchronous path */
        esp_err_t err = bringup_run();

        bringup_finish();
        return err;
    }

    return ESP_OK;
}

bool obd_chip_ready(void)
{
    return s_started;
}

int obd_chip_active_baud(void)
{
    return s_active_baud;
}

esp_err_t obd_chip_stop(void)
{
    /* a stop that races the bring-up task waits it out first (the RX
       task only exists once bring-up finishes) */
    (void)obd_core_bringup_wait(OBD_BRINGUP_WAIT_MS);

    if (!s_started)
    {
        return ESP_OK;
    }

    obd_uart_rx_task_stop();
    s_started = false;
    ESP_LOGI(TAG, "stopped");
    return ESP_OK;
}
