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
 * @file ble_manager_private.h
 * @brief Internal API between ble_manager translation units, incl. the pure
 *        layer (host-testable: no BT stack deps).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "ble_manager.h" /* channel descriptor / event types */

#ifdef __cplusplus
extern "C" {
#endif

#define BLM_SEND_BUF_SIZE  490  /* legacy BLE_SEND_BUF_SIZE               */
#define BLM_CLI_MAX        1024 /* legacy BLE_CMDLINE_MAX                 */
#define BLM_NAME_MAX       32
#define BLM_TX_QUEUE_DEPTH 32

/* ---- pure helpers (ble_manager_pack.c: host-tested) ------------------------ */

/** Packets needed to flush `pending` buffered bytes plus `add` new ones at
 *  `max_data` bytes per packet (legacy round-up math). */
int blm_pack_packets_needed(size_t pending, size_t add, size_t max_data);

/**
 * Legacy fill step: append from in[consumed..] into buf (current fill
 * *buf_len, capacity max_data). Advances *consumed and *buf_len.
 * Returns true when buf is full and must be flushed.
 */
bool blm_pack_fill(uint8_t *buf, size_t *buf_len, size_t max_data,
                   const uint8_t *in, size_t in_len, size_t *consumed);

/** Device name from the device id: "WiC_<id>" (legacy ble_uid). */
void blm_ident_name(const char *device_id, char *name, size_t name_len);

/** Serial number from the device name: legacy rule verbatim:
 *  serial = dev_name + 7 (the on-air value existing tools see). */
void blm_ident_serial(const char *dev_name, char *serial, size_t serial_len);

/** Map a dBm setting to the nearest supported ESP32-S3 level (-12..+9 in
 *  3 dB steps, legacy switch). Returns the clamped dBm actually used. */
int blm_ident_clamp_tx_power(int dbm);

/** Map the conn_profile setting to the connection window the peripheral
 *  REQUESTS on connect (units of 1.25 ms): "ios" (default, legacy
 *  20–40 ms) or "android_fast" (7.5–15 ms, max performance). The central
 *  decides whether to honor the request. */
void blm_ident_conn_window(const char *profile, uint16_t *min_units,
                           uint16_t *max_units);

/* BLE 5 as settings (2026-09-21): the PHY the device PREFERS and the
   advertising set(s) it runs. Masks/modes are stack-agnostic. */
#define BLM_PHY_1M    0x01u
#define BLM_PHY_2M    0x02u
#define BLM_PHY_CODED 0x04u

#define BLM_ADV_LEGACY   0u   /* the 4.2-visible set only (today's bytes)   */
#define BLM_ADV_EXTENDED 1u   /* the 5.0 extended set only                  */
#define BLM_ADV_BOTH     2u   /* both sets at once                          */

/** `phy` setting -> preferred PHY mask ("1m"/unknown = 1M, "2m" = 2M,
 *  "coded" = coded, "auto" = 1M|2M). */
uint8_t blm_ident_phy_mask(const char *phy);

/** `advertising` setting -> BLM_ADV_* ("legacy"/unknown = legacy). */
uint8_t blm_ident_adv_mode(const char *mode);

/* ---- Bluetooth memory profile (v5, 2026-10-31) --------------------------------
   The ESP32-S3 controller's sizing is RUNTIME (esp_bt_controller_config_t,
   read once at esp_bt_controller_init), so a setting trades internal RAM
   for Bluetooth features at the next boot, the way wifi_ram_profile does
   for the WiFi driver. The mapping below is pure (host-tested); the two
   GATT backends apply it through blm_ctrl_configure() (ble_manager_ctrl.h). */

typedef enum
{
    BLM_RAM_FULL = 0,   /* the build's Kconfig values: today's behaviour     */
    BLM_RAM_LEAN,       /* one phone, legacy advertising on 1M, no scanning  */
    BLM_RAM_CUSTOM,     /* the three knobs                                   */
} blm_ram_profile_t;

typedef struct
{
    uint8_t  max_act;      /* controller activities = connections + advertising
                              instances, 828 B each; 2..10                    */
    uint8_t  acl_tx_bufs;  /* static ACL TX buffers, about 270 B each; 0..12  */
    uint16_t scan_cache;   /* scan duplicate cache entries, 10..1000; 0 = the
                              scan feature and its caches off                 */
    bool     ble5;         /* the controller's BLE 5 feature set: ALWAYS the
                              build's (the NimBLE host is compiled with it and
                              esp_bt.h ties the two; a controller without it
                              crashed the device at the stack bring-up, three
                              times, into the park brake, 2026-10-31). Reported
                              on /api/ble, never a knob                       */
} blm_ram_cfg_t;

/* the Lean set (measured on the bench unit: README, "Bluetooth memory
   profile") */
#define BLM_RAM_LEAN_MAX_ACT     2
#define BLM_RAM_LEAN_ACL_TX_BUFS 12
#define BLM_RAM_LEAN_SCAN_CACHE  0

/** `ble_ram_profile` word -> profile ("lean" and anything unknown = lean,
 *  the default). */
blm_ram_profile_t blm_ident_ram_profile(const char *word);
const char *blm_ident_ram_profile_word(blm_ram_profile_t profile);

/** The controller sizing for @p profile: full = @p full (the build's
 *  defaults as BT_CONTROLLER_INIT_CONFIG_DEFAULT() carries them), lean =
 *  the BLM_RAM_LEAN_* set, custom = @p custom clamped to the controller's
 *  ranges. */
void blm_ident_ram_config(blm_ram_profile_t profile, const blm_ram_cfg_t *full,
                          const blm_ram_cfg_t *custom, blm_ram_cfg_t *out);

/** on_validate: why a combination is refused (lean with extended
 *  advertising or a PHY other than 1M, a custom scan cache of 1..9),
 *  NULL when it is fine. */
const char *blm_ident_ram_check(blm_ram_profile_t profile, uint8_t adv_mode,
                                uint8_t phy_mask, int scan_cache);

/** on_migrate (v4 -> v5): the profile a stored v4 object gets: "full" when
 *  it chose a BLE 5 option (kept), NULL = the schema default otherwise. */
const char *blm_ident_ram_migrated(uint8_t adv_mode, uint8_t phy_mask);

/** `address` word -> the private (rotating) address? "private" = true,
 *  "public", NULL and anything unknown = false (the default, 4.x parity). */
bool blm_ident_address_private(const char *word);
const char *blm_ident_address_word(bool private_address);

/* ---- settings (ble_manager_settings.c) --------------------------------------- */

typedef struct
{
    bool     enabled;
    bool     pairing_at_boot;
    bool     bonding;          /* v2: keep long-term keys (reconnect w/o
                                  re-pairing); false = per-session only */
    bool     sc_only;          /* v3: require LE Secure Connections;
                                  refuses the legacy-pairing downgrade */
    bool     address_private;  /* v6: advertise with a rotating resolvable
                                  private address; false = the public
                                  hardware address (4.x, the default)  */
    bool     security_request; /* v6: ask the central to pair at connect
                                  (4.x); false = wait for its first secured
                                  access (a Windows app's own ceremony)  */
    uint32_t passkey;
    int      tx_power_dbm;
    uint16_t conn_min_units;   /* requested connection window, 1.25 ms units */
    uint16_t conn_max_units;
    uint8_t  phy_mask;         /* v4: BLM_PHY_* preferred on a link          */
    uint8_t  adv_mode;         /* v4: BLM_ADV_* advertising set(s)            */
    blm_ram_profile_t ram_profile; /* v5: the Bluetooth memory profile        */
    blm_ram_cfg_t     ram_custom;  /* v5: the custom profile's knobs          */
} blm_config_t;

/** Register the "ble_manager" descriptor with settings_manager. */
esp_err_t blm_settings_register(void);

/** Boot-applied config (filled by on_apply). */
const blm_config_t *blm_core_config(void);

bool blm_settings_is_configured(void); /* boot apply ran (standard §4.3) */

/* ---- core bridges (ble_manager.c) ------------------------------------------- */

void blm_core_on_rx(const uint8_t *data, size_t len); /* FFF2 -> subscriber */
void blm_core_on_cli_line(const char *line);          /* CLI IN -> handler  */
void blm_core_on_connect(void);
void blm_core_on_disconnect(void);
bool blm_core_pairing_allowed(void);

/* ---- GATT layer (ble_manager_gatt_nimble.c / ble_manager_gatt.c) ------------- */

esp_err_t blm_gatt_stack_up(const char *dev_name);    /* controller..adv    */
void      blm_gatt_stack_down(void);
bool      blm_gatt_connected(void);
bool      blm_gatt_secured(void);
uint16_t  blm_gatt_max_data(void);                    /* min(490, MTU-3)    */
bool      blm_gatt_congested(void);
int       blm_gatt_free_packets(void);
esp_err_t blm_gatt_notify_data(const uint8_t *buf, uint16_t len); /* FFF1   */
esp_err_t blm_gatt_notify_cli(const uint8_t *buf, uint16_t len);  /* CLIOUT */
void      blm_gatt_allow_pairing(bool allow);         /* + late encryption  */

/** Notify any value handle, retrying ENOMEM (mbuf/credit exhaustion) up
 *  to max_wait_ms. ESP_ERR_INVALID_STATE without a secured link,
 *  ESP_ERR_TIMEOUT when the credits never came back, ESP_FAIL otherwise.
 *  The congestion window it feeds is self-expiring (no latch). Bluedroid
 *  backend: ESP_ERR_NOT_SUPPORTED (channels are NimBLE-only). */
esp_err_t blm_gatt_notify_handle(uint16_t val_handle, const uint8_t *buf,
                                 uint16_t len, uint32_t max_wait_ms);

/** INDICATE any value handle and wait for the central's confirmation
 *  (one indication in flight per link). ESP_ERR_INVALID_STATE without a
 *  secured link or when the link drops mid-wait, ESP_ERR_TIMEOUT when
 *  neither the send credits nor the confirmation came within max_wait_ms
 *  (the PDU may still be delivered later), ESP_FAIL otherwise. The stream
 *  channels use this: a confirmed PDU is never silently lost. */
esp_err_t blm_gatt_indicate_handle(uint16_t val_handle, const uint8_t *buf,
                                   uint16_t len, uint32_t max_wait_ms);

/** OUT value handle of channel idx (0 when unknown / not NimBLE). */
uint16_t  blm_gatt_channel_out_handle(int idx);

/* ---- TX primitives (ble_manager_gatt_tx.c, NimBLE only) ------------------------ */

uint16_t  blm_gatt_conn_handle(void);      /* BLE_HS_CONN_HANDLE_NONE when down */
void      blm_tx_init(void);               /* indication semaphores, once      */
void      blm_tx_on_link_reset(void);      /* connect / disconnect / host reset */
void      blm_tx_on_indicate_done(int status); /* GAP NOTIFY_TX, indication     */

/** The live link's PHYs (1 = 1M, 2 = 2M, 3 = coded; 0 = not connected /
 *  not NimBLE). */
void      blm_gatt_phy(uint8_t *tx, uint8_t *rx);
/** The device's public Bluetooth address as "AA:BB:CC:DD:EE:FF" (the one it
 *  advertises with under `address` = public, and the identity a bonded phone
 *  resolves the private address to). ESP_ERR_INVALID_STATE before the stack
 *  is up. */
esp_err_t blm_gatt_address(char *buf, size_t len);

/* ---- advertising sets (ble_manager_gatt_adv.c, NimBLE only) ------------------- */

struct ble_gap_event; /* NimBLE; opaque here so the host build compiles */

/** Configure the legacy / extended advertising instances for @p adv_mode
 *  with the FFF0 UUID, @p name and the secondary PHY from @p phy_mask.
 *  Once per stack bring-up, from the host's sync callback. */
esp_err_t blm_adv_configure(uint8_t own_addr_type, const char *name,
                            uint8_t adv_mode, uint8_t phy_mask,
                            int (*gap_cb)(struct ble_gap_event *, void *));
void      blm_adv_start(void);   /* the configured set(s)                  */
void      blm_adv_stop(void);    /* every instance (a central connected)   */

/* ---- GATT service table (ble_manager_gatt_svc.c, NimBLE only) ----------------- */

esp_err_t blm_svc_register(const char *dev_name);   /* DIS + FFF0 (+channels) */
void      blm_svc_reset_rx(void);                   /* CLI reassembly buffer  */
uint16_t  blm_svc_data_out_handle(void);            /* FFF1                   */
uint16_t  blm_svc_cli_out_handle(void);             /* CLI OUT                */
uint16_t  blm_svc_channel_out_handle(int idx);
void      blm_svc_publish_handles(void);            /* after ble_gatts_start  */

/* ---- stream channel registry (ble_manager_channel.c) --------------------------- */

/* one indication's budget (send credits + the central's confirmation).
   The ATT indication timeout is 30 s; a phone confirms only after the
   writes it has already queued went out (a 16 KB upload window at the
   coex-limited 2 KB/s is ~9 s), so 2 s was too short (bench 2026-09-21). */
#define BLM_CHANNEL_TX_WAIT_MS 30000

int         blm_channel_count(void);
const ble_manager_channel_desc_t *blm_channel_desc(int idx);
void        blm_channel_set_in_handle(int idx, uint16_t attr_handle);
void        blm_channel_set_out_handle(int idx, uint16_t attr_handle);
int         blm_channel_find_in_handle(uint16_t attr_handle); /* -1 = none */
/** The OUT modes the owner allows (BLE_MANAGER_CH_OUT_* mask; a 0 in the
 *  descriptor reads as INDICATE only). Drives the GATT properties. */
uint8_t     blm_channel_out_modes(int idx);
/** Host-task: the central wrote a CCCD (GAP SUBSCRIBE); routed by the OUT
 *  value handle, ignored for handles that are not a channel's. */
void        blm_channel_on_subscribe(uint16_t attr_handle, bool notify,
                                     bool indicate);
/** Pure (ble_manager_pack.c, host-tested): the live OUT mode from what the
 *  owner allows and what the central subscribed to. Notify wins when both
 *  are subscribed and allowed; INDICATE when nothing usable is subscribed
 *  (indications need no CCCD on this stack: the pre-2026-09-22 contract). */
uint8_t     blm_channel_pick_out(uint8_t allowed, bool sub_notify,
                                 bool sub_indicate);
/** Host-task RX: true when every byte was accepted. */
bool        blm_channel_on_rx(int idx, const uint8_t *data, size_t len);
/** Host-task link events, fanned to every channel. */
void        blm_channel_on_link(ble_manager_channel_event_t evt);
void        blm_channel_lock(void);                 /* freeze at start        */

/* ---- IO layer (ble_manager_io.c) ---------------------------------------------- */

esp_err_t blm_io_start(void);
void      blm_io_stop(void);
esp_err_t blm_io_queue_tx(const uint8_t *data, size_t len);
esp_err_t blm_io_cli_write(const char *data, size_t len);

#ifdef __cplusplus
}
#endif
