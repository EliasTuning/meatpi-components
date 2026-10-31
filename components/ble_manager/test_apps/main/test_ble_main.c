/**
 * @file test_ble_main.c
 * @brief On-target test app for ble_manager: the production composition,
 *        ble_manager as a bridge endpoint (`ble` ↔ `echo` raw bridge), so
 *        everything a client writes to FFF2 comes back as FFF1
 *        notifications; the CLI IN characteristic is wired to a stand-in
 *        handler (upper-cases the line and answers via CLI OUT) until the
 *        cmdline_manager exists.
 *
 * The device side only prints readiness markers: the real assertions run
 * from rpi001's BLE controller via tools/testbench/ble_bench.py (scan,
 * pair with the static passkey, subscribe, echo, CLI, throughput/RTT).
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "ble_manager.h"
#include "bridge_manager.h"
#include "dev_status_manager.h"
#include "flash_jobs.h"
#include "log_manager.h"
#include "settings_manager.h"

static const char *TAG = "ble_test";

_Static_assert(sizeof(bridge_chunk_t) == sizeof(ble_chunk_t),
               "chunk conventions diverged");

/* ---- endpoint glue (the promised three-liners) -------------------------------- */

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

/* echo endpoint: send() loops data back via its subscriber queue */
static QueueHandle_t s_echo_sub;

static esp_err_t echo_send(const uint8_t *d, size_t l)
{
    if (s_echo_sub == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    bridge_chunk_t chunk;

    while (l > 0)
    {
        size_t n = (l > BRIDGE_MANAGER_CHUNK_SIZE) ? BRIDGE_MANAGER_CHUNK_SIZE
                                                   : l;

        chunk.len = (uint16_t)n;
        memcpy(chunk.data, d, n);

        if (xQueueSend(s_echo_sub, &chunk, 0) != pdTRUE)
        {
            return ESP_FAIL;
        }

        d += n;
        l -= n;
    }

    return ESP_OK;
}

static esp_err_t echo_subscribe(QueueHandle_t q)
{
    s_echo_sub = q;
    return ESP_OK;
}

static esp_err_t echo_unsubscribe(QueueHandle_t q)
{
    (void)q;
    s_echo_sub = NULL;
    return ESP_OK;
}

/* benchmark: "blast <bytes>" on the CLI starts a TX-throughput burst, a
 * worker pushes patterned data through ble_manager_send (pacing against the
 * bounded TX queue exactly like a real bridge producer would) */
static volatile uint32_t s_blast_request;

static void blast_worker(void *arg)
{
    (void)arg;

    while (true)
    {
        if (s_blast_request == 0)
        {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        uint32_t remaining = s_blast_request;
        uint8_t pattern[BLE_MANAGER_CHUNK_SIZE];

        for (size_t i = 0; i < sizeof(pattern); i++)
        {
            pattern[i] = (uint8_t)i;
        }

        while (remaining > 0)
        {
            uint32_t n = (remaining > sizeof(pattern)) ? sizeof(pattern)
                                                       : remaining;

            if (ble_manager_send(pattern, n) == ESP_OK)
            {
                remaining -= n;
            }
            else
            {
                vTaskDelay(pdMS_TO_TICKS(2)); /* queue full: pace, don't drop */
            }
        }

        s_blast_request = 0;
        ESP_LOGI(TAG, "blast complete");
    }
}

/* stand-in CLI handler until cmdline_manager exists: upper-case echo,
 * plus the "blast <bytes>" benchmark trigger */
static void test_cli_handler(const char *line)
{
    if (strncmp(line, "blast ", 6) == 0)
    {
        s_blast_request = (uint32_t)atoi(line + 6);
        ble_manager_cli_write("BLASTING\n", 9);
        return;
    }

    char out[128];
    size_t n = 0;

    while (line[n] != '\0' && n < sizeof(out) - 2)
    {
        out[n] = (char)toupper((int)line[n]);
        n++;
    }

    out[n++] = '\n';
    ble_manager_cli_write(out, n);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    ESP_ERROR_CHECK(err);

    /* ---- compose like main -------------------------------------------- */
    ESP_ERROR_CHECK(log_manager_init());
    /* the bond store writes from the NimBLE host task, which runs on a
       PSRAM stack: its NVS work needs the flash worker (2026-11-01) */
    ESP_ERROR_CHECK(flash_jobs_init());
    ESP_ERROR_CHECK(dev_status_manager_init());
    ESP_ERROR_CHECK(settings_manager_init());
    ESP_ERROR_CHECK(log_manager_register_settings());
    ESP_ERROR_CHECK(ble_manager_init());
    ESP_ERROR_CHECK(bridge_manager_init());

    static const bridge_endpoint_t BLE_EP =
        { "ble", ble_ep_send, ble_ep_subscribe, ble_ep_unsubscribe };
    static const bridge_endpoint_t ECHO_EP =
        { "echo", echo_send, echo_subscribe, echo_unsubscribe };

    ESP_ERROR_CHECK(bridge_manager_register_endpoint(&BLE_EP));
    ESP_ERROR_CHECK(bridge_manager_register_endpoint(&ECHO_EP));

    /* enable BLE + the ble<->echo bridge, persisted before the boot pass.
       conn_profile: "ios" default here (the suite tests the default); the
       BENCHMARKS android_fast run flips this one string. */
    cJSON *cfg = cJSON_Parse(
        "{\"enabled\":true,\"passkey\":123456,\"tx_power_dbm\":9,"
        "\"pairing_at_boot\":true,\"conn_profile\":\"ios\"}");

    ESP_ERROR_CHECK(settings_manager_set("ble_manager", cfg, NULL, 0, NULL));
    cJSON_Delete(cfg);

    cfg = cJSON_Parse(
        "{\"bridges\":[{\"name\":\"br_ble\",\"a\":\"ble\",\"b\":\"echo\","
        "\"enabled\":true}]}");
    ESP_ERROR_CHECK(settings_manager_set("bridge_manager", cfg, NULL, 0,
                                         NULL));
    cJSON_Delete(cfg);

    ESP_ERROR_CHECK(settings_manager_start()); /* the one on_apply pass */
    ESP_ERROR_CHECK(log_manager_start());
    printf("INIT ok=1\n");

    ble_manager_set_cli_handler(test_cli_handler);

    /* PSRAM stack: no flash writes on this path (§2) */
    static StaticTask_t blast_tcb; /* internal: FreeRTOS object */
    static StackType_t blast_stack[3072] EXT_RAM_BSS_ATTR;

    xTaskCreateStatic(blast_worker, "blast", 3072, NULL, 5, blast_stack,
                      &blast_tcb);

    err = ble_manager_start();

    esp_err_t br_err = bridge_manager_start();

    printf("START ble=%d bridge=%d\n", err == ESP_OK, br_err == ESP_OK);
    printf("BLE READY name=WiC_%s\n", dev_status_manager_device_id());

    /* stay up for the Pi-driven scenarios; report link state transitions */
    bool was_connected = false;
    bool was_secured = false;

    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(500));

        bool connected = ble_manager_is_connected();
        bool secured = ble_manager_is_secured();

        if (connected != was_connected)
        {
            printf("LINK connected=%d\n", connected);
            was_connected = connected;
        }

        if (secured != was_secured)
        {
            printf("LINK secured=%d\n", secured);
            was_secured = secured;
        }
    }
}
