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
 * @file bridge_endpoints.c
 * @brief Jack registration + the thin wrappers: obd_chip, ble_manager,
 *        cmdline_manager, and the per-slot socket/WS adapters registered
 *        under their CONFIGURED names. The stateful adapters live in
 *        bridge_endpoints_usb.c (usb_obd) and bridge_endpoints_can.c
 *        (can). Add-on packs register further jacks directly with
 *        bridge_manager (ext init phase).
 */
#include "bridge_endpoints.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "ble_manager.h"
#include "bridge_manager.h"
#include "cmdline_manager.h"
#include "log_manager.h"
#include "obd_chip.h"
#include "socket_manager.h"
#include "websocket_manager.h"

#include "bridge_endpoints_private.h"

static const char *TAG = "bridge_endpoints";

_Static_assert(sizeof(bridge_chunk_t) == sizeof(obd_chunk_t),
               "chunk conventions diverged");
_Static_assert(sizeof(bridge_chunk_t) == sizeof(cmdline_chunk_t),
               "chunk conventions diverged");
_Static_assert(sizeof(bridge_chunk_t) == sizeof(socket_chunk_t),
               "chunk conventions diverged");
_Static_assert(sizeof(bridge_chunk_t) == sizeof(ble_chunk_t),
               "chunk conventions diverged");
_Static_assert(sizeof(bridge_chunk_t) == sizeof(websocket_chunk_t),
               "chunk conventions diverged");

/* ---- obd_chip ---------------------------------------------------------------- */

static esp_err_t obd_ep_send(const uint8_t *d, size_t l)
{
    /* every byte from a bridged app (TCP/BLE/USB/WS) counts as client
       activity: autopid yields the chip while the app drives it (legacy
       DEV_AUTOPID_ELM327_APP_BIT parity, see obd_chip.h) */
    obd_chip_client_touch();
    return obd_chip_send(d, l);
}

static esp_err_t obd_ep_subscribe(QueueHandle_t q)
{
    return obd_chip_subscribe(q, "bridge");
}

static esp_err_t obd_ep_unsubscribe(QueueHandle_t q)
{
    return obd_chip_unsubscribe(q);
}

/* ---- ble_manager --------------------------------------------------------------- */

static esp_err_t ble_ep_send(const uint8_t *d, size_t l)
{
    return ble_manager_send(d, l);
}

static esp_err_t ble_ep_subscribe(QueueHandle_t q)
{
    return ble_manager_subscribe(q);
}

static esp_err_t ble_ep_unsubscribe(QueueHandle_t q)
{
    return ble_manager_unsubscribe(q);
}

/* ---- cmdline_manager (the CLI reachable over any bridged transport) ------------ */

static esp_err_t cli_send(const uint8_t *d, size_t l)
{
    return cmdline_manager_send(d, l);
}

static esp_err_t cli_subscribe(QueueHandle_t q)
{
    return cmdline_manager_subscribe(q);
}

static esp_err_t cli_unsubscribe(QueueHandle_t q)
{
    return cmdline_manager_unsubscribe(q);
}

/* ---- socket/WS slots: jacks under their CONFIGURED names ------------------------ *
 * The endpoint ABI carries no ctx pointer, so each slot gets generated
 * wrappers bound to a name buffer filled at start() from the applied
 * settings. Renaming a server/channel renames its jack (bridges follow
 * the settings name: the old fixed-name-only wart is gone). */

static char s_sock_names[SOCKET_MANAGER_MAX_SERVERS][16];
static char s_ws_names[WEBSOCKET_MANAGER_MAX_CHANNELS][16];

#define SOCK_EP(i)                                                     \
    static esp_err_t sock##i##_send(const uint8_t *d, size_t l)       \
    {                                                                  \
        return socket_manager_send(s_sock_names[i], d, l);            \
    }                                                                  \
    static esp_err_t sock##i##_sub(QueueHandle_t q)                   \
    {                                                                  \
        return socket_manager_subscribe(s_sock_names[i], q);          \
    }                                                                  \
    static esp_err_t sock##i##_unsub(QueueHandle_t q)                 \
    {                                                                  \
        return socket_manager_unsubscribe(s_sock_names[i], q);        \
    }

SOCK_EP(0)
SOCK_EP(1)
SOCK_EP(2)
SOCK_EP(3)
#undef SOCK_EP

#define WS_EP(i)                                                       \
    static esp_err_t ws##i##_send(const uint8_t *d, size_t l)         \
    {                                                                  \
        return websocket_manager_send(s_ws_names[i], d, l);           \
    }                                                                  \
    static esp_err_t ws##i##_sub(QueueHandle_t q)                     \
    {                                                                  \
        return websocket_manager_subscribe(s_ws_names[i], q);         \
    }                                                                  \
    static esp_err_t ws##i##_unsub(QueueHandle_t q)                   \
    {                                                                  \
        return websocket_manager_unsubscribe(s_ws_names[i], q);       \
    }

WS_EP(0)
WS_EP(1)
WS_EP(2)
WS_EP(3)
WS_EP(4) /* MAX_CHANNELS grew 4 -> 6 with websocket_manager v2 (ws_log) */
WS_EP(5)
#undef WS_EP

static const bridge_endpoint_t SOCK_EPS[SOCKET_MANAGER_MAX_SERVERS] =
{
    { s_sock_names[0], sock0_send, sock0_sub, sock0_unsub },
    { s_sock_names[1], sock1_send, sock1_sub, sock1_unsub },
    { s_sock_names[2], sock2_send, sock2_sub, sock2_unsub },
    { s_sock_names[3], sock3_send, sock3_sub, sock3_unsub },
};

static const bridge_endpoint_t WS_EPS[WEBSOCKET_MANAGER_MAX_CHANNELS] =
{
    { s_ws_names[0], ws0_send, ws0_sub, ws0_unsub },
    { s_ws_names[1], ws1_send, ws1_sub, ws1_unsub },
    { s_ws_names[2], ws2_send, ws2_sub, ws2_unsub },
    { s_ws_names[3], ws3_send, ws3_sub, ws3_unsub },
    { s_ws_names[4], ws4_send, ws4_sub, ws4_unsub },
    { s_ws_names[5], ws5_send, ws5_sub, ws5_unsub },
};

/* ---- lifecycle ------------------------------------------------------------------ */

esp_err_t bridge_endpoints_init(void)
{
    bep_can_bind_restart();

    static const log_descriptor_t LOG_DESC =
        { "bridge_endpoints", ESP_LOG_INFO };
    static const bridge_endpoint_t FIXED[] =
    {
        /* multi_consumer: obd_chip fans RX out to every subscriber queue
           (each gets a full copy) and serializes TX, the single-consumer
           rule doesn't apply, so TCP + USB passthrough can both be live */
        { "obd", obd_ep_send, obd_ep_subscribe, obd_ep_unsubscribe,
          .multi_consumer = true },
        { "ble", ble_ep_send, ble_ep_subscribe, ble_ep_unsubscribe },
        /* multi_consumer since 2026-07-26: the can pump fans RX chunks
           out to every subscribed bridge (slcan + mqtt_can together);
           TX serializes through can_manager_send. ONE jack-wide ingress
           filter (bep_can_set_filter). */
        { "can", bep_can_send, bep_can_subscribe, bep_can_unsubscribe,
          .multi_consumer = true },
        { "cli", cli_send, cli_subscribe, cli_unsubscribe },
        { "usb_obd", bep_usb_send, bep_usb_subscribe, bep_usb_unsubscribe },
    };

    log_manager_register(&LOG_DESC);

    esp_err_t first_err = ESP_OK;

    for (size_t i = 0; i < sizeof(FIXED) / sizeof(FIXED[0]); i++)
    {
        esp_err_t err = bridge_manager_register_endpoint(&FIXED[i]);

        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "jack '%s' registration failed (%s)",
                     FIXED[i].name, esp_err_to_name(err));

            if (first_err == ESP_OK)
            {
                first_err = err;
            }
        }
    }

    return first_err;
}

esp_err_t bridge_endpoints_start(void)
{
    /* dynamic jacks: one per CONFIGURED server/channel, under its
       configured name (settings applied by now; registration must
       precede bridge_manager_start). A name colliding with an existing
       jack is refused by the registry: log and degrade alone. */
    for (int i = 0; i < SOCKET_MANAGER_MAX_SERVERS; i++)
    {
        const char *name = socket_manager_server_name(i);

        if (name == NULL)
        {
            break;
        }

        snprintf(s_sock_names[i], sizeof(s_sock_names[i]), "%s", name);

        if (bridge_manager_register_endpoint(&SOCK_EPS[i]) != ESP_OK)
        {
            ESP_LOGW(TAG, "socket jack '%s' not registered "
                     "(duplicate name?)", name);
        }
    }

    for (int i = 0; i < WEBSOCKET_MANAGER_MAX_CHANNELS; i++)
    {
        const char *name = websocket_manager_channel_name(i);

        if (name == NULL)
        {
            break;
        }

        snprintf(s_ws_names[i], sizeof(s_ws_names[i]), "%s", name);

        if (bridge_manager_register_endpoint(&WS_EPS[i]) != ESP_OK)
        {
            ESP_LOGW(TAG, "ws jack '%s' not registered "
                     "(duplicate name?)", name);
        }
    }

    return bep_usb_start();
}

esp_err_t bridge_endpoints_stop(void)
{
    /* jacks live as long as the registry; the adapters' own tasks park
       when unsubscribed (init-once, §3: no teardown path) */
    return ESP_OK;
}
