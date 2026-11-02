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
 * @file ble_manager_gatt_svc.c
 * @brief The NimBLE GATT service TABLE and access callbacks: Device
 *        Information (0x180A), the FFF0 service with the legacy four
 *        characteristics (FFF1/FFF2 data, CLI OUT/IN) and, APPENDED after
 *        them, one OUT/IN pair per registered stream channel. Appending
 *        keeps the legacy attribute handles stable, so an app that cached
 *        the GATT database from an older firmware keeps working (it just
 *        does not see the new characteristics until it re-discovers).
 *        GAP, security, advertising and the notify path live in
 *        ble_manager_gatt_nimble.c.
 */
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"

#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"

#include "ble_manager.h"
#include "ble_manager_private.h"

static const char *TAG = "ble_manager";

/* ---- identity strings (byte-identical to the Bluedroid backend) --------------- */

static const char MANUFACTURER_NAME[] = "MEATPI.COM";
static const char MODEL_NUMBER[]      = "WiCAN-PRO";
static char s_serial_number[32]       = "";
static const char HARDWARE_REV[]      = "1_53         ";
static const char FIRMWARE_REV[]      = "400";
static const char SOFTWARE_REV[]      = "0000";
static const uint8_t SYSTEM_ID[8]     = { 0 };
static const uint8_t REG_CERT_DATA[8] = { 0 };

/* 128-bit CLI UUIDs - same LSB-first byte arrays as legacy */
static const ble_uuid128_t CLI_OUT_UUID = BLE_UUID128_INIT(
    0xBE, 0xF0, 0xAD, 0xDE, 0x34, 0x12, 0x78, 0x56,
    0x9A, 0xBC, 0xEF, 0x01, 0xC0, 0xDE, 0x00, 0x02);
static const ble_uuid128_t CLI_IN_UUID = BLE_UUID128_INIT(
    0xBE, 0xF0, 0xAD, 0xDE, 0x34, 0x12, 0x78, 0x56,
    0x9A, 0xBC, 0xEF, 0x01, 0xC0, 0xDE, 0x00, 0x03);

/* ---- handles -------------------------------------------------------------------- */

static uint16_t s_fff1_val_handle;
static uint16_t s_fff2_val_handle;
static uint16_t s_cli_out_val_handle;
static uint16_t s_cli_in_val_handle;
static uint16_t s_ch_handle[BLE_MANAGER_CHANNEL_MAX][2]; /* [i][0]=out [1]=in */

/* CLI IN reassembly (line splitting), PSRAM per §2 */
static char s_cli_buf[BLM_CLI_MAX] EXT_RAM_BSS_ATTR;
static size_t s_cli_len;

/* flat copy of one data/channel write (hot path), PSRAM */
static uint8_t s_rx_buf[BLM_SEND_BUF_SIZE] EXT_RAM_BSS_ATTR;

/* ---- CLI line reassembly (same semantics as the Bluedroid backend) ------------- */

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

void blm_svc_reset_rx(void)
{
    s_cli_len = 0;
}

/* ---- GATT access callbacks -------------------------------------------------------- */

/** Device Info: every characteristic is a static read-only string. */
static int dev_info_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const void *data = NULL;
    size_t len = 0;
    uint16_t uuid16 = ble_uuid_u16(ctxt->chr->uuid);

    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    /* lengths = sizeof() incl. the NUL, serial padded to 32 - the
       Bluedroid attribute table served exactly these bytes (legacy
       apps see identical values on either stack) */
    switch (uuid16)
    {
        case 0x2A29: data = MANUFACTURER_NAME; len = sizeof(MANUFACTURER_NAME); break;
        case 0x2A24: data = MODEL_NUMBER; len = sizeof(MODEL_NUMBER); break;
        case 0x2A25: data = s_serial_number; len = sizeof(s_serial_number); break;
        case 0x2A27: data = HARDWARE_REV; len = sizeof(HARDWARE_REV); break;
        case 0x2A26: data = FIRMWARE_REV; len = sizeof(FIRMWARE_REV); break;
        case 0x2A28: data = SOFTWARE_REV; len = sizeof(SOFTWARE_REV); break;
        case 0x2A23: data = SYSTEM_ID; len = sizeof(SYSTEM_ID); break;
        case 0x2A2A: data = REG_CERT_DATA; len = sizeof(REG_CERT_DATA); break;
        default: return BLE_ATT_ERR_UNLIKELY;
    }

    return (os_mbuf_append(ctxt->om, data, len) == 0)
               ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/** FFF0: every OUT characteristic is notify-only (reads answer a dummy
 *  byte like the Bluedroid table); FFF2, CLI IN and the channel IN
 *  characteristics are the write pipes. */
static int fff0_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR)
    {
        static const uint8_t dummy = 0x00;

        return (os_mbuf_append(ctxt->om, &dummy, 1) == 0)
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
    {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);

    if (attr_handle == s_fff2_val_handle)
    {
        /* the data pipe - the GATT flags (WRITE_ENC|WRITE_AUTHEN) already
           bar unpaired writes; this mirrors the CLI path's software check
           so the vehicle-command pipe is gated symmetrically */
        if (!blm_gatt_secured())
        {
            ESP_LOGW(TAG, "data write rejected: link not securely paired");
            return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
        }

        uint16_t out_len = 0;

        if (ble_hs_mbuf_to_flat(ctxt->om, s_rx_buf, sizeof(s_rx_buf),
                                &out_len) != 0)
        {
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }

        blm_core_on_rx(s_rx_buf, out_len);
        return 0;
    }

    if (attr_handle == s_cli_in_val_handle)
    {
        if (!blm_gatt_secured())
        {
            ESP_LOGW(TAG, "CLI write rejected: link not securely paired");
            return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
        }

        size_t room = BLM_CLI_MAX - 1 - s_cli_len;
        uint16_t to_copy = (len > room) ? (uint16_t)room : len;
        uint16_t copied = 0;

        ble_hs_mbuf_to_flat(ctxt->om, (uint8_t *)&s_cli_buf[s_cli_len],
                            to_copy, &copied);
        s_cli_len += copied;
        s_cli_buf[s_cli_len] = '\0';
        cli_process_lines();
        return 0;
    }

    int ch = blm_channel_find_in_handle(attr_handle);

    if (ch >= 0)
    {
        if (!blm_gatt_secured())
        {
            ESP_LOGW(TAG, "channel write rejected: link not securely paired");
            return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
        }

        uint16_t out_len = 0;

        if (ble_hs_mbuf_to_flat(ctxt->om, s_rx_buf, sizeof(s_rx_buf),
                                &out_len) != 0)
        {
            return BLE_ATT_ERR_INSUFFICIENT_RES; /* > 490 B in one write */
        }

        /* a full RX buffer is visible to write-WITH-response clients;
           write-without-response drops are counted (rx_overflow) and the
           owner's protocol detects the gap */
        return blm_channel_on_rx(ch, s_rx_buf, out_len)
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

/* ---- service tables ----------------------------------------------------------------- */

/* Device Info reads require an encrypted + authenticated (paired/bonded)
   link too (meatpi 2026-07-08): NO characteristic is readable by an
   unpaired peer - an unpaired read gets Insufficient Authentication,
   which prompts the client to pair. */
#define DI_CHR(uuid16) \
    { .uuid = BLE_UUID16_DECLARE(uuid16), .access_cb = dev_info_access, \
      .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | \
               BLE_GATT_CHR_F_READ_AUTHEN }

/* The subscribe (the CCCD write) needs the paired link too, as the 4.x table
   had it (ESP_GATT_PERM_WRITE_ENC_MITM on the CCCD): NimBLE gates a CCCD
   only through the NOTIFY_INDICATE_* flags of its characteristic. Without
   them a plain link subscribed FFF1 silently and its write-without-response
   to FFF2 was dropped silently, so an app that only subscribes and writes
   never paired (2026-11-02, the Discord report). With them the subscribe is
   answered Insufficient Authentication and the phone pairs right there. */
#define OUT_FLAGS (BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | \
                   BLE_GATT_CHR_F_READ_AUTHEN | \
                   BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_INDICATE | \
                   BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC | \
                   BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN)
#define IN_FLAGS  (BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | \
                   BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN)
/* stream channel OUT: the properties follow the owner's out_modes. An
   INDICATE-only channel confirms every PDU end to end (the 2026-09-21
   finding: notifications were lost inside the device under sustained TX,
   btmon 144 of 146 on air while ble_gatts_notify_custom() returned 0 for
   all - root cause 2026-09-22: the controller's heap-allocated ACL TX
   buffers, CONFIG_BT_CTRL_BLE_STATIC_ACL_TX_BUF_NB=12 fixed it). A channel that
   also allows NOTIFY lets the app choose speed with its CCCD, and its
   protocol carries the credits + frame counter that make a loss
   detectable (ble_http v2). */
#define CH_OUT_BASE  (BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | \
                      BLE_GATT_CHR_F_READ_AUTHEN | \
                      BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC | \
                      BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN)

static ble_gatt_chr_flags ch_out_flags(int idx)
{
    uint8_t m = blm_channel_out_modes(idx);
    ble_gatt_chr_flags f = CH_OUT_BASE;

    if (m & BLE_MANAGER_CH_OUT_INDICATE) f |= BLE_GATT_CHR_F_INDICATE;
    if (m & BLE_MANAGER_CH_OUT_NOTIFY)   f |= BLE_GATT_CHR_F_NOTIFY;
    return f;
}

static const struct ble_gatt_chr_def DI_CHRS[] =
{
    DI_CHR(0x2A29), DI_CHR(0x2A24), DI_CHR(0x2A25), DI_CHR(0x2A27),
    DI_CHR(0x2A26), DI_CHR(0x2A28), DI_CHR(0x2A23), DI_CHR(0x2A2A),
    { 0 }
};

/* the legacy four, then 2 per channel, then the terminator; NimBLE keeps
   referencing these arrays, so they are static (PSRAM per §2) */
#define FFF0_FIXED 4
static struct ble_gatt_chr_def
    s_fff0_chrs[FFF0_FIXED + 2 * BLE_MANAGER_CHANNEL_MAX + 1] EXT_RAM_BSS_ATTR;
static ble_uuid16_t s_ch_uuid[BLE_MANAGER_CHANNEL_MAX][2] EXT_RAM_BSS_ATTR;
static struct ble_gatt_svc_def s_gatt_svcs[3] EXT_RAM_BSS_ATTR;

/* UUID objects with STATIC storage: BLE_UUID16_DECLARE() points at a
   compound literal, which inside a function is a stack temporary - NimBLE
   would then read a dangling pointer during discovery (`ble_uuid_flat
   rc=3`, 0 characteristics on the phone; bench-caught 2026-09-21). */
static const ble_uuid16_t UUID_DIS  = BLE_UUID16_INIT(0x180A);
static const ble_uuid16_t UUID_FFF0 = BLE_UUID16_INIT(0xFFF0);
static const ble_uuid16_t UUID_FFF1 = BLE_UUID16_INIT(0xFFF1);
static const ble_uuid16_t UUID_FFF2 = BLE_UUID16_INIT(0xFFF2);

static void build_tables(void)
{
    memset(s_fff0_chrs, 0, sizeof(s_fff0_chrs));

    s_fff0_chrs[0] = (struct ble_gatt_chr_def)
    {   /* FFF1: data OUT (notify/indicate; enc+authen reads) */
        .uuid = &UUID_FFF1.u, .access_cb = fff0_access,
        .val_handle = &s_fff1_val_handle, .flags = OUT_FLAGS,
    };
    s_fff0_chrs[1] = (struct ble_gatt_chr_def)
    {   /* FFF2: data IN (write/write-nr; enc+authen) */
        .uuid = &UUID_FFF2.u, .access_cb = fff0_access,
        .val_handle = &s_fff2_val_handle, .flags = IN_FLAGS,
    };
    s_fff0_chrs[2] = (struct ble_gatt_chr_def)
    {   /* CLI OUT (128-bit) */
        .uuid = &CLI_OUT_UUID.u, .access_cb = fff0_access,
        .val_handle = &s_cli_out_val_handle, .flags = OUT_FLAGS,
    };
    s_fff0_chrs[3] = (struct ble_gatt_chr_def)
    {   /* CLI IN (128-bit) */
        .uuid = &CLI_IN_UUID.u, .access_cb = fff0_access,
        .val_handle = &s_cli_in_val_handle, .flags = IN_FLAGS,
    };

    int n = blm_channel_count();

    for (int i = 0; i < n; i++)
    {
        const ble_manager_channel_desc_t *d = blm_channel_desc(i);

        s_ch_uuid[i][0].u.type = BLE_UUID_TYPE_16;
        s_ch_uuid[i][0].value = d->uuid_out;
        s_ch_uuid[i][1].u.type = BLE_UUID_TYPE_16;
        s_ch_uuid[i][1].value = d->uuid_in;

        s_fff0_chrs[FFF0_FIXED + 2 * i] = (struct ble_gatt_chr_def)
        {
            .uuid = &s_ch_uuid[i][0].u, .access_cb = fff0_access,
            .val_handle = &s_ch_handle[i][0], .flags = ch_out_flags(i),
        };
        s_fff0_chrs[FFF0_FIXED + 2 * i + 1] = (struct ble_gatt_chr_def)
        {
            .uuid = &s_ch_uuid[i][1].u, .access_cb = fff0_access,
            .val_handle = &s_ch_handle[i][1], .flags = IN_FLAGS,
        };
    }

    memset(s_gatt_svcs, 0, sizeof(s_gatt_svcs));
    s_gatt_svcs[0] = (struct ble_gatt_svc_def)
    {   /* Device Information (0x180A) - same eight characteristics */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_DIS.u,
        .characteristics = DI_CHRS,
    };
    s_gatt_svcs[1] = (struct ble_gatt_svc_def)
    {   /* FFF0 - the data pipes + the CLI pipes + stream channels */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_FFF0.u,
        .characteristics = s_fff0_chrs,
    };
}

esp_err_t blm_svc_register(const char *dev_name)
{
    blm_ident_serial(dev_name, s_serial_number, sizeof(s_serial_number));
    build_tables();

    int rc = ble_gatts_count_cfg(s_gatt_svcs);

    if (rc == 0)
    {
        rc = ble_gatts_add_svcs(s_gatt_svcs);
    }

    if (rc == 0)
    {
        rc = ble_svc_gap_device_name_set(dev_name);
    }

    if (rc != 0)
    {
        ESP_LOGE(TAG, "GATT registration failed (%d)", rc);
        return ESP_FAIL;
    }

    return ESP_OK;
}

void blm_svc_publish_handles(void)
{
    /* the val_handle out-params are assigned by ble_gatts_start(), which
       runs before the host's sync callback - publish the channel IN
       handles there so the write callback can route by handle */
    int n = blm_channel_count();

    for (int i = 0; i < n; i++)
    {
        blm_channel_set_in_handle(i, s_ch_handle[i][1]);
        blm_channel_set_out_handle(i, s_ch_handle[i][0]);
    }
}

uint16_t blm_svc_data_out_handle(void)
{
    return s_fff1_val_handle;
}

uint16_t blm_svc_cli_out_handle(void)
{
    return s_cli_out_val_handle;
}

uint16_t blm_svc_channel_out_handle(int idx)
{
    if (idx < 0 || idx >= blm_channel_count())
    {
        return 0;
    }

    return s_ch_handle[idx][0];
}
