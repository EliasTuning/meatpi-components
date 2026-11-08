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
 * @file ble_manager.c
 * @brief Lifecycle, subscriber/CLI registries, status. Settings live in
 *        ble_manager_settings.c (blm_core_config / blm_settings_*).
 */
#include "ble_manager.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "dev_status_manager.h"
#include "log_manager.h"

#include "ble_manager_private.h"

static const char *TAG = "ble_manager";

static bool s_started;

static QueueHandle_t s_sub;                 /* the ONE subscriber (bridge) */
static uint32_t s_sub_drops;
static ble_cli_handler_t s_cli_handler;
static volatile bool s_pairing_allowed = true;

/* the live session (2026-11-08): one log line per milestone, a summary at
   the end (ble_manager_diag.c makes the words). The rx side is written on
   the host task, the tx side on the IO task: two words, never shared */
static struct
{
    int64_t  t0_us;             /* the connect                             */
    uint32_t secured_at_ms;
    uint32_t first_write_at_ms;
    uint32_t last_write_at_ms;
    uint32_t rx_writes;
    uint32_t rx_bytes;
    uint32_t tx_notifications;
    uint32_t tx_bytes;
    uint32_t sub_drops_at_start;
    uint32_t tx_drops_at_start;
    bool     subscribed;
    bool     live;
} s_sess;

static uint32_t sess_ms(void)
{
    return (uint32_t)((esp_timer_get_time() - s_sess.t0_us) / 1000);
}

/* ---- lifecycle ----------------------------------------------------------------- */

esp_err_t ble_manager_init(void)
{
    static const log_descriptor_t LOG_DESC = { "ble_manager", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);
    return blm_settings_register();
}

esp_err_t ble_manager_start(void)
{
    const blm_config_t *cfg = blm_core_config();

    if (!blm_settings_is_configured())
    {
        ESP_LOGE(TAG, "unconfigured (settings boot pass failed); not starting");
        return ESP_ERR_INVALID_STATE; /* standard §4.3 step 5 */
    }

    if (s_started)
    {
        return ESP_OK;
    }

    if (!cfg->enabled)
    {
        ESP_LOGI(TAG, "disabled; BLE stack stays down");
        s_started = true;
        return ESP_OK;
    }

    s_pairing_allowed = cfg->pairing_at_boot;

    /* the on-air identity: "WiC_<device id>" (legacy ble_uid) */
    static char name[BLM_NAME_MAX]; /* GATT layer keeps a pointer */

    blm_ident_name(dev_status_manager_device_id(), name, sizeof(name));

    /* the GATT table is built from the channel registry now: freeze it
       (a late registrant would never get characteristics) */
    blm_channel_lock();

    esp_err_t err = blm_gatt_stack_up(name);

    if (err != ESP_OK)
    {
        return err;
    }

    err = blm_io_start();

    if (err != ESP_OK)
    {
        blm_gatt_stack_down();
        return err;
    }

    s_started = true;
    dev_status_manager_set(DEV_STATUS_BIT_BLE_ENABLED);
    ESP_LOGI(TAG, "started as '%s' (tx %d dBm, pairing %s)", name,
             cfg->tx_power_dbm, s_pairing_allowed ? "on" : "off");
    return ESP_OK;
}

esp_err_t ble_manager_stop(void)
{
    if (!s_started)
    {
        return ESP_OK;
    }

    if (blm_core_config()->enabled)
    {
        blm_io_stop();
        blm_gatt_stack_down();
    }

    s_started = false;
    dev_status_manager_clear(DEV_STATUS_BIT_BLE_ENABLED |
                             DEV_STATUS_BIT_BLE_CONNECTED);
    ESP_LOGI(TAG, "stopped");
    return ESP_OK;
}

/* ---- endpoint trio --------------------------------------------------------------- */

esp_err_t ble_manager_subscribe(QueueHandle_t q)
{
    if (q == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_sub != NULL)
    {
        return ESP_ERR_INVALID_STATE; /* one subscriber: one bridge */
    }

    s_sub = q;
    return ESP_OK;
}

esp_err_t ble_manager_unsubscribe(QueueHandle_t q)
{
    if (s_sub != q)
    {
        return ESP_ERR_NOT_FOUND;
    }

    s_sub = NULL;
    return ESP_OK;
}

esp_err_t ble_manager_send(const uint8_t *data, size_t len)
{
    if (!blm_gatt_connected())
    {
        return ESP_ERR_INVALID_STATE;
    }

    return blm_io_queue_tx(data, len);
}

/* ---- status ------------------------------------------------------------------------ */

bool ble_manager_is_enabled(void)
{
    /* the CONFIGURED truth: stays true while interface_manager holds
       the stack stopped (the BLE_ENABLED bit tracks the RUNNING state,
       which is what a runtime stop clears) */
    return blm_settings_is_configured() && blm_core_config()->enabled;
}

bool ble_manager_is_connected(void)
{
    return blm_gatt_connected();
}

bool ble_manager_is_secured(void)
{
    return blm_gatt_secured();
}

/* ---- pairing window ----------------------------------------------------------------- */

void ble_manager_pairing_enable(void)
{
    s_pairing_allowed = true;
    blm_gatt_allow_pairing(true);
    ESP_LOGI(TAG, "pairing ENABLED");
}

void ble_manager_pairing_disable(void)
{
    s_pairing_allowed = false;
    blm_gatt_allow_pairing(false);
    ESP_LOGI(TAG, "pairing DISABLED");
}

bool ble_manager_pairing_is_enabled(void)
{
    return s_pairing_allowed;
}

bool blm_core_pairing_allowed(void)
{
    return s_pairing_allowed;
}

/* ---- CLI ------------------------------------------------------------------------------ */

esp_err_t ble_manager_set_cli_handler(ble_cli_handler_t handler)
{
    s_cli_handler = handler;
    return ESP_OK;
}

esp_err_t ble_manager_cli_write(const char *data, size_t len)
{
    return blm_io_cli_write(data, len);
}

/* ---- bridges from the GATT layer -------------------------------------------------------- */

void blm_core_on_rx(const uint8_t *data, size_t len)
{
    if (s_sess.live)
    {
        uint32_t now = sess_ms();

        if (s_sess.first_write_at_ms == BLM_DIAG_NEVER)
        {
            s_sess.first_write_at_ms = now;
            ESP_LOGI(TAG, "first app write %lu ms after the connect (%s, %u B%s)",
                     (unsigned long)now, blm_gatt_secured() ? "secured" : "plain",
                     (unsigned)len, s_sub == NULL ? ", no bridge on the pipe" : "");
        }

        s_sess.last_write_at_ms = now;
        s_sess.rx_writes++;
        s_sess.rx_bytes += (uint32_t)len;
    }

    if (s_sub == NULL)
    {
        return; /* nobody attached: bytes fall on the floor by design */
    }

    ble_chunk_t chunk;

    while (len > 0)
    {
        size_t n = (len > BLE_MANAGER_CHUNK_SIZE) ? BLE_MANAGER_CHUNK_SIZE
                                                  : len;

        chunk.len = (uint16_t)n;
        memcpy(chunk.data, data, n);

        if (xQueueSend(s_sub, &chunk, 0) != pdTRUE)
        {
            s_sub_drops++; /* never block the BT callback */

            if ((s_sub_drops % 100) == 1)
            {
                ESP_LOGW(TAG, "subscriber queue full (%lu drops)",
                         (unsigned long)s_sub_drops);
            }
        }

        data += n;
        len -= n;
    }
}

void blm_core_on_cli_line(const char *line)
{
    if (s_cli_handler != NULL)
    {
        s_cli_handler(line);
    }
    else
    {
        ESP_LOGD(TAG, "CLI line dropped (no cmdline_manager yet): %s", line);
    }
}

void blm_core_on_connect(const blm_link_info_t *link)
{
    const blm_config_t *cfg = blm_core_config();
    char peer[24], win[64];

    memset(&s_sess, 0, sizeof(s_sess));
    s_sess.t0_us = esp_timer_get_time();
    s_sess.secured_at_ms = BLM_DIAG_NEVER;
    s_sess.first_write_at_ms = BLM_DIAG_NEVER;
    s_sess.last_write_at_ms = BLM_DIAG_NEVER;
    s_sess.sub_drops_at_start = s_sub_drops;
    s_sess.tx_drops_at_start = blm_io_tx_drops();
    s_sess.live = true;

    dev_status_manager_set(DEV_STATUS_BIT_BLE_CONNECTED);

    if (link == NULL)
    {
        ESP_LOGI(TAG, "client connected");
        return;
    }

    blm_diag_format_addr(link->peer, peer, sizeof(peer));
    blm_diag_format_window(link->itvl_units, link->latency,
                           link->timeout_units, win, sizeof(win));
    /* the window the central opened with, and the one this device asks
       for (conn_profile); a later `connection update` line says what the
       central settled on */
    ESP_LOGI(TAG, "client connected: peer %s (%s), %s; asking %u-%u ms",
             peer, blm_diag_addr_type_name(link->peer_type), win,
             (unsigned)(cfg->conn_min_units * 125 / 100),
             (unsigned)(cfg->conn_max_units * 125 / 100));
}

void blm_core_on_conn_update(const blm_link_info_t *link)
{
    char win[64];

    blm_diag_format_window(link->itvl_units, link->latency,
                           link->timeout_units, win, sizeof(win));
    ESP_LOGI(TAG, "connection update: %s (%lu ms after the connect)", win,
             (unsigned long)sess_ms());
}

void blm_core_on_secured(void)
{
    if (s_sess.live && s_sess.secured_at_ms == BLM_DIAG_NEVER)
    {
        s_sess.secured_at_ms = sess_ms();
    }
}

void blm_core_on_data_subscribe(bool notify, bool indicate, bool restored)
{
    s_sess.subscribed = s_sess.subscribed || notify || indicate; /* sticky: the summary says "was subscribed" */
    ESP_LOGI(TAG, "data pipe %s (%s%s%s, %lu ms after the connect)",
             (notify || indicate) ? "subscribed" : "unsubscribed",
             notify ? "notify" : indicate ? "indicate" : "none",
             restored ? ", restored from the bond" : ", by the central",
             blm_gatt_secured() ? "" : ", link still plain",
             (unsigned long)sess_ms());
}

void blm_core_on_tx(size_t len)
{
    if (s_sess.live)
    {
        s_sess.tx_notifications++;
        s_sess.tx_bytes += (uint32_t)len;
    }
}

void blm_core_on_disconnect(int reason)
{
    dev_status_manager_clear(DEV_STATUS_BIT_BLE_CONNECTED);

    if (!s_sess.live)
    {
        ESP_LOGI(TAG, "client disconnected");
        return;
    }

    blm_diag_session_t s =
    {
        .duration_ms = sess_ms(),
        .secured_at_ms = s_sess.secured_at_ms,
        .first_write_at_ms = s_sess.first_write_at_ms,
        .last_write_at_ms = s_sess.last_write_at_ms,
        .rx_writes = s_sess.rx_writes,
        .rx_bytes = s_sess.rx_bytes,
        .tx_notifications = s_sess.tx_notifications,
        .tx_bytes = s_sess.tx_bytes,
        .sub_drops = s_sub_drops - s_sess.sub_drops_at_start,
        .tx_drops = blm_io_tx_drops() - s_sess.tx_drops_at_start,
        .subscribed = s_sess.subscribed,
    };
    char text[240];

    s_sess.live = false;
    blm_diag_format_summary(&s, text, sizeof(text));
    ESP_LOGI(TAG, "client disconnected: %s (0x%x); %s",
             blm_diag_reason_name(reason), (unsigned)reason, text);
}
