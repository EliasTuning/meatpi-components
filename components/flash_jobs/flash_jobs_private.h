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
 * @file flash_jobs_private.h
 * @brief The pure core of the worker's lifetime (host-tested): where a call
 *        runs, when a worker is created, when it leaves, the counters.
 *        flash_jobs.c drives it under a spinlock; the semaphores and the
 *        task are the target side's.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The worker's stack: the deepest jobs are a settings file write through
   LittleFS (an open, a write, a sync and a rename, each through the
   partition layer), the end of an OTA session (the image verified
   through the partition reads), the peer cache of the Tailscale client (a
   blob of up to 5.8 KB into NVS) and a script file read through stdio.
   It was 6 KB on 2026-10-31; the whole bench battery of that night (the
   settings saves, the uploads, the OTA session, the crash reports, the
   backup copies) used 2.5 KB of it at most (stack_hw_min 3576), so 4 KB
   leaves the standard's kilobyte (2026-11-01); the watermark is logged at
   every exit (stats.stack_hw_min, `system -m`, /api/status health) for the
   next look. An internal stack's overflow is caught by FreeRTOS
   (CONFIG_FREERTOS_CHECK_STACKOVERFLOW), never silent. */
#define FLASH_JOBS_STACK_BYTES 4096

/* How long a worker waits for the next job before it leaves. The web UI's
   Submit writes a handful of settings files within a second or two; the
   backup copies sweep writes several parts in a row. */
#define FLASH_JOBS_IDLE_MS 5000

/* A refusal (no internal RAM for a worker) logs one W line per period. */
#define FLASH_JOBS_LOG_PERIOD_MS 10000

typedef struct
{
    bool     worker_alive; /* a worker task exists (set at its creation,
                              cleared by the worker when it decides to leave) */
    bool     job_pending;  /* a job is published and the worker has not
                              taken it yet */
    uint32_t inline_runs;
    uint32_t marshalled;
    uint32_t spawns;
    uint32_t exits;
    uint32_t refusals;
    uint32_t stack_hw_min; /* 0 until a worker reports one */
    uint32_t last_log_ms;  /* the refusal rate limit */
    bool     logged_once;
} fj_state_t;

typedef enum
{
    FJ_INLINE = 0, /* run on the caller's own stack */
    FJ_MARSHAL,    /* publish to the worker and wait */
} fj_route_t;

typedef enum
{
    FJ_WORKER_RUN = 0, /* a job is pending: run it */
    FJ_WORKER_WAIT,    /* a stale wake-up: wait again */
    FJ_WORKER_EXIT,    /* idle for the whole period: leave */
} fj_worker_step_t;

/** Where a call runs. A caller on an internal stack (the worker included:
 *  its own stack is internal) runs the job in line. */
fj_route_t fj_route(bool on_internal_stack);

/** The caller publishes a job (under the lock that serializes callers).
 *  Returns true when no worker is alive and one must be created first. */
bool fj_publish(fj_state_t *st);

/** The outcome of creating a worker. A failure withdraws the pending job
 *  and counts a refusal. */
void fj_spawned(fj_state_t *st, bool ok);

/** The worker woke up: @p signalled when the caller's wake-up arrived,
 *  false when the idle period ran out. A pending job is always run, even
 *  after a timeout (the caller published before the worker could look). */
fj_worker_step_t fj_worker_wake(fj_state_t *st, bool signalled);

/** A job returned: count it by how it ran. */
void fj_done(fj_state_t *st, fj_route_t how);

/** The worker leaves: record its stack watermark (bytes, 0 = unknown). */
void fj_worker_exit(fj_state_t *st, uint32_t stack_hw);

/** A refusal: count it; true when it should be logged (one per period). */
bool fj_refused(fj_state_t *st, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
