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
 * @file fw_update.h
 * @brief Firmware updates pulled from the meatpi-devices manifests
 *        (service component, 2026-10-18; internal/TASK_fw_update.md).
 *
 * The device reads `<base_url><device_type>/firmware/manifest.json` on a
 * schedule (and on request), picks the newest release its channel offers
 * (stable, beta or alfa; stepping stones and yanked releases honoured),
 * tells the user (web UI, HA push, console) and, on request, downloads
 * the image through ota_manager's pipe (sha256 and product checked before
 * anything is activated), restarts and verifies the new image: a run that
 * crashes three times before it settled rolls back to the previous slot
 * (the crash-loop brake's verdict, fw_update_boot_check); a bootloader
 * built with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE falls back on its own
 * after one crash before the confirm. Nothing installs by itself in v1.
 *
 * Dependencies: ota_manager (the pipe), http_client_manager (the stream),
 * partition_migrate (the layout id), restart_tracker (the milestone and
 * the planned restart), settings_manager, log_manager, cmdline_manager,
 * http_server_manager, dev_status_manager, sleep_manager, config_backup
 * (the pre-wipe copies), nvs_flash (the job record). The uplink word and
 * the product allowlist come from the composition root.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    FW_UPDATE_TARGET_SELF = 0,
    FW_UPDATE_TARGET_DONGLE,  /**< the ESPNetLink dongle through the relay */
} fw_update_target_t;

/* ---- the dongle relay's view of the dongle (2026-10-19) ---------------------------
 * fw_update knows no dongle component: the composition root wires these
 * operations from espnetlink_link (main_glue). */

typedef struct
{
    bool present;        /**< a dongle is known: attached, paired or seen  */
    bool reachable;      /**< an address answers now                       */
    bool steady;         /**< the link's pairing machine is at rest        */
    bool usb_attached;   /**< the USB link is up                           */
    char host[16];       /**< "192.168.7.1" (USB) or the AP gateway        */
    char via[8];         /**< "usb" | "wifi" | ""                          */
    char device_id[13];
    char fw_version[24]; /**< what it reported at the last identify        */
    int  api_level;
} fw_update_dongle_endpoint_t;

typedef struct
{
    esp_err_t (*endpoint)(fw_update_dongle_endpoint_t *out);
    /** Pause the link's own traffic to the dongle while the image is pushed. */
    esp_err_t (*hold)(bool hold);
    /** The dongle restarts on purpose. */
    void (*expect_reboot)(void);
    /** GET /api/info now; @p out (nullable) receives the endpoint after. */
    esp_err_t (*identify)(int timeout_ms, fw_update_dongle_endpoint_t *out);
} fw_update_dongle_ops_t;

/** Wire the dongle relay (main). Without it `targets.dongle` says
 *  not_built and an install for the dongle answers dongle_absent. */
esp_err_t fw_update_set_dongle_ops(const fw_update_dongle_ops_t *ops);

/** Monotonic within a job. */
typedef enum
{
    FW_JOB_IDLE = 0,
    FW_JOB_CHECKING,
    FW_JOB_DOWNLOADING,
    FW_JOB_VERIFYING,
    FW_JOB_PUSHING,        /**< dongle target only                          */
    FW_JOB_REBOOTING,      /**< the image is READY, the restart scheduled   */
    FW_JOB_PENDING_VERIFY, /**< after the restart, until the milestone      */
    FW_JOB_VERIFIED,
    FW_JOB_ROLLED_BACK,
    FW_JOB_BOOT_FAILED,
    FW_JOB_FAILED,
    FW_JOB_ABORTED,
} fw_update_job_state_t;

#define FW_UPDATE_F_CONFIRM_WIPE   0x02u /**< accept a layout that formats  */

#define FW_UPDATE_VER_LEN     24
#define FW_UPDATE_FWVER_LEN   32
#define FW_UPDATE_BUILD_LEN   48
#define FW_UPDATE_SUMMARY_LEN 141
#define FW_UPDATE_MD5_LEN     33
#define FW_UPDATE_CODE_LEN    24
#define FW_UPDATE_DETAIL_LEN  96
#define FW_UPDATE_WIPES_LEN   64
#define FW_UPDATE_REASON_LEN  96
#define FW_UPDATE_NOTES_URL_LEN 256  /* base_url + the manifest's notes path */

/** The snapshot GET /api/fw_update renders (fw_update/HTTP_API.md). */
typedef struct
{
    /* settings as applied */
    bool     enabled;
    char     channel[8];
    char     cellular_policy[12];   /**< wifi_only: downloads never over LTE */
    /* the uplink right now */
    char     uplink[16];            /**< wifi | espnetlink | espnetlink_usb |
                                         eth | none                          */
    bool     is_cellular;
    /* the last manifest check */
    bool     checked;               /**< at least one check finished         */
    int64_t  check_time;            /**< unix seconds, 0 without a clock     */
    bool     check_ok;
    char     check_error[FW_UPDATE_CODE_LEN];
    bool     etag_hit;
    uint32_t check_age_s;
    int32_t  next_in_s;             /**< -1 = no scheduled check            */
    uint32_t checks;
    /* this device */
    char     installed[FW_UPDATE_FWVER_LEN];
    char     build[FW_UPDATE_BUILD_LEN];
    bool     release_known;
    bool     dev_build;
    bool     installed_yanked;
    char     yank_reason[FW_UPDATE_REASON_LEN];
    bool     have_latest;
    char     latest_version[FW_UPDATE_VER_LEN];
    char     latest_date[11];
    char     latest_summary[FW_UPDATE_SUMMARY_LEN];
    uint32_t latest_size;
    char     latest_notes_url[FW_UPDATE_NOTES_URL_LEN]; /**< the release notes, absolute;
                                          "" none. The page fetches them itself
                                          (2026-11-02): the device never does */
    bool     available;
    bool     installable;
    char     reason[FW_UPDATE_CODE_LEN];
    char     target_version[FW_UPDATE_VER_LEN];
    bool     step;
    char     verdict[12];
    char     wipes[FW_UPDATE_WIPES_LEN];
    char     target_layout_md5[FW_UPDATE_MD5_LEN];
    char     flashed_md5[FW_UPDATE_MD5_LEN];
    char     build_md5[FW_UPDATE_MD5_LEN];
    /* the ESPNetLink dongle through the relay (2026-10-19) */
    bool     dongle_wired;           /**< the relay is built and wired       */
    bool     dongle_present;
    bool     dongle_reachable;
    char     dongle_transport[8];    /**< usb | wifi | ""                    */
    char     dongle_device_id[13];
    char     dongle_installed[24];   /**< its fw_version, "" never seen      */
    int      dongle_api_level;
    bool     dongle_release_known;
    bool     dongle_dev_build;
    bool     dongle_checked;         /**< its manifest was fetched once      */
    char     dongle_check_error[FW_UPDATE_CODE_LEN];
    bool     dongle_have_latest;
    char     dongle_latest_version[FW_UPDATE_VER_LEN];
    char     dongle_latest_date[11];
    char     dongle_latest_summary[FW_UPDATE_SUMMARY_LEN];
    uint32_t dongle_latest_size;
    char     dongle_latest_notes_url[FW_UPDATE_NOTES_URL_LEN];
    bool     dongle_available;
    bool     dongle_installable;
    char     dongle_target_version[FW_UPDATE_VER_LEN];
    char     dongle_verdict[20];     /**< not_built | not_present | unreachable
                                          | needs_usb_flash | no_manifest |
                                          update_available | up_to_date |
                                          layout_mismatch | no_path |
                                          unknown_version                     */
    char     dongle_reason[FW_UPDATE_CODE_LEN];
    bool     dongle_cellular_needed; /**< the download would cost mobile data */
    bool     dongle_have_last;       /**< the last relay job (this boot)     */
    uint32_t dongle_last_id;
    char     dongle_last_from[24];
    char     dongle_last_to[FW_UPDATE_VER_LEN];
    fw_update_job_state_t dongle_last_state;
    char     dongle_last_error[FW_UPDATE_CODE_LEN];
    char     dongle_last_detail[FW_UPDATE_DETAIL_LEN];
    int64_t  dongle_last_time;
    uint32_t dongle_last_push_s;
    /* the job */
    bool     job_active;
    uint32_t job_id;
    fw_update_target_t job_target;
    char     job_transport[8];       /**< dongle: usb | wifi                 */
    char     job_version[FW_UPDATE_VER_LEN];
    fw_update_job_state_t job_state;
    uint32_t job_received;
    uint32_t job_total;
    uint8_t  job_attempt;
    char     job_error[FW_UPDATE_CODE_LEN];
    char     job_detail[FW_UPDATE_DETAIL_LEN];
    uint32_t job_elapsed_s;
    bool     job_backup_sd;
    /* the last job that got as far as a restart (NVS: outlives a power cycle) */
    bool     have_last;
    uint32_t last_id;
    fw_update_target_t last_target;
    char     last_from[FW_UPDATE_FWVER_LEN];
    char     last_to[FW_UPDATE_FWVER_LEN];
    fw_update_job_state_t last_state;
    char     last_by[12];           /**< "" | app | bootloader              */
    char     last_error[16];
    int64_t  last_time;
    /* rollback */
    bool     pending_verify;        /**< the running image is not confirmed */
    char     running[8];
    char     previous[8];
    char     ota_state[16];         /**< valid | new | pending_verify | ... */
    bool     bootloader_rollback;   /**< the bootloader was built with it   */
    uint32_t bootloader_version;
} fw_update_status_t;

/** Log + settings descriptors, the NVS record read. No network. */
esp_err_t fw_update_init(void);

/** The task: the first check 90 s after the network is up. */
esp_err_t fw_update_start(void);

/** Aborts a job, parks the task (the sleep prepare hook). */
esp_err_t fw_update_stop(void);

/**
 * BOOT PATH: right after restart_tracker_init(), BEFORE main_park_check().
 * Reads the job record; a run that the crash-loop brake would park rolls
 * back to the previous slot instead (never returns then), a verified or
 * failed update is recorded, anything else returns ESP_OK. Needs NVS and
 * the restart tracker only: no settings, no task.
 */
esp_err_t fw_update_boot_check(void);

/** Ask for a manifest check now. ESP_ERR_INVALID_STATE while an install
 *  runs; a running check answers ESP_OK with its job id. */
esp_err_t fw_update_check_now(uint32_t *job_id);

/**
 * Install @p version (NULL = the computed target) on @p target. Validates
 * against the last manifest view synchronously; the download runs on the
 * component's task. Idempotent on (target, version): the same job answers
 * ESP_OK with its id. Errors (the HTTP route maps them, HTTP_API.md):
 * ESP_ERR_INVALID_STATE busy, ESP_ERR_NOT_FOUND unknown version,
 * ESP_ERR_NOT_ALLOWED a verdict or policy refusal (see @p why),
 * ESP_ERR_INVALID_ARG a bad target. @p why (nullable, FW_UPDATE_CODE_LEN)
 * receives the snake_case code; @p already (nullable) is set when the
 * running firmware already is that version.
 */
esp_err_t fw_update_install(fw_update_target_t target, const char *version,
                            uint32_t flags, uint32_t *job_id, char *why,
                            bool *already);

/** Abort the job: ESP_ERR_NOT_FOUND none, ESP_ERR_INVALID_STATE too late
 *  (the restart is scheduled). */
esp_err_t fw_update_abort(void);

/** The snapshot (about 1 KB: keep it off task stacks). RAM only, so safe
 *  from a PSRAM-stack task (the HA poster): the slot fields are the record
 *  the boot task and the confirm helper read from flash (2026-10-21). */
esp_err_t fw_update_status(fw_update_status_t *out);

/** Mark the running image verified now (the console, the bench). */
esp_err_t fw_update_confirm(void);

/** Erase the job record (the bench). Caller on an internal stack. */
esp_err_t fw_update_forget(void);

/** Set when the view changes or a job moves (one consumer: the HA glue). */
typedef void (*fw_update_event_cb_t)(const fw_update_status_t *st);
esp_err_t fw_update_set_event_cb(fw_update_event_cb_t cb);

/** The uplink word and whether it costs mobile data: provided by the
 *  composition root (espnetlink_link, usb_host_manager, wifi_manager). */
typedef const char *(*fw_update_uplink_fn)(bool *is_cellular);
esp_err_t fw_update_set_uplink_provider(fw_update_uplink_fn fn);

/** The /api/fw_update routes (main, before http_server_manager_start). */
esp_err_t fw_update_register_http(void);

/** The `fwupdate` command: called by on_apply when `cli` is true. */
esp_err_t fw_update_register_cli(void);

#ifdef __cplusplus
}
#endif
