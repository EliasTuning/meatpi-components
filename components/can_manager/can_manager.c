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
 * @file can_manager.c
 * @brief Lifecycle + the shared can_core bus handle. Settings live in
 *        can_manager_settings.c (standard §4.1); what is on the bus (the
 *        probe, the watch, the id sample) in can_manager_probe.c.
 */
#include "can_manager.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "log_manager.h"

#include "can_manager_private.h"

static const char *TAG = "can_manager";

/* ---- state ---------------------------------------------------------------- */

static bool s_started;
static bool s_running;

/* start / stop / probe never overlap (the probe borrows the one handle) */
static SemaphoreHandle_t s_life;
static StaticSemaphore_t s_life_buf;    /* internal: FreeRTOS object        */

/* ONE handle per physical CAN peripheral (can_core contract) */
EXT_RAM_BSS_ATTR static can_core_handle_t s_bus;

/* baud = "auto": the bitrates the node tries, most likely first (the two
 * ISO 15765-4 / J1939 rates, then the body and legacy ones) */
static const uint16_t AUTO_BAUDS[] = { 500, 250, 125, 1000, 100, 83, 95, 33 };

/* ---- transceiver standby -------------------------------------------------- */

void cm_standby(bool standby)
{
    gpio_set_direction(CONFIG_WICAN_CAN_STDBY_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(CONFIG_WICAN_CAN_STDBY_GPIO, standby ? 1 : 0);
}

/* ---- restart ---------------------------------------------------------------- */

/* esp_restart() resets the CPUs, not the peripherals, and drops the clock to
 * the crystal first: a controller left running sits on the bus at HALF its
 * bitrate, TX pin attached, until the next boot parks the transceiver. Bench
 * 2026-10-02: 25 failed transmissions in a row for the sending node at every
 * restart (32 in a row is its bus-off). So before a restart: transceiver to
 * standby (the bus is released at once), then the controller off the bus.
 * A panic resets without shutdown handlers: nothing can be done there. */
static void cm_on_shutdown(void)
{
    cm_standby(true);

    if (s_running || cm_probe_node_up())
    {
        can_core_quiesce(&s_bus);
    }
}

/* ---- public API ------------------------------------------------------------ */

static esp_err_t start_locked(void);

esp_err_t can_manager_init(void)
{
    static const log_descriptor_t LOG_DESC = { "can_manager", ESP_LOG_INFO };
    static bool s_hooked;

    log_manager_register(&LOG_DESC);

    /* Transceiver to standby before anything else. A software reset does
       not reset the CAN controller: after a panic or a watchdog reset (no
       shutdown handler runs there) it is still on the bus, acknowledging
       and raising error flags with nobody behind it, until a new node
       resets it. Bench 2026-10-03: the ECU simulator, same board, kept
       acknowledging at 250 kbit/s for as long as it stayed "disabled"
       after a restart. In standby the transceiver cannot drive the bus;
       can_manager_start() wakes it when the settings want the bus. */
    cm_standby(true);

    if (s_life == NULL)
    {
        s_life = xSemaphoreCreateMutexStatic(&s_life_buf);
    }

    if (!s_hooked)
    {
        esp_err_t err = esp_register_shutdown_handler(cm_on_shutdown);

        if (err == ESP_OK || err == ESP_ERR_INVALID_STATE)
        {
            s_hooked = true; /* INVALID_STATE = already registered */
        }
        else
        {
            ESP_LOGW(TAG, "no shutdown hook (%s): a restart leaves the "
                          "controller on the bus until the next boot",
                     esp_err_to_name(err));
        }
    }

    return canm_settings_register();
}

esp_err_t can_manager_start(void)
{
    if (!canm_settings_is_configured())
    {
        return ESP_ERR_INVALID_STATE; /* §4.3 step 5 */
    }

    if (s_started)
    {
        return ESP_OK;
    }

    xSemaphoreTake(s_life, portMAX_DELAY);

    esp_err_t err = start_locked();

    xSemaphoreGive(s_life);
    return err;
}

static esp_err_t start_locked(void)
{
    s_started = true;

    if (!canm_settings_enabled())
    {
        cm_standby(true); /* park the transceiver while unused */
        ESP_LOGI(TAG, "disabled by settings");
        return ESP_OK;
    }

    cm_standby(false);

    uint32_t baud_kbps = canm_settings_baud_kbps(); /* 0 = auto */
    bool silent = canm_settings_silent();

    can_core_config_t cfg =
    {
        .tx_gpio        = CONFIG_WICAN_CAN_TX_GPIO,
        .rx_gpio        = CONFIG_WICAN_CAN_RX_GPIO,
        .baud_kbps      = baud_kbps ? baud_kbps : AUTO_BAUDS[0],
        .silent_mode    = silent,
        .rx_queue_depth = 32,
        .tx_queue_depth = 64,
    };

    if (baud_kbps == 0)
    {
        memcpy(cfg.baud_candidates, AUTO_BAUDS, sizeof(AUTO_BAUDS));
        cfg.n_baud_candidates = sizeof(AUTO_BAUDS) / sizeof(AUTO_BAUDS[0]);
    }

    if (can_core_init(&s_bus, &cfg) != ELM327_OK)
    {
        ESP_LOGE(TAG, "bus init failed (baud %lu)",
                 (unsigned long)baud_kbps);
        cm_standby(true);
        return ESP_FAIL; /* main logs + degrades (§3 no panics) */
    }

    s_running = true;
    cm_node_came_up();

    /* listening, whatever the settings ask for: the link policy
       (can_core_link.c) promotes the node once it may talk */
    if (baud_kbps == 0)
    {
        ESP_LOGI(TAG, "up: listening for the bitrate (auto) tx=%d rx=%d "
                      "stdby=%d%s",
                 CONFIG_WICAN_CAN_TX_GPIO, CONFIG_WICAN_CAN_RX_GPIO,
                 CONFIG_WICAN_CAN_STDBY_GPIO, silent ? " SILENT" : "");
    }
    else
    {
        ESP_LOGI(TAG, "up: listening at %lu kbit/s tx=%d rx=%d stdby=%d%s",
                 (unsigned long)baud_kbps, CONFIG_WICAN_CAN_TX_GPIO,
                 CONFIG_WICAN_CAN_RX_GPIO, CONFIG_WICAN_CAN_STDBY_GPIO,
                 silent ? " SILENT" : "");
    }

    return ESP_OK;
}

void can_manager_transceiver(bool active)
{
    cm_standby(!active);
}

esp_err_t can_manager_stop(void)
{
    if (s_life != NULL)
    {
        xSemaphoreTake(s_life, portMAX_DELAY); /* a probe finishes first */
    }

    if (s_running || cm_probe_node_up())
    {
        can_core_deinit(&s_bus);
        s_running = false;
        cm_probe_node_dropped();
    }

    cm_standby(true);
    s_started = false;

    if (s_life != NULL)
    {
        xSemaphoreGive(s_life);
    }

    return ESP_OK;
}

/* ---- what the other files of the component need (can_manager_private.h) ------- */

struct can_core_handle_s *cm_bus(void)
{
    return &s_bus;
}

bool cm_started(void)
{
    return s_started;
}

bool cm_running(void)
{
    return s_running;
}

void cm_life_take(void)
{
    xSemaphoreTake(s_life, portMAX_DELAY);
}

void cm_life_give(void)
{
    xSemaphoreGive(s_life);
}

struct can_core_handle_s *can_manager_core_handle(void)
{
    return s_running ? &s_bus : NULL;
}

esp_err_t can_manager_send(uint32_t id, bool ext, bool rtr,
                           const uint8_t *data, uint8_t dlc)
{
    if (!s_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (dlc > 8 || (dlc > 0 && data == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    can_core_frame_t f =
    {
        .id  = id,
        .ext = ext,
        .rtr = rtr,
        .dlc = dlc,
    };

    if (dlc > 0)
    {
        memcpy(f.data, data, dlc);
    }

    if (can_core_transmit(&s_bus, &f, 100) == ELM327_OK)
    {
        return ESP_OK;
    }

    /* refused (the node may not talk) is a state, a full TX queue or a
       driver error is a failure */
    return s_bus.tx_open ? ESP_FAIL : ESP_ERR_INVALID_STATE;
}

bool can_manager_tx_ready(void)
{
    return s_running && s_bus.tx_open;
}

esp_err_t can_manager_adopt_baud(uint32_t baud_kbps)
{
    if (!s_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    bool adopted = false;

    if (s_life != NULL)
    {
        xSemaphoreTake(s_life, portMAX_DELAY); /* not under a probe */
    }

    elm327_err_t rc = can_core_adopt_baud(&s_bus, baud_kbps, &adopted);

    if (s_life != NULL)
    {
        xSemaphoreGive(s_life);
    }

    if (rc != ELM327_OK)
    {
        return ESP_FAIL;
    }

    return adopted ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t can_manager_subscribe_queue(QueueHandle_t q, uint32_t filter,
                                      uint32_t mask, bool ext,
                                      bool monitor_all, int *out_idx)
{
    if (!s_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (q == NULL || out_idx == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    can_core_queue_subscriber_t sub =
    {
        .queue       = q,
        .filter      = filter,
        .mask        = mask,
        .ext         = ext,
        .monitor_all = monitor_all,
        .active      = true,
    };

    int idx = can_core_register_rx_queue(&s_bus, &sub);

    if (idx < 0)
    {
        return ESP_ERR_NO_MEM;
    }

    *out_idx = idx;
    return ESP_OK;
}

esp_err_t can_manager_unsubscribe_queue(int idx)
{
    if (!s_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    can_core_unregister_rx_queue(&s_bus, idx);
    return ESP_OK;
}

esp_err_t can_manager_subscriber_name(int idx, const char *name)
{
    if (!s_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    can_core_set_rx_queue_name(&s_bus, idx, name);
    return ESP_OK;
}

bool can_manager_subscriber_get(int idx, can_manager_subscriber_t *out)
{
    can_core_queue_subscriber_t sub;

    if (out == NULL || !s_running ||
        !can_core_get_rx_queue(&s_bus, idx, &sub))
    {
        return false;
    }

    out->name = (sub.name != NULL) ? sub.name : "";
    out->drops = sub.drops;
    return true;
}

void can_manager_capacity(size_t *used, size_t *cap)
{
    if (used != NULL)
    {
        *used = s_running ? (size_t)can_core_rx_queue_count(&s_bus) : 0;
    }

    if (cap != NULL)
    {
        *cap = CAN_CORE_MAX_QUEUE_SUBSCRIBERS;
    }
}

esp_err_t can_manager_status(can_manager_status_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->enabled = canm_settings_enabled();
    out->running = s_running;
    out->silent = canm_settings_silent();
    out->baud_kbps = canm_settings_baud_kbps();
    out->baud_auto = (out->baud_kbps == 0);
    out->link.state = "stopped";
    out->link.listen_only = true;

    if (s_running)
    {
        can_core_get_stats(&s_bus, &out->stats);
        can_core_get_link(&s_bus, &out->link);
        out->baud_kbps = out->link.baud_kbps;
    }

    return ESP_OK;
}

void can_manager_zero_stats(void)
{
    if (s_running)
    {
        can_core_reset_stats(&s_bus);
    }
}
