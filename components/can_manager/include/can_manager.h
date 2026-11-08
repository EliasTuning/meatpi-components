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
 * @file can_manager.h
 * @brief Owner of the native CAN (TWAI) bus: WiCAN Pro TX=GPIO2 /
 *        RX=GPIO1 / STDBY=GPIO38.
 *
 * Model (ARCHITECTURE "the one pattern"): can_manager owns the ONE
 * physical CAN peripheral through a single shared `can_core` handle;
 * every consumer REGISTERS into it instead of touching the driver:
 *   - registered bus clients with their own filter/mask (add-on jacks),
 *   - task-owned RX queues (drop-oldest fan-out) via
 *     can_manager_subscribe_queue(),
 *   - plain TX via can_manager_send().
 *
 * Settings ("can_manager", reboot-to-apply): enabled (default false),
 * baud (kbit/s enum or "auto"), silent (listen-only for good), cli.
 *
 * Listen before talk (2026-10-02): a node at the wrong bitrate destroys the
 * bus traffic, in listen-only mode too on this chip, so the node always
 * starts WITHOUT its TX pin and gets it only once frames prove the bitrate
 * (or a fixed-bitrate bus stayed silent). Until then, and on a mismatch,
 * can_manager_send() refuses. can_manager_status() tells where the link is.
 *
 * The ESP TWAI transceiver and the MIC3624 OBD chip are two nodes on the
 * SAME vehicle CAN bus: electrically legal; contention between autopid
 * polling (via the MIC) and native-CAN traffic is a policy question left
 * to the user in v1 (README).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "can_core.h" /* the shared-bus core (adopted, see PROVENANCE) */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    bool     enabled;      /**< settings value                              */
    bool     running;      /**< bus up (driver installed + started)         */
    bool     silent;       /**< settings value: listen-only for good        */
    bool     baud_auto;    /**< settings value: baud = "auto"               */
    uint32_t baud_kbps;    /**< running: the bitrate the node is at;
                                stopped: the setting (0 = auto)             */
    can_core_link_t  link; /**< running: listen-before-talk state, the
                                detected bitrate, the mode right now        */
    can_core_stats_t stats;
} can_manager_status_t;

/** Register settings + log descriptors. Call in main's init pass. */
esp_err_t can_manager_init(void);

/** Bring the bus up per settings (no-op when disabled). §4 lifecycle. */
esp_err_t can_manager_start(void);

/** Park the bus: TWAI stopped/uninstalled, transceiver into standby.
 *  Called by main's sleep-prepare BEFORE sleep_manager's GPIO hold. */
esp_err_t can_manager_stop(void);

/** Transceiver driver on/off (its standby pin) without touching the
 *  controller. For a script that drives the TX pin by hand after
 *  can_manager_stop(); can_manager_start() sets it itself. */
void can_manager_transceiver(bool active);

/** The shared bus handle for the adopted elm327_* components.
 *  NULL while the bus is not running. */
struct can_core_handle_s *can_manager_core_handle(void);

/** Thin TX wrapper for v6-native consumers. ESP_ERR_INVALID_STATE when
 *  the bus is down or the node may not talk (listen-only setting, bitrate
 *  not proven yet, mismatch): it waits up to 100 ms for a promotion that
 *  is under way. */
esp_err_t can_manager_send(uint32_t id, bool ext, bool rtr,
                           const uint8_t *data, uint8_t dlc);

/** True when a frame handed to can_manager_send() would go out now: the
 *  bus is up, in normal mode, its link verdict reached. A consumer that
 *  has another path (UDS over the OBD chip) asks this, not "is CAN up". */
bool can_manager_tx_ready(void);

/**
 * @brief A tester states the bus bitrate (a J2534 CONNECT's baud, an slcan
 *        client's `S` command).
 *
 * Taken only when the link policy has no verdict of its own: `baud = auto`
 * with nothing proven, the bus silent for 300 ms at the candidate in use,
 * never bitten (can_ab_adopt). The node then runs at that bitrate in the
 * configured mode, `adopted` and unverified, until frames prove it or the
 * watchdog demotes it on the first unreadable traffic. A fixed bitrate is
 * never overruled: a tester that disagrees with it is told by its own
 * protocol (J2534 ERR_INVALID_BAUDRATE). Born 2026-10-17 from a J2534 tester
 * on a gatewayed OBD port that speaks only when asked: with `auto` the
 * session opened and every write was refused ("connected, no comm").
 *
 * @return ESP_OK taken (the node bounced to that bitrate);
 *         ESP_ERR_NOT_SUPPORTED not taken (fixed bitrate, a verdict exists,
 *         the bus is not silent, bitten, or not a bitrate this node knows);
 *         ESP_ERR_INVALID_STATE before the bus is up; ESP_FAIL when the node
 *         could not be restarted.
 */
esp_err_t can_manager_adopt_baud(uint32_t baud_kbps);

/** Subscribe a task-owned queue of can_core_frame_t (drop-oldest when
 *  full). monitor_all=true bypasses filter/mask. Returns the subscriber
 *  index via out_idx (>=0) for later unsubscribe. */
esp_err_t can_manager_subscribe_queue(QueueHandle_t q, uint32_t filter,
                                      uint32_t mask, bool ext,
                                      bool monitor_all, int *out_idx);

esp_err_t can_manager_unsubscribe_queue(int idx);

/** Name a subscription for the status views (/api/can "subscribers", the
 *  console). @p name must outlive it: a string literal. */
esp_err_t can_manager_subscriber_name(int idx, const char *name);

/** One subscriber as the status views show it. */
typedef struct
{
    const char *name;   /**< "" when it gave none.                           */
    uint32_t    drops;  /**< Frames its queue lost: it was full, the oldest
                             frame made room for the newest.                 */
} can_manager_subscriber_t;

/** The subscriber in slot @p idx (0 .. capacity-1). false: the slot is
 *  free, or the bus is down. A subscriber reads its own losses with the
 *  index can_manager_subscribe_queue() gave it. */
bool can_manager_subscriber_get(int idx, can_manager_subscriber_t *out);

/** Occupancy of the queue-subscriber table (boot health report). */
void can_manager_capacity(size_t *used, size_t *cap);

esp_err_t can_manager_status(can_manager_status_t *out);

/* ---- what is on the bus? (never transmits) --------------------------------- */

typedef enum
{
    CAN_PROBE_NONE = 0,   /**< No probe ran yet (status only).               */
    CAN_PROBE_SILENT,     /**< Nothing on the bus while listening.           */
    CAN_PROBE_LIVE,       /**< Frames read at baud_kbps.                     */
    CAN_PROBE_UNREADABLE, /**< Traffic that no bitrate tried can read.       */
} can_probe_result_t;

typedef struct
{
    can_probe_result_t result;
    uint32_t baud_kbps;   /**< LIVE: the bitrate that reads the bus.         */
    uint32_t frames;      /**< Frames seen while probing.                    */
    uint32_t took_ms;
} can_manager_probe_t;

/**
 * @brief Listen to the bus for a moment and say what is there.
 *
 * Never transmits: the node listens without its TX pin. While can_manager
 * is disabled a temporary listen-only node tries 500 and 250 kbit/s (the
 * two ISO 15765-4 / J1939 rates) and is torn down again; while it runs, or
 * while a watch is held, that node's own verdict is returned at once.
 * A busy bus answers in milliseconds; "silent" needs 400 ms in which the
 * node heard nothing at all (no frame, no receive error, no busy RX line);
 * a bus that is neither after 1.5 s is reported unreadable. Callers: whoever
 * is about to make the OBD chip transmit at a bitrate of its own.
 *
 * @return ESP_OK with @p out filled; ESP_ERR_INVALID_STATE before
 *         can_manager_start(); ESP_FAIL when the node could not be brought up
 *         (the caller then knows nothing about the bus).
 */
esp_err_t can_manager_probe(can_manager_probe_t *out);

/**
 * @brief Hold a listen-only node on the bus (no TX pin), or let it go.
 *
 * For a bus guard that must look before every transmission while the bus
 * is silent: with a watch, can_manager_probe() answers at once from what
 * the node heard. Costs nothing on a silent bus, an interrupt per frame on
 * a live one, so let it go once the bitrate is known. No-op while
 * can_manager runs by its own settings (that node already listens).
 */
esp_err_t can_manager_watch(bool on);

/** The last probe's answer and its age (CAN_PROBE_NONE when none ran).
 *  For status surfaces; never blocks. */
void can_manager_last_probe(can_manager_probe_t *out, uint32_t *age_ms);

/** One identifier heard by can_manager_sample_ids(). */
typedef struct
{
    uint32_t id;
    uint16_t count;       /**< Frames with this id in the window (saturates). */
    uint8_t  dlc;         /**< Of the last one.                              */
    bool     ext;         /**< 29-bit.                                       */
} can_id_seen_t;

/**
 * @brief Listen for @p ms and report the distinct identifiers heard.
 *
 * Never transmits. Like can_manager_probe(): a running bus or a held watch
 * is read as it is; otherwise a listen-only node of its own goes up for the
 * duration, and the window of @p ms opens once that node's link is verified
 * (the bitrate found by listening, up to 4 s; a silent bus returns nothing
 * after that). For whoever must tell a J1939 network (well-known groups
 * broadcast on 29-bit ids) from an OBD port before the native bus is enabled.
 *
 * @param out    At most @p max distinct ids, in order of first appearance.
 * @param n      Out: how many were written.
 * @param frames Out (may be NULL): frames read in the window, the ones
 *               beyond @p max distinct ids included.
 * @return ESP_OK; ESP_ERR_INVALID_STATE before can_manager_start();
 *         ESP_FAIL when the node could not be brought up.
 */
esp_err_t can_manager_sample_ids(uint32_t ms, can_id_seen_t *out, size_t max,
                                 size_t *n, uint32_t *frames);

/** "none", "silent", "live" or "unreadable". Never NULL. */
const char *can_manager_probe_name(can_probe_result_t result);

/** Zero the stats counters (CLI `can -z`). */
void can_manager_zero_stats(void);

/** /api/can route (HTTP compositions only: main wires it). */
esp_err_t can_manager_register_http(void);

#ifdef __cplusplus
}
#endif
