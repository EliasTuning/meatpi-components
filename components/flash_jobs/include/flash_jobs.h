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
 * @file flash_jobs.h
 * @brief The internal-stack worker for flash work (2026-10-31).
 *
 * Every access to the SPI flash (a LittleFS or NVS read or write, an OTA
 * write, a partition erase) switches the flash cache off for the SPI
 * transaction, and a task whose stack lives in PSRAM cannot run with the
 * cache off: IDF asserts it (`spi_flash_disable_interrupts_caches_and_
 * other_cpu`, cache_utils.c) and the device reboots. The component
 * standard's rule (section 2): such work runs on a task with an
 * internal-RAM stack.
 *
 * This component is that task, shared. `flash_jobs_run(fn, arg)` runs
 * `fn(arg)` and returns its result: in line when the caller's own stack is
 * internal (the main task at boot, the console, the worker itself inside a
 * job), otherwise on the worker task, with the caller blocked until the job
 * returns. The worker is created at the first marshalled job (4 KB of
 * internal heap since 2026-11-01, 6 KB before) and leaves after 5 s without
 * one, so a device that is not writing pays nothing at idle. One job runs
 * at a time; a job that calls `flash_jobs_run()` again runs that call in
 * line.
 *
 * Users: settings_manager (every settings file), filesystem (every `/data`
 * operation of a PSRAM-stack caller and the async pipe's chunks),
 * restart_tracker and dev_status_manager (their NVS records), ota_manager
 * (the update session), config_backup (the backup key in NVS), the
 * Tailscale client through microlink's flash hook (the device keys, the
 * peer cache: 2026-11-01), the script engine's file read, fw_update's
 * otadata and record payloads. The HTTP server task, the filesystem's
 * writer, the VPN state task and the Tailscale tasks run on PSRAM stacks
 * because of it.
 *
 * The job's argument may live in PSRAM (the caller's stack included): the
 * flash driver copies through an internal bounce buffer when a source or
 * destination is not in internal RAM. What must not happen inside a job is
 * waiting for the caller (the caller is blocked on the job).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A job: runs on an internal-RAM stack; its return value is handed back
 *  to the caller of flash_jobs_run(). */
typedef esp_err_t (*flash_job_fn_t)(void *arg);

typedef struct
{
    uint32_t inline_runs;  /**< calls run on the caller's own internal stack */
    uint32_t marshalled;   /**< calls run on the worker task */
    uint32_t spawns;       /**< worker tasks created */
    uint32_t exits;        /**< worker tasks that left after the idle period */
    uint32_t refusals;     /**< marshalled calls refused: no internal RAM for a worker */
    uint32_t stack_hw_min; /**< the lowest stack watermark a worker reported at
                                its exit, bytes (0: no worker has exited yet) */
    bool     worker_alive; /**< a worker task exists right now */
} flash_jobs_stats_t;

/**
 * Init: the log descriptor and the FreeRTOS objects. Call from main before
 * the first component that may run flash work from a PSRAM stack
 * (filesystem, settings_manager). Before init, a call from an internal
 * stack still runs in line; a call from a PSRAM stack is refused with
 * ESP_ERR_INVALID_STATE.
 */
esp_err_t flash_jobs_init(void);

/**
 * Run @p fn(@p arg) on an internal-RAM stack and return its result.
 *
 * In line when the calling task's stack is internal; otherwise on the
 * worker task, the caller blocked until the job returns. ESP_ERR_NO_MEM
 * when the worker had to be created and the internal heap could not give
 * its stack (the job did not run; the refusal is counted and logged at W,
 * one line per 10 s). ESP_ERR_INVALID_ARG for a NULL job.
 */
esp_err_t flash_jobs_run(flash_job_fn_t fn, void *arg);

/** True when the calling task's stack is in internal RAM. */
bool flash_jobs_on_internal_stack(void);

/** The counters (a snapshot). @p out may not be NULL. */
void flash_jobs_stats(flash_jobs_stats_t *out);

#ifdef __cplusplus
}
#endif
