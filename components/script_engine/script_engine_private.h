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

/** Internals shared across the script_engine .c files. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "berry.h"
#include "esp_err.h"

#include "event_manager.h"

#define SE_SCRIPTS_DIR "/data/scripts"

/* Register all device bindings (uds/can/event/value/log/sleep) into a VM. */
void se_bindings_register(bvm *vm);
/* Every binding documented in script_engine_doc.c and vice versa (logs
 * the drift; called once at init). */
bool se_bindings_selfcheck(void);
/** Install the uds_manager-backed obd.* port (bind.c; call once at init). */
void se_obd_port_install(void);

/* [A-Za-z0-9_.-], <=40 chars, no "..": safe as a /data/scripts entry. */
bool se_script_name_ok(const char *name);

/* Set/clear the trigger event exposed to the NEXT run as evt_* globals
 * (NULL clears). The events glue sets it before an event-triggered run. */
void se_set_trigger(const em_event_t *ev);

/* Register the `script.run` event action (script_engine_events.c). */
void se_events_register(void);

/* Cooperative budget/kill check — bindings call it at every I/O point. */
void se_check_budget(bvm *vm);

esp_err_t script_engine_register_cli(void);

/* ---- settings (script_engine_settings.c) ------------------------------------ */

/** Register the "script_engine" descriptor with settings_manager. */
esp_err_t se_settings_register(void);

bool     se_settings_enabled(void);        /* scripts may run at all         */
uint32_t se_settings_max_runtime_ms(void); /* per-run budget (0 = unlimited) */
bool     se_settings_allow_reflash(void);  /* UDS 0x34-37 + transfer_file     */
bool     se_settings_exclusive(void);      /* scripts hold the bus (autopid off) */

/* the exclusive-bus hold (script_engine_bind.c): taken at a script's first
 * ECU access when the setting is on; the runner releases it after EVERY run
 * next to se_obd_autorelease(). */
void se_bus_hold_release(void);
bool     se_settings_is_configured(void);  /* boot apply ran (standard §4.3) */

/* Provided by the berry component's ESP port. */
void berry_port_set_capture(char *buf, size_t cap);
