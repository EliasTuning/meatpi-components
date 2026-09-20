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
 * @file ble_manager_gatt.c
 * @brief The Bluedroid GATT server: attribute tables, GAP/GATTS handlers,
 *        advertising and security — the ON-AIR CONTRACT, preserved verbatim
 *        from the legacy ble.c (UUIDs, permissions, adv/conn parameters,
 *        security block). Deliberately dropped vs legacy: the sideways
 *        wifi/config-server calls on connect (policy belongs to the
 *        composition root — dev_status bits carry the signal instead).
 */
#include <string.h>

#include "esp_attr.h"
#include "esp_bt.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatts_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ble_manager.h"
#include "ble_manager_private.h"

static const char *TAG = "ble_manager";

#define ADV_CONFIG_FLAG      (1 << 0)
#define SCAN_RSP_CONFIG_FLAG (1 << 1)
#define BLM_APP_ID           0x56 /* legacy ESP_SPP_APP_ID */

#define DEV_INFO_SVC_INST_ID 0
#define FFF0_SVC_INST_ID     2

/* ---- attribute-table indices (legacy layout) --------------------------------- */

enum
{
    IDX_SVC_DEVICE_INFO,
    IDX_CHAR_MANUFACTURER_DECL, IDX_CHAR_MANUFACTURER_VAL,
    IDX_CHAR_MODEL_DECL,        IDX_CHAR_MODEL_VAL,
    IDX_CHAR_SERIAL_DECL,       IDX_CHAR_SERIAL_VAL,
    IDX_CHAR_HW_REV_DECL,       IDX_CHAR_HW_REV_VAL,
    IDX_CHAR_FW_REV_DECL,       IDX_CHAR_FW_REV_VAL,
    IDX_CHAR_SW_REV_DECL,       IDX_CHAR_SW_REV_VAL,
    IDX_CHAR_SYSTEM_ID_DECL,    IDX_CHAR_SYSTEM_ID_VAL,
    IDX_CHAR_REG_CERT_DECL,     IDX_CHAR_REG_CERT_VAL,
    DEVICE_INFO_IDX_NB
};

enum
{
    IDX_SVC_FFF0,
    IDX_CHAR_FFF1_DECL, IDX_CHAR_FFF1_VAL, IDX_CHAR_FFF1_CCCD,
    IDX_CHAR_FFF2_DECL, IDX_CHAR_FFF2_VAL,
    IDX_CHAR_CLI_OUT_DECL, IDX_CHAR_CLI_OUT_VAL, IDX_CHAR_CLI_OUT_CCCD,
    IDX_CHAR_CLI_IN_DECL, IDX_CHAR_CLI_IN_VAL,
    FFF0_IDX_NB
};

/* ---- UUIDs + static values (byte-identical to legacy) -------------------------- */

static const uint16_t SVC_UUID_DEVICE_INFO   = 0x180A;
static const uint16_t CHR_UUID_MANUFACTURER  = 0x2A29;
static const uint16_t CHR_UUID_MODEL_NUMBER  = 0x2A24;
static const uint16_t CHR_UUID_SERIAL_NUMBER = 0x2A25;
static const uint16_t CHR_UUID_HARDWARE_REV  = 0x2A27;
static const uint16_t CHR_UUID_FIRMWARE_REV  = 0x2A26;
static const uint16_t CHR_UUID_SOFTWARE_REV  = 0x2A28;
static const uint16_t CHR_UUID_SYSTEM_ID     = 0x2A23;
static const uint16_t CHR_UUID_REG_CERT_DATA = 0x2A2A;

static const uint8_t MANUFACTURER_NAME[] = "MEATPI.COM";
static const uint8_t MODEL_NUMBER[]      = "WiCAN-PRO";
static uint8_t s_serial_number[32]       = "";
static uint8_t HARDWARE_REV[]            = "1_53         ";
static uint8_t FIRMWARE_REV[]            = "400";
static uint8_t SOFTWARE_REV[]            = "0000";
static const uint8_t SYSTEM_ID[8]        = { 0 };
static const uint8_t REG_CERT_DATA[8]    = { 0 };

static const uint16_t SVC_UUID_FFF0 = 0xFFF0;
static const uint16_t CHR_UUID_FFF1 = 0xFFF1; /* notify+indicate: data OUT */
static const uint16_t CHR_UUID_FFF2 = 0xFFF2; /* write(+NR): data IN       */

/* CLI characteristics (128-bit, legacy byte order preserved) */
static const uint8_t CLI_OUT_CHAR_UUID[16] =
{
    0xBE, 0xF0, 0xAD, 0xDE, 0x34, 0x12, 0x78, 0x56,
    0x9A, 0xBC, 0xEF, 0x01, 0xC0, 0xDE, 0x00, 0x02
};
static const uint8_t CLI_IN_CHAR_UUID[16] =
{
    0xBE, 0xF0, 0xAD, 0xDE, 0x34, 0x12, 0x78, 0x56,
    0x9A, 0xBC, 0xEF, 0x01, 0xC0, 0xDE, 0x00, 0x03
};

/* advertised service uuid: FFF0 on the 128-bit base (legacy) */
static uint8_t s_adv_service_uuid[16] =
{
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0xF0, 0xFF, 0x00, 0x00,
};

static const uint16_t PRIMARY_SERVICE_UUID   = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t CHAR_DECLARATION_UUID  = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t CLIENT_CHAR_CFG_UUID   = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
static const uint8_t  CHAR_PROP_READ         = ESP_GATT_CHAR_PROP_BIT_READ;
static const uint8_t  CHAR_PROP_WRITE_WNR    = ESP_GATT_CHAR_PROP_BIT_WRITE |
                                               ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
static const uint8_t  CHAR_PROP_NOTIFY_IND   = ESP_GATT_CHAR_PROP_BIT_NOTIFY |
                                               ESP_GATT_CHAR_PROP_BIT_INDICATE;
static const uint8_t  CCC_DEFAULT[2]         = { 0x00, 0x00 };
static const uint8_t  CHAR_VALUE_DUMMY[1]    = { 0x00 };

static uint8_t s_manufacturer_adv[] = "MeatPi";

/* ---- adv / scan rsp / params (legacy values) ------------------------------------ */

static esp_ble_adv_data_t s_adv_config =
{
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = true,
    .min_interval = 0x0006,
    .max_interval = 0x0010,
    .appearance = 0x00,
    .service_uuid_len = sizeof(s_adv_service_uuid),
    .p_service_uuid = s_adv_service_uuid,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static esp_ble_adv_data_t s_scan_rsp_config =
{
    .set_scan_rsp = true,
    .include_name = true,
    .manufacturer_len = sizeof(s_manufacturer_adv),
    .p_manufacturer_data = s_manufacturer_adv,
};

static esp_ble_adv_params_t s_adv_params =
{
    .adv_int_min = 0x100,
    .adv_int_max = 0x100,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

/* ---- state -------------------------------------------------------------------------- */

static const char *s_dev_name;
static uint8_t s_adv_config_done;
static uint16_t s_dev_info_handles[DEVICE_INFO_IDX_NB] EXT_RAM_BSS_ATTR;
static uint16_t s_fff0_handles[FFF0_IDX_NB] EXT_RAM_BSS_ATTR;
static uint16_t s_conn_id = 0xffff;
static esp_gatt_if_t s_gatts_if = 0xff;
static uint16_t s_mtu = 23;
static uint16_t s_max_data = 20;      /* min(BLM_SEND_BUF_SIZE, MTU-3)      */
static volatile bool s_connected;
static volatile bool s_secured;
static volatile bool s_congested;
static esp_bd_addr_t s_last_remote_bda;

/* CLI IN reassembly (long writes + line splitting), PSRAM per §2 */
static char s_cli_buf[BLM_CLI_MAX] EXT_RAM_BSS_ATTR;
static size_t s_cli_len;

/* ---- attribute tables (permissions byte-identical to legacy) ---------------------- */

static const esp_gatts_attr_db_t DEV_INFO_DB[DEVICE_INFO_IDX_NB] =
{
    [IDX_SVC_DEVICE_INFO] = {{ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&PRIMARY_SERVICE_UUID, ESP_GATT_PERM_READ,
         sizeof(uint16_t), sizeof(SVC_UUID_DEVICE_INFO),
         (uint8_t *)&SVC_UUID_DEVICE_INFO}},
#define RO_CHAR(decl, val, uuid, data) \
    [decl] = {{ESP_GATT_AUTO_RSP}, \
        {ESP_UUID_LEN_16, (uint8_t *)&CHAR_DECLARATION_UUID, ESP_GATT_PERM_READ, \
         sizeof(uint8_t), sizeof(CHAR_PROP_READ), (uint8_t *)&CHAR_PROP_READ}}, \
    [val] = {{ESP_GATT_AUTO_RSP}, \
        {ESP_UUID_LEN_16, (uint8_t *)&(uuid), ESP_GATT_PERM_READ, \
         sizeof(data), sizeof(data), (uint8_t *)(data)}}
    RO_CHAR(IDX_CHAR_MANUFACTURER_DECL, IDX_CHAR_MANUFACTURER_VAL,
            CHR_UUID_MANUFACTURER, MANUFACTURER_NAME),
    RO_CHAR(IDX_CHAR_MODEL_DECL, IDX_CHAR_MODEL_VAL,
            CHR_UUID_MODEL_NUMBER, MODEL_NUMBER),
    RO_CHAR(IDX_CHAR_SERIAL_DECL, IDX_CHAR_SERIAL_VAL,
            CHR_UUID_SERIAL_NUMBER, s_serial_number),
    RO_CHAR(IDX_CHAR_HW_REV_DECL, IDX_CHAR_HW_REV_VAL,
            CHR_UUID_HARDWARE_REV, HARDWARE_REV),
    RO_CHAR(IDX_CHAR_FW_REV_DECL, IDX_CHAR_FW_REV_VAL,
            CHR_UUID_FIRMWARE_REV, FIRMWARE_REV),
    RO_CHAR(IDX_CHAR_SW_REV_DECL, IDX_CHAR_SW_REV_VAL,
            CHR_UUID_SOFTWARE_REV, SOFTWARE_REV),
    RO_CHAR(IDX_CHAR_SYSTEM_ID_DECL, IDX_CHAR_SYSTEM_ID_VAL,
            CHR_UUID_SYSTEM_ID, SYSTEM_ID),
    RO_CHAR(IDX_CHAR_REG_CERT_DECL, IDX_CHAR_REG_CERT_VAL,
            CHR_UUID_REG_CERT_DATA, REG_CERT_DATA),
#undef RO_CHAR
};

static const esp_gatts_attr_db_t FFF0_DB[FFF0_IDX_NB] =
{
    [IDX_SVC_FFF0] = {{ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&PRIMARY_SERVICE_UUID, ESP_GATT_PERM_READ,
         sizeof(uint16_t), sizeof(uint16_t), (uint8_t *)&SVC_UUID_FFF0}},
    [IDX_CHAR_FFF1_DECL] = {{ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&CHAR_DECLARATION_UUID, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(CHAR_PROP_NOTIFY_IND),
         (uint8_t *)&CHAR_PROP_NOTIFY_IND}},
    [IDX_CHAR_FFF1_VAL] = {{ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&CHR_UUID_FFF1,
         ESP_GATT_PERM_READ_ENC_MITM,
         BLM_SEND_BUF_SIZE, sizeof(CHAR_VALUE_DUMMY),
         (uint8_t *)CHAR_VALUE_DUMMY}},
    [IDX_CHAR_FFF1_CCCD] = {{ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&CLIENT_CHAR_CFG_UUID,
         ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE_ENC_MITM,
         sizeof(uint16_t), sizeof(CCC_DEFAULT), (uint8_t *)CCC_DEFAULT}},
    [IDX_CHAR_FFF2_DECL] = {{ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&CHAR_DECLARATION_UUID, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(uint8_t), (uint8_t *)&CHAR_PROP_WRITE_WNR}},
    [IDX_CHAR_FFF2_VAL] = {{ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&CHR_UUID_FFF2,
         ESP_GATT_PERM_WRITE_ENC_MITM,
         BLM_SEND_BUF_SIZE, 0, NULL}},
    [IDX_CHAR_CLI_OUT_DECL] = {{ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&CHAR_DECLARATION_UUID, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(CHAR_PROP_NOTIFY_IND),
         (uint8_t *)&CHAR_PROP_NOTIFY_IND}},
    [IDX_CHAR_CLI_OUT_VAL] = {{ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_128, (uint8_t *)CLI_OUT_CHAR_UUID,
         ESP_GATT_PERM_READ_ENC_MITM,
         BLM_SEND_BUF_SIZE, sizeof(CHAR_VALUE_DUMMY),
         (uint8_t *)CHAR_VALUE_DUMMY}},
    [IDX_CHAR_CLI_OUT_CCCD] = {{ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&CLIENT_CHAR_CFG_UUID,
         ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE_ENC_MITM,
         sizeof(uint16_t), sizeof(CCC_DEFAULT), (uint8_t *)CCC_DEFAULT}},
    [IDX_CHAR_CLI_IN_DECL] = {{ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&CHAR_DECLARATION_UUID, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(uint8_t), (uint8_t *)&CHAR_PROP_WRITE_WNR}},
    [IDX_CHAR_CLI_IN_VAL] = {{ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_128, (uint8_t *)CLI_IN_CHAR_UUID,
         ESP_GATT_PERM_WRITE_ENC_MITM,
         BLM_CLI_MAX, 0, NULL}},
};

/* ---- CLI line reassembly (legacy semantics) ---------------------------------------- */

static void cli_process_lines(void)
{
    size_t line_start = 0;

    for (size_t i = 0; i < s_cli_len; i++)
    {
        char c = s_cli_buf[i];

        if (c == '\r' || c == '\n')
        {
            s_cli_buf[i] = '\0';

            if (i > line_start)
            {
                blm_core_on_cli_line(&s_cli_buf[line_start]);
            }

            line_start = i + 1;
        }
    }

    if (line_start > 0)
    {
        size_t remaining = s_cli_len - line_start;

        memmove(s_cli_buf, &s_cli_buf[line_start], remaining);
        s_cli_len = remaining;
        s_cli_buf[s_cli_len] = '\0';
    }
}

static void on_cli_write(esp_ble_gatts_cb_param_t *param)
{
    if (!s_secured)
    {
        ESP_LOGW(TAG, "CLI write rejected: link not securely paired yet");
        return;
    }

    if (param->write.is_prep)
    {
        /* long write: accumulate at the offset, executed later */
        size_t end = (size_t)param->write.offset + param->write.len;

        if (end <= BLM_CLI_MAX - 1)
        {
            memcpy(&s_cli_buf[param->write.offset], param->write.value,
                   param->write.len);

            if (end > s_cli_len)
            {
                s_cli_len = end;
            }

            s_cli_buf[s_cli_len] = '\0';
        }
        else
        {
            ESP_LOGE(TAG, "prepared CLI write overflow");
        }

        return;
    }

    size_t to_copy = param->write.len;

    if (to_copy > BLM_CLI_MAX - 1 - s_cli_len)
    {
        to_copy = BLM_CLI_MAX - 1 - s_cli_len;
    }

    memcpy(&s_cli_buf[s_cli_len], param->write.value, to_copy);
    s_cli_len += to_copy;
    s_cli_buf[s_cli_len] = '\0';
    cli_process_lines();
}

static void on_cli_exec_write(esp_ble_gatts_cb_param_t *param)
{
    if (param->exec_write.exec_write_flag == ESP_GATT_PREP_WRITE_EXEC)
    {
        cli_process_lines();

        /* legacy: a prepared buffer without a newline is one command */
        if (s_cli_len > 0)
        {
            s_cli_buf[s_cli_len] = '\0';
            blm_core_on_cli_line(s_cli_buf);
        }
    }

    s_cli_len = 0;
    s_cli_buf[0] = '\0';
}

/* ---- GAP ------------------------------------------------------------------------------ */

static void gap_event_handler(esp_gap_ble_cb_event_t event,
                              esp_ble_gap_cb_param_t *param)
{
    switch (event)
    {
        case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
            s_adv_config_done &= ~SCAN_RSP_CONFIG_FLAG;

            if (s_adv_config_done == 0)
            {
                esp_ble_gap_start_advertising(&s_adv_params);
            }
            break;

        case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
            s_adv_config_done &= ~ADV_CONFIG_FLAG;

            if (s_adv_config_done == 0)
            {
                esp_ble_gap_start_advertising(&s_adv_params);
            }
            break;

        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS)
            {
                ESP_LOGE(TAG, "advertising start failed (0x%x)",
                         param->adv_start_cmpl.status);
            }
            else
            {
                ESP_LOGI(TAG, "advertising");
            }
            break;

        case ESP_GAP_BLE_NC_REQ_EVT:
            /* DisplayYesNo on both sides: confirm (legacy behavior) */
            esp_ble_confirm_reply(param->ble_security.ble_req.bd_addr, true);
            break;

        case ESP_GAP_BLE_SEC_REQ_EVT:
            /* the runtime pairing window (legacy toggle) */
            esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr,
                                     blm_core_pairing_allowed());
            break;

        case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
            /* IO_CAP_OUT: the static passkey is "displayed" (never logged —
               standard §10: no secrets in logs) */
            break;

        case ESP_GAP_BLE_AUTH_CMPL_EVT:
            s_secured = param->ble_security.auth_cmpl.success;
            ESP_LOGI(TAG, "pairing %s", s_secured ? "success" : "failed");
            break;

        case ESP_GAP_BLE_SET_LOCAL_PRIVACY_COMPLETE_EVT:
            if (param->local_privacy_cmpl.status != ESP_BT_STATUS_SUCCESS)
            {
                ESP_LOGE(TAG, "local privacy config failed");
                break;
            }

            if (esp_ble_gap_config_adv_data(&s_adv_config) == ESP_OK)
            {
                s_adv_config_done |= ADV_CONFIG_FLAG;
            }

            if (esp_ble_gap_config_adv_data(&s_scan_rsp_config) == ESP_OK)
            {
                s_adv_config_done |= SCAN_RSP_CONFIG_FLAG;
            }
            break;

        default:
            break;
    }
}

/* ---- GATTS ----------------------------------------------------------------------------- */

static void gatts_event_handler(esp_gatts_cb_event_t event,
                                esp_gatt_if_t gatts_if,
                                esp_ble_gatts_cb_param_t *param)
{
    switch (event)
    {
        case ESP_GATTS_REG_EVT:
            if (param->reg.status != ESP_GATT_OK)
            {
                ESP_LOGE(TAG, "app register failed (%d)", param->reg.status);
                return;
            }

            s_gatts_if = gatts_if;
            esp_ble_gap_set_device_name(s_dev_name);
            esp_ble_gap_config_local_icon(ESP_BLE_APPEARANCE_GENERIC_COMPUTER);
            esp_ble_gap_config_local_privacy(true); /* resolvable random */
            esp_ble_gatts_create_attr_tab(DEV_INFO_DB, gatts_if,
                                          DEVICE_INFO_IDX_NB,
                                          DEV_INFO_SVC_INST_ID);
            break;

        case ESP_GATTS_CREAT_ATTR_TAB_EVT:
            if (param->add_attr_tab.status != ESP_GATT_OK)
            {
                ESP_LOGE(TAG, "attr table failed (0x%x)",
                         param->add_attr_tab.status);
                break;
            }

            if (param->add_attr_tab.svc_inst_id == DEV_INFO_SVC_INST_ID &&
                param->add_attr_tab.num_handle == DEVICE_INFO_IDX_NB)
            {
                memcpy(s_dev_info_handles, param->add_attr_tab.handles,
                       sizeof(s_dev_info_handles));
                esp_ble_gatts_start_service(
                    s_dev_info_handles[IDX_SVC_DEVICE_INFO]);
                esp_ble_gatts_create_attr_tab(FFF0_DB, gatts_if, FFF0_IDX_NB,
                                              FFF0_SVC_INST_ID);
            }
            else if (param->add_attr_tab.svc_inst_id == FFF0_SVC_INST_ID &&
                     param->add_attr_tab.num_handle == FFF0_IDX_NB)
            {
                memcpy(s_fff0_handles, param->add_attr_tab.handles,
                       sizeof(s_fff0_handles));
                esp_ble_gatts_start_service(s_fff0_handles[IDX_SVC_FFF0]);
            }
            break;

        case ESP_GATTS_WRITE_EVT:
            if (param->write.handle == s_fff0_handles[IDX_CHAR_CLI_IN_VAL])
            {
                on_cli_write(param);
            }
            else if (param->write.handle == s_fff0_handles[IDX_CHAR_FFF2_VAL])
            {
                /* the data pipe: DEBUG only — this is the hot path (§10) */
                ESP_LOGD(TAG, "rx %d bytes", param->write.len);
                blm_core_on_rx(param->write.value, param->write.len);
            }
            break;

        case ESP_GATTS_EXEC_WRITE_EVT:
            on_cli_exec_write(param);
            break;

        case ESP_GATTS_MTU_EVT:
            s_mtu = param->mtu.mtu;
            s_max_data = (BLM_SEND_BUF_SIZE <= s_mtu - 3)
                             ? BLM_SEND_BUF_SIZE : (uint16_t)(s_mtu - 3);
            ESP_LOGI(TAG, "MTU %u (payload %u)", s_mtu, s_max_data);
            break;

        case ESP_GATTS_CONNECT_EVT:
        {
            /* the configured connection window (settings conn_profile:
               "ios" = legacy 20–40 ms, "android_fast" = 7.5–15 ms) */
            esp_ble_conn_update_params_t conn_params = { 0 };

            memcpy(conn_params.bda, param->connect.remote_bda,
                   sizeof(esp_bd_addr_t));
            conn_params.latency = 0;
            conn_params.min_int = blm_core_config()->conn_min_units;
            conn_params.max_int = blm_core_config()->conn_max_units;
            conn_params.timeout = 400; /* 4 s (legacy) */
            esp_ble_gap_update_conn_params(&conn_params);

            s_conn_id = param->connect.conn_id;
            s_connected = true;
            s_secured = false; /* until AUTH_CMPL */
            s_cli_len = 0;
            memcpy(s_last_remote_bda, param->connect.remote_bda,
                   sizeof(esp_bd_addr_t));

            if (blm_core_pairing_allowed())
            {
                esp_ble_set_encryption(param->connect.remote_bda,
                                       ESP_BLE_SEC_ENCRYPT_MITM);
            }

            blm_core_on_connect();
            break;
        }

        case ESP_GATTS_DISCONNECT_EVT:
            s_connected = false;
            s_secured = false;
            s_cli_len = 0;
            blm_core_on_disconnect();
            esp_ble_gap_start_advertising(&s_adv_params); /* re-advertise */
            break;

        case ESP_GATTS_CONGEST_EVT:
            s_congested = param->congest.congested;
            break;

        default:
            break;
    }
}

/* ---- stack lifecycle ---------------------------------------------------------------------- */

esp_err_t blm_gatt_stack_up(const char *dev_name)
{
    const blm_config_t *cfg = blm_core_config();

    s_dev_name = dev_name;
    blm_ident_serial(dev_name, (char *)s_serial_number,
                     sizeof(s_serial_number));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

    bt_cfg.controller_task_stack_size = 1024 * 8; /* legacy sizing */

    esp_err_t err = esp_bt_controller_init(&bt_cfg);

    if (err == ESP_OK)
    {
        err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    }

    if (err == ESP_OK)
    {
        err = esp_bluedroid_init();
    }

    if (err == ESP_OK)
    {
        err = esp_bluedroid_enable();
    }

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "BT stack bring-up failed: %s", esp_err_to_name(err));
        return err;
    }

    /* TX power (settings, clamped to the supported 3 dB steps) */
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

    err = esp_ble_gatts_register_callback(gatts_event_handler);

    if (err == ESP_OK)
    {
        err = esp_ble_gap_register_callback(gap_event_handler);
    }

    if (err == ESP_OK)
    {
        err = esp_ble_gatts_app_register(BLM_APP_ID);
    }

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "callback registration failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    esp_ble_gatt_set_local_mtu(517); /* legacy */

    /* security block — byte-identical to legacy */
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_MITM_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_OUT;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t auth_option = ESP_BLE_ONLY_ACCEPT_SPECIFIED_AUTH_ENABLE;
    uint8_t oob_support = ESP_BLE_OOB_DISABLE;
    uint32_t passkey = cfg->passkey;

    esp_ble_gap_set_security_param(ESP_BLE_SM_CLEAR_STATIC_PASSKEY, &passkey,
                                   sizeof(uint32_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_STATIC_PASSKEY, &passkey,
                                   sizeof(uint32_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req,
                                   sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap,
                                   sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size,
                                   sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_ONLY_ACCEPT_SPECIFIED_SEC_AUTH,
                                   &auth_option, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_OOB_SUPPORT, &oob_support,
                                   sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key,
                                   sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key,
                                   sizeof(uint8_t));
    return ESP_OK;
}

void blm_gatt_stack_down(void)
{
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    s_connected = false;
    s_secured = false;
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

uint16_t blm_gatt_max_data(void)
{
    return s_max_data;
}

bool blm_gatt_congested(void)
{
    return s_congested;
}

int blm_gatt_free_packets(void)
{
    return s_connected ? esp_ble_get_cur_sendable_packets_num(s_conn_id) : 0;
}

esp_err_t blm_gatt_notify_data(const uint8_t *buf, uint16_t len)
{
    return esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id,
                                       s_fff0_handles[IDX_CHAR_FFF1_VAL],
                                       len, (uint8_t *)buf, false);
}

esp_err_t blm_gatt_notify_cli(const uint8_t *buf, uint16_t len)
{
    return esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id,
                                       s_fff0_handles[IDX_CHAR_CLI_OUT_VAL],
                                       len, (uint8_t *)buf, false);
}

void blm_gatt_allow_pairing(bool allow)
{
    /* legacy: enabling pairing mid-connection kicks off encryption */
    if (allow && s_connected && !s_secured)
    {
        esp_ble_set_encryption(s_last_remote_bda, ESP_BLE_SEC_ENCRYPT_MITM);
    }
}
