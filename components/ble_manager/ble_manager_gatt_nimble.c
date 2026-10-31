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
 * @file ble_manager_gatt_nimble.c
 * @brief The NimBLE GATT backend - the SAME on-air contract as
 *        ble_manager_gatt.c (Bluedroid), selected by the sdkconfig BT
 *        host choice (CONFIG_BT_NIMBLE_ENABLED) for the memory/perf
 *        A/B meatpi asked for (2026-07-05). Same UUIDs, Device Info
 *        strings, security level (SC+MITM+BOND, static passkey),
 *        MTU 517, adv layout and conn-window request. The service
 *        table + access callbacks live in ble_manager_gatt_svc.c; this
 *        file owns GAP, security, advertising and the notify path.
 *
 *        Stack-mapping notes:
 *         - free-packets pacing: NimBLE exposes no controller-credit
 *           count; notify_* do a bounded ENOMEM retry instead and feed
 *           a SELF-EXPIRING congestion window (ble_manager_gatt_tx.c), which the IO layer's
 *           pacing tolerates. (The 2026-09-21 slcan bench found the
 *           former latch never cleared once the TX task stopped
 *           notifying while it was set: BLE TX dead for the boot.)
 *         - CCCD security: NimBLE has no per-CCCD permission flags;
 *           notify_* additionally refuse until the link is secured
 *           (same effective policy as legacy's ENC_MITM CCCDs).
 *         - long CLI writes: NimBLE reassembles queued writes and
 *           delivers ONE access call - newline-split as usual.
 *         - the host task (2026-11-01): created here on a PSRAM stack
 *           instead of by nimble_port_freertos_init(), which takes 4 KB
 *           of the internal heap; the bond store's NVS writes that made
 *           the stack internal run on the flash_jobs worker
 *           (ble_manager_store_nvs.c wraps them and the store's load).
 */
#include <string.h>

#include "sdkconfig.h"

#include "esp_attr.h"
#include "esp_bt.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "ble_manager.h"
#include "ble_manager_ctrl.h"
#include "ble_manager_private.h"

/* NimBLE's NVS-backed key store: the config store module registers the
   read/write/delete callbacks (no public header in the IDF include path) */
extern void ble_store_config_init(void);

static const char *TAG = "ble_manager";

/* ---- state ---------------------------------------------------------------------- */

static const char *s_dev_name;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_mtu = 23;
static uint16_t s_max_data = 20;
static volatile bool s_connected;
static volatile bool s_secured;
static uint8_t s_own_addr_type;

/* the live link's PHYs (BLE_HCI_LE_PHY_1M/2M/CODED = 1/2/3), 0 = none */
static volatile uint8_t s_phy_tx;
static volatile uint8_t s_phy_rx;

static bool s_phy_requested;           /* one PHY request per link */

/* the `phy` setting: ask for the preferred PHY(s); the central decides (a
   4.2 peer keeps 1M), the result comes back as PHY_UPDATE_COMPLETE */
static void phy_request(void)
{
    uint8_t mask = blm_core_config()->phy_mask;

    if (mask == BLM_PHY_1M || s_phy_requested || !s_connected)
    {
        return;
    }

    if ((mask == BLM_PHY_2M && s_phy_tx == BLE_HCI_LE_PHY_2M) ||
        (mask == BLM_PHY_CODED && s_phy_tx == BLE_HCI_LE_PHY_CODED))
    {
        s_phy_requested = true; /* the controller's default preference already did it */
        return;
    }

    s_phy_requested = true;

    int prc = ble_gap_set_prefered_le_phy(s_conn_handle, mask, mask,
                                          BLE_GAP_LE_PHY_CODED_ANY);

    if (prc != 0)
    {
        ESP_LOGW(TAG, "PHY preference 0x%02x refused (%d)", mask, prc);
    }
}

/* ---- GAP ------------------------------------------------------------------------------ */

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type)
    {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status != 0)
            {
                blm_adv_start();
                break;
            }

            s_conn_handle = event->connect.conn_handle;
            s_connected = true;
            s_secured = false;
            s_phy_tx = BLE_HCI_LE_PHY_1M; /* every link starts on 1M */
            s_phy_rx = BLE_HCI_LE_PHY_1M;
            blm_tx_on_link_reset();
            blm_svc_reset_rx();
            blm_adv_stop(); /* one central: the other advertising set too */

            {
                /* the configured connection window (conn_profile) */
                struct ble_gap_upd_params params =
                {
                    .itvl_min = blm_core_config()->conn_min_units,
                    .itvl_max = blm_core_config()->conn_max_units,
                    .latency = 0,
                    .supervision_timeout = 400, /* 4 s (legacy) */
                };

                ble_gap_update_params(s_conn_handle, &params);
            }

            /* the `phy` setting is requested in CONN_UPDATE / ENC_CHANGE
               (phy_request), once the 4 s supervision timeout above is in
               force: asked here, at connect, the 2M switch happened under
               the central's default 420 ms timeout and the link dropped
               with reason 0x08 every time (UB500/BlueZ, 2026-09-21) */
            s_phy_requested = false;

            /* NOTE: do NOT request Data Length Extension here. Tried
               2026-07-05 (the esp-idf ble_throughput demo's lever):
               with WiFi coex active the 251-byte/2.1 ms LL packets
               collide with WiFi airtime and TX COLLAPSED 28 -> 2 KB/s,
               plus connect instability. The demo's numbers assume a
               radio without WiFi. Controllers may still negotiate DLE
               on their own - just don't force long TX packets. */

            /* No Security Request from our side at connect (the legacy
               esp_ble_set_encryption mirror, removed 2026-09-21): phones
               pair on their first encrypted access anyway, and a
               peripheral-initiated request makes Windows start a
               system pairing that an app's own ceremony then cannot join
               (WinRT custom pairing "Failed", no ceremony asked; the DUT
               saw ENOTCONN 2 s in). The runtime pairing window still
               applies through PASSKEY_ACTION. */

            blm_core_on_connect();
            blm_channel_on_link(BLE_MANAGER_CH_CONNECTED);
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            s_connected = false;
            s_secured = false;
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_phy_tx = 0;
            s_phy_rx = 0;
            blm_tx_on_link_reset(); /* + wakes an indication waiter */
            blm_svc_reset_rx();
            blm_core_on_disconnect();
            blm_channel_on_link(BLE_MANAGER_CH_DISCONNECTED);
            blm_adv_start(); /* re-advertise the configured set(s) */
            break;

        case BLE_GAP_EVENT_CONN_UPDATE:
            if (event->conn_update.status == 0)
            {
                phy_request(); /* our 4 s supervision timeout is live now */
            }
            break;

        case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
            if (event->phy_updated.status == 0)
            {
                s_phy_tx = event->phy_updated.tx_phy;
                s_phy_rx = event->phy_updated.rx_phy;
                ESP_LOGI(TAG, "PHY now tx %s rx %s",
                         s_phy_tx == BLE_HCI_LE_PHY_2M ? "2M" :
                         s_phy_tx == BLE_HCI_LE_PHY_CODED ? "coded" : "1M",
                         s_phy_rx == BLE_HCI_LE_PHY_2M ? "2M" :
                         s_phy_rx == BLE_HCI_LE_PHY_CODED ? "coded" : "1M");
            }
            else
            {
                ESP_LOGW(TAG, "PHY update failed (%d); staying on the current PHY",
                         event->phy_updated.status);
            }
            break;

        case BLE_GAP_EVENT_ADV_COMPLETE:
            /* an advertising instance stopped (the one that produced the
               connection, or our own stop); DISCONNECT restarts the set(s) */
            break;

        case BLE_GAP_EVENT_SUBSCRIBE:
            /* the central wrote a CCCD (or a bonded one's was restored):
               a stream channel's OUT mode follows it */
            blm_channel_on_subscribe(event->subscribe.attr_handle,
                                     event->subscribe.cur_notify,
                                     event->subscribe.cur_indicate);
            break;

        case BLE_GAP_EVENT_NOTIFY_TX:
            /* status 0 = the PDU went to the controller; for an indication
               EDONE = the central confirmed it, ETIMEOUT = it did not */
            if (event->notify_tx.indication &&
                event->notify_tx.status != 0)
            {
                blm_tx_on_indicate_done(event->notify_tx.status);
            }
            break;

        case BLE_GAP_EVENT_ENC_CHANGE:
            phy_request(); /* fallback when the central kept its own parameters */
        {
            struct ble_gap_conn_desc desc;

            if (event->enc_change.status == 0 &&
                ble_gap_conn_find(event->enc_change.conn_handle,
                                  &desc) == 0)
            {
                s_secured = desc.sec_state.encrypted &&
                            desc.sec_state.authenticated;
                ESP_LOGD(TAG, "sec: enc=%d authen=%d bonded=%d keysz=%d",
                         desc.sec_state.encrypted,
                         desc.sec_state.authenticated,
                         desc.sec_state.bonded, desc.sec_state.key_size);
            }

            if (s_secured)
            {
                ESP_LOGI(TAG, "pairing success");
            }
            else
            {
                /* NimBLE status: BLE_HS_SM_US_ERR(x) = 0x500+x our side,
                   BLE_HS_SM_PEER_ERR(x) = 0x600+x the peer's SMP reason */
                ESP_LOGW(TAG, "pairing failed (status 0x%03x)", event->enc_change.status);
            }

            if (s_secured)
            {
                blm_channel_on_link(BLE_MANAGER_CH_SECURED);
            }
            break;
        }

        case BLE_GAP_EVENT_PASSKEY_ACTION:
            if (!blm_core_pairing_allowed())
            {
                /* the runtime pairing window (legacy SEC_REQ veto) */
                ESP_LOGW(TAG, "pairing refused (window closed)");
                return BLE_HS_EREJECT;
            }

            if (event->passkey.params.action == BLE_SM_IOACT_DISP)
            {
                /* the static passkey is "displayed" (never logged) */
                struct ble_sm_io io =
                {
                    .action = BLE_SM_IOACT_DISP,
                    .passkey = blm_core_config()->passkey,
                };

                ble_sm_inject_io(event->passkey.conn_handle, &io);
            }
            break;

        case BLE_GAP_EVENT_MTU:
            s_mtu = event->mtu.value;
            s_max_data = (BLM_SEND_BUF_SIZE <= s_mtu - 3)
                             ? BLM_SEND_BUF_SIZE : (uint16_t)(s_mtu - 3);
            ESP_LOGI(TAG, "MTU %u (payload %u)", s_mtu, s_max_data);
            break;

        case BLE_GAP_EVENT_REPEAT_PAIRING:
        {
            /* peer lost its bond: delete ours and re-pair (bench-friendly;
               the stale-bond disconnect gotcha from the Pi bench) */
            struct ble_gap_conn_desc desc;

            if (ble_gap_conn_find(event->repeat_pairing.conn_handle,
                                  &desc) == 0)
            {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }

            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }

        default:
            break;
    }

    return 0;
}

/* ---- host sync: address, PHY preference, advertising sets ------------------------------- */

static void on_sync(void)
{
    const blm_config_t *cfg = blm_core_config();

    /* attribute handles are assigned by now (ble_gatts_start ran) */
    blm_svc_publish_handles();

    /* resolvable private address like legacy; public as fallback */
    if (ble_hs_util_ensure_addr(0) != 0 ||
        ble_hs_id_infer_auto(1, &s_own_addr_type) != 0)
    {
        s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
    }

    /* the `phy` setting as the controller's default preference for new
       links: it is what our LL answers a central's PHY request with, so
       `1m` (mask 0x01) really keeps a 5.0 central on 1M (a PC's Intel
       adapter asked for 2M on its own and got it while the preference was
       unset, 2026-09-21), and `2m`/`coded`/`auto` make the controller
       start the switch itself right after the connection */
    {
        int prc = ble_gap_set_prefered_default_le_phy(cfg->phy_mask,
                                                      cfg->phy_mask);

        if (prc != 0)
        {
            ESP_LOGW(TAG, "default PHY preference 0x%02x refused (%d)",
                     cfg->phy_mask, prc);
        }
    }

    /* the advertising set(s) per the `advertising` setting (the adv file
       owns the on-air bytes; the legacy set is byte-identical to 2026-07) */
    if (blm_adv_configure(s_own_addr_type, s_dev_name, cfg->adv_mode,
                          cfg->phy_mask, gap_event) == ESP_OK)
    {
        blm_adv_start();
    }
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset (%d)", reason);
    s_connected = false;
    s_secured = false;
    blm_tx_on_link_reset();
}

/* ---- the host task ------------------------------------------------------------------------ */

/* The NimBLE host task on a PSRAM stack (2026-11-01, lever 3 of
   internal/TASK_ble_tailscale_margin.md). IDF's esp_nimble_enable() would
   create it with a 4 KB stack from the internal heap, and it had to be
   internal because the bond store writes NVS from this task; those writes
   run on the flash_jobs worker now (ble_manager_store_nvs.c wraps the
   persist functions and the store's load, which the host also calls from
   this task at startup), and nothing else the task runs touches the flash
   (HCI, ATT, the security manager's ECDH, the GATT callbacks, which the
   channel owners keep to flag-and-wake). The size is the port's 4 KB
   (CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE) plus 2 KB: blesec measured
   1028 B of headroom on 4 KB right after a pairing (the ECDH of LE Secure
   Connections is the deepest path the task runs), the standard's line,
   and a PSRAM stack's overflow is silent (section 2) while its bytes cost
   nothing internal. The task's watermark is on the task monitor whenever
   it exists, blesec reads it after the pairing and stackaudit asserts the
   placement.

   One task for the firmware's life: created at the first bring-up, it runs
   nimble_port_run() until nimble_port_stop() returns it, then parks on a
   notification until the next bring-up. Nothing is ever deleted or freed
   (the TCB is a static, the stack a PSRAM static), so the restart path
   (interface_manager stops and starts the stack around an access-point
   station, the config mode, the sleep path) has no task lifetime to
   race: the stopper waits for the park before it tears the port down,
   which is also one step cleaner than the port's own order (the host
   task deleting itself while nimble_port_deinit() ran beside it). */
/* the port's NIMBLE_HS_STACK_SIZE is this Kconfig value; named here so the
   static stack audit (tools/stack_audit.py) resolves the size from
   sdkconfig.h */
#define BLM_HOST_STACK_BYTES (CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE + 2048)

static StaticTask_t s_host_tcb; /* internal: FreeRTOS TCB */
static StackType_t  s_host_stack[BLM_HOST_STACK_BYTES] EXT_RAM_BSS_ATTR;
static TaskHandle_t s_host_task;
static SemaphoreHandle_t s_host_parked;     /* the task -> the stopper */
static StaticSemaphore_t s_host_parked_buf; /* internal: FreeRTOS object */

static void host_task(void *arg)
{
    (void)arg;

    for (;;)
    {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY); /* a bring-up */
        nimble_port_run();                             /* until nimble_port_stop() */
        ESP_LOGD(TAG, "host task parked: stack_hw=%u",
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
        xSemaphoreGive(s_host_parked);
    }
}

/* The task's first creation (nothing on later bring-ups). The values are
   esp_nimble_enable()'s (nimble_port_freertos.c) but the stack size: the
   priority configMAX_PRIORITIES - 4, pinned to NIMBLE_CORE. The task
   waits for host_task_wake(); created and woken apart so the bring-up's
   per-step line measures the creation alone (the host's startup, which
   runs at once after the wake, loads the bond store through the flash
   worker, whose 4.4 KB internal stack lives for 5 s after that job). */
static esp_err_t host_task_create(void)
{
    if (s_host_parked == NULL)
    {
        s_host_parked = xSemaphoreCreateBinaryStatic(&s_host_parked_buf);
    }

    if (s_host_task == NULL)
    {
        s_host_task = xTaskCreateStaticPinnedToCore(
            host_task, "nimble_host",
            sizeof(s_host_stack) / sizeof(s_host_stack[0]), NULL,
            configMAX_PRIORITIES - 4, s_host_stack, &s_host_tcb, NIMBLE_CORE);
    }

    return s_host_task != NULL ? ESP_OK : ESP_FAIL;
}

static void host_task_wake(void)
{
    (void)xSemaphoreTake(s_host_parked, 0); /* a stale park, if any */
    xTaskNotifyGive(s_host_task);
}

/* After nimble_port_stop(): the task is past nimble_port_run()'s last
   NimBLE call once it parks, and only then may the port be torn down.
   Bounded: the stop procedure already waited for the host task's own
   acknowledgement of the stop event, so the park follows at once. */
static void host_task_join(void)
{
    if (s_host_task == NULL)
    {
        return;
    }

    if (xSemaphoreTake(s_host_parked, pdMS_TO_TICKS(2000)) != pdTRUE)
    {
        ESP_LOGW(TAG, "host task did not park after the stop");
    }
}

/* ---- stack lifecycle ---------------------------------------------------------------------- */

esp_err_t blm_gatt_stack_up(const char *dev_name)
{
    const blm_config_t *cfg = blm_core_config();

    s_dev_name = dev_name;

    blm_tx_init(); /* the indication semaphores, once */

    /* the controller sized by the memory profile (v5): nimble_port_init()
       builds BT_CONTROLLER_INIT_CONFIG_DEFAULT() itself and refuses a
       controller that is already up, so its three steps run here
       (nimble_port.c: init, enable, then the host) with the same unwinding */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

    blm_ctrl_configure(&bt_cfg);

    /* the internal heap at each step of the bring-up: the memory profile
       moves the first number, the rest is the stack's structure (the
       controller task's stack is internal by IDF's design; the host task
       reads 0 since 2026-11-01, its stack a PSRAM static and its TCB a
       static of this file, and 4.4 KB before that) */
    size_t ram[6];

    ram[0] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    esp_err_t err = esp_bt_controller_init(&bt_cfg);

    ram[1] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    if (err == ESP_OK)
    {
        err = esp_bt_controller_enable(ESP_BT_MODE_BLE);

        if (err != ESP_OK)
        {
            (void)esp_bt_controller_deinit();
        }
    }

    ram[2] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    if (err == ESP_OK)
    {
        err = esp_nimble_init(); /* the host; nimble_port_deinit() undoes all three */

        if (err != ESP_OK)
        {
            (void)esp_bt_controller_disable();
            (void)esp_bt_controller_deinit();
        }
    }

    ram[3] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "NimBLE bring-up failed: %s", esp_err_to_name(err));
        return err;
    }

    /* security block: LE Secure Connections + MITM (static passkey via
       DisplayOnly). Bonding (long-term key exchange/retention) is a
       setting - OFF still encrypts+authenticates per session but keeps no
       reusable key, so every reconnect re-pairs. */
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding = cfg->bonding ? 1 : 0;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    /* refuse the legacy-pairing downgrade unless a pre-4.2 accessory
       needs it (v3, default on) */
    ble_hs_cfg.sm_sc_only = cfg->sc_only ? 1 : 0;
    /* only exchange the long-term enc + identity keys when bonding; with
       bonding off there is nothing reusable to distribute or store */
    ble_hs_cfg.sm_our_key_dist = cfg->bonding
        ? (BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID) : 0;
    ble_hs_cfg.sm_their_key_dist = cfg->bonding
        ? (BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID) : 0;

    /* persistent bond store (CONFIG_BT_NIMBLE_NVS_PERSIST): keeps the
       long-term keys in NVS so a bonded phone reconnects WITHOUT a fresh
       passkey, across reboots/power-cycles. Round-robin overflow evicts
       the oldest bond once MAX_BONDS (3) is reached instead of failing. */
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
#if !CONFIG_BT_NIMBLE_STATIC_TO_DYNAMIC
    /* the load reads every table from NVS and sets the store callbacks: it
       lands in the wrapper of ble_manager_store_nvs.c (a flash job). With
       CONFIG_BT_NIMBLE_STATIC_TO_DYNAMIC (this build) the host does the
       same call itself from its own task at every startup
       (ble_hs_pvcy_set_default_irk), before any pairing can need the
       store, so the call here would only load the tables twice */
    ble_store_config_init();
#endif

    ble_svc_gap_init();
    ble_svc_gatt_init();

    err = blm_svc_register(dev_name);

    if (err != ESP_OK)
    {
        return err;
    }

    ble_att_set_preferred_mtu(517); /* legacy */

    /* TX power (settings, clamped) - controller API, stack-agnostic */
    esp_power_level_t lvl;

    switch (cfg->tx_power_dbm)
    {
        case -12: lvl = ESP_PWR_LVL_N12; break;
        case -9:  lvl = ESP_PWR_LVL_N9;  break;
        case -6:  lvl = ESP_PWR_LVL_N6;  break;
        case -3:  lvl = ESP_PWR_LVL_N3;  break;
        case 0:   lvl = ESP_PWR_LVL_N0;  break;
        case 3:   lvl = ESP_PWR_LVL_P3;  break;
        case 6:   lvl = ESP_PWR_LVL_P6;  break;
        default:  lvl = ESP_PWR_LVL_P9;  break;
    }

    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, lvl);

    ram[4] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    err = host_task_create();

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "NimBLE host task not created");
        nimble_port_deinit();
        return err;
    }

    ram[5] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "internal RAM per step: controller init %d, enable %d, host init %d, "
             "services %d, host task %d B (%u free before)",
             (int)ram[0] - (int)ram[1], (int)ram[1] - (int)ram[2],
             (int)ram[2] - (int)ram[3], (int)ram[3] - (int)ram[4],
             (int)ram[4] - (int)ram[5], (unsigned)ram[0]);
    host_task_wake();
    return ESP_OK;
}

void blm_gatt_stack_down(void)
{
    if (nimble_port_stop() == 0)
    {
        host_task_join(); /* the host task parked: past its last port call */
        nimble_port_deinit();
    }

    s_connected = false;
    s_secured = false;
    blm_tx_on_link_reset();
}

/* ---- accessors for the IO layer -------------------------------------------------------------- */

bool blm_gatt_connected(void)
{
    return s_connected;
}

bool blm_gatt_secured(void)
{
    return s_secured;
}

uint16_t blm_gatt_conn_handle(void)
{
    return s_conn_handle;
}

uint16_t blm_gatt_max_data(void)
{
    return s_max_data;
}

uint16_t blm_gatt_channel_out_handle(int idx)
{
    return blm_svc_channel_out_handle(idx);
}

void blm_gatt_phy(uint8_t *tx, uint8_t *rx)
{
    uint8_t t = 0;
    uint8_t r = 0;

    if (s_connected)
    {
        /* the controller's answer wins over the cached event values */
        if (ble_gap_read_le_phy(s_conn_handle, &t, &r) != 0)
        {
            t = s_phy_tx;
            r = s_phy_rx;
        }
    }

    if (tx != NULL) *tx = t;
    if (rx != NULL) *rx = r;
}

void blm_gatt_allow_pairing(bool allow)
{
    /* legacy: enabling pairing mid-connection kicks off encryption */
    if (allow && s_connected && !s_secured)
    {
        ble_gap_security_initiate(s_conn_handle);
    }
}
