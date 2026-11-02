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
 * @file fw_update_private.h
 * @brief Internal contract between fw_update's translation units: the
 *        boot-applied config, the engine's one state object and its
 *        helpers (fw_update.c), the job runners (fw_update_job.c), the
 *        public API (fw_update_api.c), the dongle relay
 *        (fw_update_dongle.c), the fetch path, the record store, the
 *        internal-stack helper and the status builder.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "fw_update.h"
#include "fw_update_core.h"
#include "fw_update_record.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FWU_TAG "fw_update"

#define FWU_BASE_URL_LEN   129
#define FWU_CERT_SET_LEN   25
#define FWU_MANIFEST_CAP   (64 * 1024)
#define FWU_DEFAULT_BASE_URL \
    "https://raw.githubusercontent.com/meatpiHQ/meatpi-devices/main/"

#define FWU_TASK_STACK       12288   /* PSRAM: TLS + the manifest parse       */
#define FWU_TASK_PRIO        4
#define FWU_FIRST_CHECK_S    90
#define FWU_AFTER_UPDATE_S   20      /* the first check after an update's boot */
#define FWU_RAM_FREE_MIN     (24 * 1024)
#define FWU_RAM_LARGEST_MIN  (8 * 1024)
#define FWU_HOLD_MINUTES     10
#define FWU_HOLD_RENEW_US    (5LL * 60 * 1000000)
#define FWU_RESTART_DELAY_MS 1500

#define FWU_DONGLE_DEVICE_TYPE "espnetlink"

/* ---- settings (fw_update_settings.c) ---------------------------------------------- */

typedef struct
{
    bool     enabled;           /**< the scheduled check                     */
    uint8_t  channel;           /**< FWU_CH_*                                */
    uint32_t check_interval_h;  /**< 1..168                                  */
    char     base_url[FWU_BASE_URL_LEN];
    bool     base_is_http;      /**< a bench root: http image URLs accepted  */
    char     cert_set[FWU_CERT_SET_LEN];
} fwu_config_t;

esp_err_t fwu_settings_register(void);
const fwu_config_t *fwu_settings_config(void);
bool fwu_settings_is_configured(void);

/* ---- the record store and the flash helper (fw_update_boot.c) -------------------- */

/** The NVS blob: false when none or invalid. */
bool fwu_store_read(fwu_record_t *out);
/** Seal and write (one NVS blob write). Caller on an INTERNAL stack. */
esp_err_t fwu_store_write(fwu_record_t *rec);
esp_err_t fwu_store_erase(void);

/** Run @p fn(@p arg) on a one-shot INTERNAL-stack task and wait for it
 *  (standard §2: flash writes never from a PSRAM stack). Bounded by
 *  @p wait_ms; ESP_ERR_NO_MEM / ESP_ERR_TIMEOUT otherwise. */
esp_err_t fwu_run_internal(void (*fn)(void *), void *arg, uint32_t wait_ms);

/* ---- the slots (fw_update_boot.c) --------------------------------------------------- */

/** What the flash says about the two app slots: the labels, the running
 *  image's otadata state and the bootloader's description. The otadata
 *  read maps flash and the description is a flash read: both freeze the
 *  cache, and IDF aborts a task whose stack is in PSRAM there (the field
 *  crash of 2026-10-21: the HA poster built its push from the status and
 *  parked the device). So the flash is read on an INTERNAL stack only, at
 *  init on the boot task and after every confirm on the helper, and the
 *  status copies this record from under the engine lock. */
typedef struct
{
    bool     valid;                 /**< read at least once                 */
    bool     pending_verify;        /**< NEW or PENDING_VERIFY              */
    char     running[8];
    char     previous[8];
    char     ota_state[16];         /**< ota_manager_img_state_str()        */
    bool     bootloader_rollback;   /**< description version >= 2           */
    uint32_t bootloader_version;
} fwu_slots_t;

/** Read the slots from flash into @p arg (an fwu_slots_t *, NULL = no-op).
 *  Caller on an INTERNAL stack: the boot task at init, or a
 *  fwu_run_internal() payload. */
void fwu_slots_read_fn(void *arg);

/* ---- the fetch path (fw_update_fetch.c) -------------------------------------------- */

typedef struct
{
    int    status;        /**< the final HTTP status, -1 transport           */
    bool   etag_hit;      /**< 304: the cached view stands                   */
    char   etag[64];
    char  *body;          /**< PSRAM, NUL-terminated; caller frees (free())  */
    size_t len;
    char   code[FW_UPDATE_CODE_LEN];   /**< snake_case failure code, ""     */
} fwu_fetch_result_t;

/** GET base_url + device_type/firmware/manifest.json (<= 64 KB) with the
 *  ETag when one is known; two attempts for a transport failure. */
esp_err_t fwu_fetch_manifest(const fwu_config_t *cfg, const char *device_type,
                             const char *etag, fwu_fetch_result_t *out);

typedef void (*fwu_progress_fn)(size_t received, size_t total);

/**
 * Download the release's image through ota_manager's pipe: size, sha256
 * and product checked before the image is activated. @p abort (nullable)
 * is polled per chunk. Three attempts (5, 20, 60 s apart) for a transport
 * failure, one for a refusal. On ESP_OK the next boot runs the image.
 * @p code / @p detail (FW_UPDATE_CODE_LEN / FW_UPDATE_DETAIL_LEN) say why not.
 */
esp_err_t fwu_fetch_image(const fwu_release_t *r, const fwu_config_t *cfg,
                          fwu_progress_fn progress, volatile bool *abort,
                          uint8_t *attempt, char *code, char *detail);

/**
 * Download a release's image into a PSRAM buffer of exactly its size (the
 * dongle relay stages the image before it pushes it): the Content-Length
 * must equal the manifest size, @p abort is polled per chunk, three
 * attempts for a transport failure. The caller owns @p buf.
 */
esp_err_t fwu_fetch_image_to_ram(const fwu_release_t *r, const fwu_config_t *cfg,
                                 uint8_t *buf, fwu_progress_fn progress,
                                 volatile bool *abort, uint8_t *attempt,
                                 char *code, char *detail);

/* ---- the engine's state (fw_update.c) --------------------------------------------- */

typedef enum
{
    FWU_CMD_CHECK = 0,
    FWU_CMD_INSTALL,
} fwu_cmd_type_t;

typedef struct
{
    uint8_t  type;
    uint8_t  target;
    uint32_t flags;
    uint32_t job_id;
    char     version[FW_UPDATE_VER_LEN];
} fwu_cmd_t;

typedef struct
{
    bool     active;
    uint32_t id;
    fw_update_target_t target;
    char     version[FW_UPDATE_VER_LEN];
    fw_update_job_state_t state;
    uint32_t received;
    uint32_t total;
    uint8_t  attempt;
    char     error[FW_UPDATE_CODE_LEN];
    char     detail[FW_UPDATE_DETAIL_LEN];
    int64_t  t_start_us;
    bool     backup_sd;
    uint32_t flags;
    char     transport[8];  /**< dongle: usb | wifi                        */
} fwu_job_t;

typedef struct
{
    bool     checked;
    int64_t  time;          /**< unix seconds, 0 without a clock            */
    int64_t  mono_us;       /**< esp_timer at the check                      */
    bool     ok;
    bool     etag_hit;
    char     error[FW_UPDATE_CODE_LEN];
    uint32_t count;
} fwu_check_t;

/** One PSRAM object (about 75 KB, the manifest view most of it), read and
 *  written under fwu_engine_lock() by every file of the component. */
typedef struct
{
    fwu_manifest_t  view;
    bool            view_valid;
    fwu_selection_t sel;
    char            etag[64];
    fwu_check_t     check;
    fwu_job_t       job;
    fwu_release_t   job_rel;
    fwu_record_t    rec;
    bool            rec_valid;
    fwu_slots_t     slots;      /**< the flash's word on the slots (above)  */
    uint32_t        job_seq;
    bool            inited;
    bool            started;
    volatile bool   stopped;
    volatile bool   abort;
    int64_t         net_up_us;
    int64_t         next_check_us;
    int64_t         hold_us;
    fw_update_event_cb_t event_cb;
    fw_update_uplink_fn  uplink;
    const fw_update_dongle_ops_t *dongle;   /**< NULL: no relay wired     */
    QueueHandle_t   cmds;
} fwu_state_t;

fwu_state_t *fwu_state(void);
#define FWU (fwu_state())

void fwu_engine_lock(void);
void fwu_engine_unlock(void);
const fwu_manifest_t *fwu_engine_manifest(bool *valid);
const fwu_selection_t *fwu_engine_selection(void);
const fwu_job_t *fwu_engine_job(void);
const fwu_check_t *fwu_engine_check(void);
const fwu_record_t *fwu_engine_record(bool *valid);
int32_t fwu_engine_next_check_s(void);
const char *fwu_engine_uplink(bool *is_cellular);

/* the engine's helpers, lent to the job, api and dongle files */
bool fwu_network_up(void);
void fwu_reselect(void);                                   /* under the lock */
void fwu_job_begin(uint32_t id, fw_update_target_t target,
                   const char *version, uint32_t flags,
                   fw_update_job_state_t state);           /* under the lock */
void fwu_job_fail(fw_update_job_state_t state, const char *code,
                  const char *detail);                     /* under the lock */
void fwu_notify(void);                                     /* NOT under the lock */
void fwu_verify_now(const char *why);
void fwu_renew_hold(void);
void fwu_write_record_fn(void *arg);   /* fwu_run_internal() payloads */
void fwu_confirm_fn(void *arg);        /* the confirm, then the slots read */
void fwu_erase_record_fn(void *arg);
void fwu_restart_task(void *arg);      /* the planned OTA_APPLY restart */
/** The check interval with its +-10 % jitter, in microseconds. */
int64_t fwu_jittered_interval_us(void);

/* the job runners (fw_update_job.c), called by the task */
void fwu_run_check(uint32_t job_id);
void fwu_run_install(const fwu_cmd_t *cmd);

/* ---- the dongle relay (fw_update_dongle.c, 2026-10-19) ---------------------------- */

void fwu_dongle_init(void);

/** The dongle manifest after the self check (the task, NOT under the
 *  lock): skipped when no relay is wired or no dongle is known. */
void fwu_dongle_check(const fwu_config_t *cfg);

/** The synchronous refusals of POST install target=dongle (under the
 *  lock): fills @p why (FW_UPDATE_CODE_LEN) and @p already; @p version
 *  (nullable) picks a release by hand (a downgrade is allowed then);
 *  @p chosen receives the version string to carry in the command. */
esp_err_t fwu_dongle_validate_install(const char *version, uint32_t flags,
                                      char *why, bool *already,
                                      char *chosen, size_t chosen_cap);

/** The relay job (the task). */
void fwu_dongle_install(const fwu_cmd_t *cmd);


/** The snapshot's dongle part (under the lock). */
void fwu_dongle_status_fill(fw_update_status_t *out);

/** A bench cap on the staging buffer (0 = none): the no_memory leg. */
void fwu_dongle_set_stage_cap(uint32_t bytes);

/* the status builder (fw_update_status.c) */
esp_err_t fwu_status_build(fw_update_status_t *out);

/* ---- routes and the console ----------------------------------------------------- */

/** Map an install refusal to the HTTP status text (fw_update_http.c). */
const char *fwu_http_status_for(esp_err_t err, const char *code);

#ifdef __cplusplus
}
#endif
