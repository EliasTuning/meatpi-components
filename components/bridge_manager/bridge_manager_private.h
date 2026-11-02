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
 * @file bridge_manager_private.h
 * @brief Internal API between bridge_manager translation units, incl. the
 *        pure config layer (host-testable: no FreeRTOS/IDF runtime deps).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BM_CTX_MAX 4096 /* per-direction translator ctx pool slot size:
                           256 until 2026-07-22; canmqtt keeps its frame
                           batch + JSON build buffer + tx reassembly ALL
                           in ctx (never on the 4 KB pump stack: the
                           stack-audit lesson), which needs ~4 KB. Slots
                           are PSRAM (2 x 6 bridges x 4 KB = 48 KB). */

/* ---- pure config layer (bridge_manager_config.c: host-tested) ------------- */

typedef struct
{
    bool enabled;
    char name[16];
    char a[16];
    char b[16];
    char translator[16];
} bm_bridge_cfg_t;

/** Parse one settings array item ("translator" defaults to "raw",
 *  "enabled" to false). Pure. */
esp_err_t bm_parse_bridge(const cJSON *item, bm_bridge_cfg_t *out);

/**
 * Cross-item + registry validation, registry passed as plain name lists so
 * this stays pure: translator names must be known, a != b, bridge names
 * unique, and an endpoint may appear in at most ONE enabled bridge
 * (single-consumer rule: two bridges reading one endpoint would split its
 * RX stream between their queues nondeterministically) UNLESS its
 * endpoint_multi flag says the provider fans RX out to every subscriber
 * (obd: the default TCP+USB passthrough pair rides this). Endpoint names
 * need NOT be registered (2026-10-29): a socket server or WebSocket
 * channel created in the same submit registers its jack at the next boot,
 * and a row left behind by a server renamed or removed must not refuse
 * every later save; a name that never resolves degrades that bridge alone
 * at start (bridge_manager_reason). A name not in the registry counts as
 * single-stream for the rule.
 * ep_count/tr_count < 0 = registry not final (BOOT apply): the translator
 * check AND the single-consumer rule are skipped, capabilities are
 * unknowable before the jacks register, and build_bridge degrades a
 * refused second subscribe alone. endpoint_multi is parallel to
 * endpoint_names (may be NULL = all single-consumer).
 */
esp_err_t bm_validate_bridges(const cJSON *bridges,
                              const char *const *endpoint_names,
                              const bool *endpoint_multi, int ep_count,
                              const char *const *translator_names,
                              int tr_count, char *err, size_t err_len);

/** Does any row of @p bridges (a settings array) name @p endpoint on either
 *  end, enabled or not? Pure. */
bool bm_bridges_use_endpoint(const cJSON *bridges, const char *endpoint);

/** The v1 -> v2 migration (2026-11-02): append the factory Bluetooth bridge
 *  `br_ble_obd` (obd <-raw-> ble, enabled) to @p bridges when no row uses the
 *  `ble` end and the table has room. True when a row was added. Pure. */
bool bm_bridges_add_ble_default(cJSON *bridges);

/* ---- registries (bridge_manager.c) ------------------------------------------ */

struct bridge_endpoint_s;
const void *bm_core_endpoint(const char *name);    /* bridge_endpoint_t*   */
const void *bm_core_translator(const char *name);  /* bridge_translator_t* */

/** Fill @p out with registered endpoint/translator names (≤ cap); returns
 *  the count. For the settings validator's registry-existence checks.
 *  @p multi_out (optional, parallel to @p out) receives each endpoint's
 *  multi_consumer capability for the single-consumer exemption. */
int bm_core_endpoint_names(const char **out, bool *multi_out, int cap);
int bm_core_translator_names(const char **out, int cap);

/** True once bridge_manager_start ran: the registry is FINAL, so the
 *  settings validator may use it (the translator check, the capability
 *  flags of the single-consumer rule) at runtime PUTs. At the boot apply
 *  this is false and the registry-dependent rules are skipped (dynamic
 *  socket/WS jacks register at bridge_endpoints_start, in between). */
bool bm_core_started(void);

/* ---- settings (bridge_manager_settings.c) ------------------------------------ */

/** Register the "bridge_manager" descriptor with settings_manager. */
esp_err_t bm_settings_register(void);

const bm_bridge_cfg_t *bm_core_bridge_cfg(int idx);/* NULL past end        */

int  bm_settings_count(void);         /* configured bridge slots            */
bool bm_settings_is_configured(void); /* boot apply ran (standard §4.3)     */

/* ---- pump (bridge_manager_pump.c) -------------------------------------------- */

esp_err_t bm_pump_start_all(void);
void      bm_pump_stop_all(void);
esp_err_t bm_pump_stats(const char *bridge_name, void *stats_out);

/** Why a configured, enabled bridge was not built at the last start: a
 *  short sentence (the unregistered endpoint or translator named, or a
 *  refused subscribe), NULL when it runs, is disabled, or is unknown. */
const char *bm_pump_reason(const char *bridge_name);

#ifdef __cplusplus
}
#endif
