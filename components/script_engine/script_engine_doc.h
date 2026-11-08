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
 * @file script_engine_doc.h
 * @brief The engine's self-description: the scripting reference (what a
 *        script can call, the globals it can read, a Berry primer, error
 *        hints, the limits) and the built-in example scripts, served to
 *        the web UI by GET /api/scripts/reference and /api/scripts/examples.
 *
 * Pure header (no Berry, no IDF): script_engine_doc.c and
 * script_engine_examples.c compile on the host, and the host suite checks
 * that the tables stay consistent (every binding documented in a known
 * group, every example a valid script name under the inline size cap).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

/* ---- the engine's limits (one place; the reference reports them) ------- */

#define SE_SRC_MAX          8192          /* inline run / check body (bytes) */
#define SE_OUT_MAX          4096          /* captured output per run         */
#define SE_SCRIPT_FILE_MAX  (64 * 1024)   /* a stored /data/scripts file     */
#define SE_SLEEP_MAX_MS     60000         /* one sleep_ms() call             */
#define SE_NAME_MAX         40            /* script file name (with .be)     */
#define SE_RESP_MAX_BYTES   128           /* response hex is cut after this  */

/** One device binding as the reference describes it. */
typedef struct
{
    const char *name;   /**< the Berry global, e.g. "obd_request"           */
    const char *sig;    /**< "obd_request(hexreq[, timeout_ms])"            */
    const char *group;  /**< a group id from se_doc_group_ok()              */
    const char *ret;    /**< what it returns                                */
    const char *doc;    /**< one or two sentences                           */
    const char *ex;     /**< a one-line example call                        */
} se_bind_doc_t;

/** A built-in example script. */
typedef struct
{
    const char *id;     /**< file-name-safe ("hello"); the suggested name    */
    const char *title;
    const char *desc;   /**< one sentence for the gallery                   */
    const char *needs;  /**< "" | "vehicle" | "dtc" | "can"                  */
    uint8_t     level;  /**< 1 first steps, 2 everyday, 3 advanced           */
    const char *src;    /**< the Berry source                               */
} se_example_t;

/** What the reference needs from the running engine. */
typedef struct
{
    const char *language;        /**< "Berry 1.1.0"                         */
    bool        enabled;
    bool        allow_reflash;
    bool        exclusive;     /* scripts hold the bus (autopid off) while at an ECU */
    uint32_t    max_runtime_ms;
} se_doc_ctx_t;

/* ---- bindings docs (script_engine_doc.c) --------------------------------- */

size_t               se_bind_doc_count(void);
const se_bind_doc_t *se_bind_doc(size_t i);
const se_bind_doc_t *se_bind_doc_find(const char *name);
bool                 se_doc_group_ok(const char *id);

/* ---- examples (script_engine_examples.c) --------------------------------- */

size_t              se_example_count(void);
const se_example_t *se_example_get(size_t i);
const se_example_t *se_example_find(const char *id);

/* ---- JSON builders (script_engine_doc.c; caller owns the result) --------- */

cJSON *se_doc_reference(const se_doc_ctx_t *ctx);
cJSON *se_doc_examples(void);   /* the gallery list: no sources */
