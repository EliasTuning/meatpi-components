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
 * @file ble_manager_http.c
 * @brief GET /api/ble: link state + the stream-channel registry with its
 *        per-channel counters (rx/tx bytes, overflow, credit timeouts).
 *        The bench's witness for the BLE transports; the web UI's BLE
 *        status card. Documented in components/HTTP_API.md (6e15).
 */
#include <stdio.h>
#include <stdlib.h>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "http_server_manager.h"

#include "ble_manager.h"
#include "ble_manager_ctrl.h"
#include "ble_manager_private.h"

static const char *TAG = "ble_manager";

/* the bench blast (POST /api/ble/blast, below): progress for GET /api/ble */
static volatile bool     s_blast_running;
static volatile uint32_t s_blast_bytes, s_blast_target, s_blast_ms;

/* esp_http_server has no 409 helper: a JSON error with an explicit status */
static esp_err_t send_conflict(httpd_req_t *req, const char *why)
{
    char body[96];

    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", why);
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();

    cJSON_AddBoolToObject(o, "enabled", ble_manager_is_enabled());
    cJSON_AddBoolToObject(o, "connected", ble_manager_is_connected());
    cJSON_AddBoolToObject(o, "secured", ble_manager_is_secured());
    cJSON_AddBoolToObject(o, "pairing_enabled", ble_manager_pairing_is_enabled());
    cJSON_AddNumberToObject(o, "max_payload", blm_gatt_max_data());

    /* v6 (2026-11-02): the address the device advertises with and its kind.
       `address` is the public (hardware) address: the one on air under
       `public`, the identity a bonded phone resolves to under `private`. */
    {
        char addr[18] = "";

        cJSON_AddStringToObject(o, "address_type",
                                blm_ident_address_word(blm_core_config()->address_private));
        cJSON_AddStringToObject(o, "address",
                                blm_gatt_address(addr, sizeof(addr)) == ESP_OK ? addr : "");
        cJSON_AddBoolToObject(o, "security_request", blm_core_config()->security_request);
    }

    /* BLE 5 as settings (v4): the preference and the live link's PHYs */
    {
        const blm_config_t *cfg = blm_core_config();
        uint8_t tx = 0;
        uint8_t rx = 0;

        blm_gatt_phy(&tx, &rx);
        cJSON_AddStringToObject(o, "phy",
            cfg->phy_mask == BLM_PHY_2M ? "2m" :
            cfg->phy_mask == BLM_PHY_CODED ? "coded" :
            cfg->phy_mask == (BLM_PHY_1M | BLM_PHY_2M) ? "auto" : "1m");
        cJSON_AddStringToObject(o, "advertising",
            cfg->adv_mode == BLM_ADV_EXTENDED ? "extended" :
            cfg->adv_mode == BLM_ADV_BOTH ? "both" : "legacy");
        cJSON_AddNumberToObject(o, "phy_tx", tx); /* 1 = 1M, 2 = 2M, 3 = coded */
        cJSON_AddNumberToObject(o, "phy_rx", rx);

        /* the memory profile (v5): the word and the controller sizing the
           last bring-up applied (the bench asserts the active value) */
        const blm_ram_cfg_t *ram = blm_ctrl_applied();

        cJSON_AddStringToObject(o, "ram_profile",
                                blm_ident_ram_profile_word(cfg->ram_profile));

        if (ram != NULL)
        {
            cJSON *r = cJSON_AddObjectToObject(o, "ram");

            cJSON_AddNumberToObject(r, "max_act", ram->max_act);
            cJSON_AddNumberToObject(r, "acl_tx_bufs", ram->acl_tx_bufs);
            cJSON_AddNumberToObject(r, "scan_cache", ram->scan_cache);
            cJSON_AddBoolToObject(r, "ble5", ram->ble5);
        }
    }

    {
        /* the bench blast (POST /api/ble/blast) */
        cJSON *bl = cJSON_AddObjectToObject(o, "blast");

        cJSON_AddBoolToObject(bl, "running", s_blast_running);
        cJSON_AddNumberToObject(bl, "bytes", s_blast_bytes);
        cJSON_AddNumberToObject(bl, "target", s_blast_target);
        cJSON_AddNumberToObject(bl, "ms", s_blast_ms);
    }

    size_t used, cap;

    ble_manager_channel_capacity(&used, &cap);
    cJSON_AddNumberToObject(o, "channels_cap", cap);

    cJSON *arr = cJSON_AddArrayToObject(o, "channels");

    for (int i = 0; i < (int)used; i++)
    {
        const ble_manager_channel_desc_t *d = blm_channel_desc(i);
        ble_manager_channel_stats_t st;

        if (d == NULL || ble_manager_channel_stats(i, &st) != ESP_OK)
        {
            continue;
        }

        cJSON *c = cJSON_CreateObject();
        char uuid[8];

        cJSON_AddStringToObject(c, "name", d->name);
        snprintf(uuid, sizeof(uuid), "%04X", d->uuid_out);
        cJSON_AddStringToObject(c, "uuid_out", uuid);
        snprintf(uuid, sizeof(uuid), "%04X", d->uuid_in);
        cJSON_AddStringToObject(c, "uuid_in", uuid);
        cJSON_AddNumberToObject(c, "rx_size", d->rx_size);
        cJSON_AddNumberToObject(c, "rx_bytes", st.rx_bytes);
        cJSON_AddNumberToObject(c, "tx_bytes", st.tx_bytes);
        cJSON_AddNumberToObject(c, "rx_overflow", st.rx_overflow);
        cJSON_AddNumberToObject(c, "tx_timeouts", st.tx_timeouts);
        cJSON_AddNumberToObject(c, "tx_link_down", st.tx_link_down);
        cJSON_AddNumberToObject(c, "rx_pending", st.rx_pending);
        cJSON_AddStringToObject(c, "out", ble_manager_channel_out_name(st.out_mode));
        {
            /* what the owner allows: "indicate", "notify" or "both" */
            uint8_t m = d->out_modes ? d->out_modes : BLE_MANAGER_CH_OUT_INDICATE;

            cJSON_AddStringToObject(c, "out_modes",
                                    m == (BLE_MANAGER_CH_OUT_INDICATE | BLE_MANAGER_CH_OUT_NOTIFY)
                                        ? "both" : ble_manager_channel_out_name(m));
        }
        cJSON_AddNumberToObject(c, "tx_notifications", st.tx_notifications);
        cJSON_AddNumberToObject(c, "tx_indications", st.tx_indications);
        cJSON_AddItemToArray(arr, c);
    }

    char *s = cJSON_PrintUnformatted(o);

    cJSON_Delete(o);

    if (s == NULL)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");

    esp_err_t r = httpd_resp_send(req, s, HTTPD_RESP_USE_STRLEN);

    free(s);
    return r;
}

/* ---- POST /api/ble/blast: pump N bytes of notifications on the data pipe ------------
 *
 * The bench's device->app throughput source (the July `blast` console
 * command, reachable over HTTP and therefore through the BLE tunnel while
 * WiFi is handed over). Runs on its own PSRAM-stack task; one at a time.
 * The pattern is a counting byte so the receiver can spot loss. */

static void blast_task(void *arg)
{
    static uint8_t buf[490] EXT_RAM_BSS_ATTR;
    size_t chunk = (size_t)(uintptr_t)arg;
    size_t sent = 0;
    int64_t t0 = esp_timer_get_time();

    for (size_t i = 0; i < sizeof(buf); i++)
    {
        buf[i] = (uint8_t)i;
    }

    while (sent < s_blast_target && ble_manager_is_connected())
    {
        size_t n = s_blast_target - sent < chunk ? s_blast_target - sent : chunk;

        if (ble_manager_send(buf, n) == ESP_OK)
        {
            sent += n;
            s_blast_bytes = sent;
        }
        else
        {
            /* queue full: one connection interval's worth of pause (a 2 ms
               retry produced ~500 refused sends per second and a W line
               per 100 of them in the IO layer, bench 2026-09-21) */
            vTaskDelay(pdMS_TO_TICKS(8));

            if (esp_timer_get_time() - t0 > 120000000LL)
            {
                break;
            }
        }
    }

    s_blast_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    ESP_LOGI(TAG, "blast: %lu of %lu B enqueued in %lu ms",
             (unsigned long)sent, (unsigned long)s_blast_target, (unsigned long)s_blast_ms);
    s_blast_running = false;
    vTaskDelete(NULL);
}

static esp_err_t blast_handler(httpd_req_t *req)
{
    char body[96];
    int  n = httpd_req_recv(req, body, sizeof(body) - 1);

    if (n <= 0)
    {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body");
    }

    body[n] = '\0';

    cJSON *in = cJSON_Parse(body);
    const cJSON *b = in ? cJSON_GetObjectItemCaseSensitive(in, "bytes") : NULL;
    const cJSON *c = in ? cJSON_GetObjectItemCaseSensitive(in, "chunk") : NULL;
    uint32_t bytes = cJSON_IsNumber(b) ? (uint32_t)b->valuedouble : 262144;
    uint32_t chunk = cJSON_IsNumber(c) ? (uint32_t)c->valuedouble : 490;

    cJSON_Delete(in);

    if (bytes == 0 || bytes > (8u << 20) || chunk == 0 || chunk > 490)
    {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bytes 1..8388608, chunk 1..490");
    }

    if (!ble_manager_is_connected())
    {
        return send_conflict(req, "no BLE link");
    }

    if (s_blast_running)
    {
        return send_conflict(req, "blast running");
    }

    s_blast_running = true;
    s_blast_bytes = 0;
    s_blast_ms = 0;
    s_blast_target = bytes;

    if (xTaskCreateWithCaps(blast_task, "ble_blast", 4096, (void *)(uintptr_t)chunk, 5, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
    {
        s_blast_running = false;
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "task");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"started\":true}");
}

esp_err_t ble_manager_register_http(void)
{
    static const httpd_uri_t URIS[] =
    {
        { .uri = "/api/ble", .method = HTTP_GET, .handler = status_handler },
        { .uri = "/api/ble/blast", .method = HTTP_POST, .handler = blast_handler },
    };

    esp_err_t err = http_server_manager_register_handlers(
        URIS, sizeof(URIS) / sizeof(URIS[0]));

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "/api/ble registered");
    }

    return err;
}
