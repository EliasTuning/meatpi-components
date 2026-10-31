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
 * @file ble_manager_store_nvs.c
 * @brief The NimBLE bond store's NVS writes on the flash_jobs worker
 *        (2026-11-01, lever 3 of internal/TASK_ble_tailscale_margin.md).
 *
 * NimBLE persists a bond (the long-term keys, the identity, the CCCD
 * state of a bonded peer) from its host task: ble_store_config.c calls one
 * of the persist functions of ble_store_nvs.c after every write or delete
 * of its RAM tables, and those functions open NVS, write or erase the blob
 * and commit. That is flash work, which cannot run on a PSRAM stack (the
 * standard, section 2; IDF asserts it), and it was the one reason the host
 * task kept a 4 KB internal stack. The linker's --wrap (CMakeLists.txt,
 * the ip4_routes idiom) lands every call to a persist function here, and
 * the wrapper runs the real one through flash_jobs_run(): in line on an
 * internal stack, on the worker when the caller's stack is in PSRAM (the
 * host task's, since the same day). The caller blocks until the write is
 * done, as it did when the write ran on its own stack, so the host's
 * ordering is unchanged.
 *
 * The read side is wrapped the same way: ble_store_config_init() loads
 * every table from NVS, and the host calls it itself from the host task
 * at every startup (ble_hs_pvcy_set_default_irk(), under
 * CONFIG_BT_NIMBLE_STATIC_TO_DYNAMIC; the first image without this wrap
 * aborted there at 1 s of uptime, IDF's assertion naming the task), as
 * well as blm_gatt_stack_up() from the main task. The restore helpers of
 * ble_store_config.c (ble_restore_*_sec_nvs, the bond-count re-sequencing)
 * reach the flash only through the persist functions, so they are
 * covered. The two persist functions this build does not compile
 * (encrypted advertising data, host-based privacy) are wrapped only when
 * their Kconfig options are on: a wrapper needs its __real_ symbol.
 *
 * A job the worker could not run (no internal RAM for it, or a call from
 * a PSRAM stack before flash_jobs_init()) is reported as
 * BLE_HS_ESTORE_FAIL, which is what the store reports for an NVS failure:
 * the bond works for the session and is not persisted, one W line says
 * so, and the worker's refusal counter (health.flash_jobs) carries it.
 */
#include "sdkconfig.h"

#if CONFIG_BT_NIMBLE_NVS_PERSIST

#include "esp_err.h"
#include "esp_log.h"

#include "host/ble_hs.h"

#include "flash_jobs.h"

static const char *TAG = "ble_manager";

typedef struct
{
    int (*fn)(void); /* the real persist function */
    int rc;          /* its result                 */
} store_job_t;

static esp_err_t store_job(void *arg)
{
    store_job_t *job = arg;

    job->rc = job->fn();
    return ESP_OK;
}

static int run_persist(int (*real)(void), const char *what)
{
    store_job_t job = { .fn = real, .rc = 0 };
    esp_err_t   err = flash_jobs_run(store_job, &job);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "bond store: %s not persisted (%s)", what,
                 esp_err_to_name(err));
        return BLE_HS_ESTORE_FAIL;
    }

    return job.rc;
}

/* the load: void, so the job carries nothing back; a load the worker
   could not run leaves the tables empty (every bond of this boot is
   per-session) and says so once */
void __real_ble_store_config_init(void);

static esp_err_t load_job(void *arg)
{
    (void)arg;
    __real_ble_store_config_init();
    return ESP_OK;
}

void __wrap_ble_store_config_init(void)
{
    esp_err_t err = flash_jobs_run(load_job, NULL);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "bond store not loaded (%s): bonds of this boot are "
                      "per-session", esp_err_to_name(err));
    }
}

int __real_ble_store_config_persist_our_secs(void);
int __wrap_ble_store_config_persist_our_secs(void)
{
    return run_persist(__real_ble_store_config_persist_our_secs, "our keys");
}

int __real_ble_store_config_persist_peer_secs(void);
int __wrap_ble_store_config_persist_peer_secs(void)
{
    return run_persist(__real_ble_store_config_persist_peer_secs, "peer keys");
}

int __real_ble_store_config_persist_cccds(void);
int __wrap_ble_store_config_persist_cccds(void)
{
    return run_persist(__real_ble_store_config_persist_cccds, "cccds");
}

int __real_ble_store_config_persist_csfcs(void);
int __wrap_ble_store_config_persist_csfcs(void)
{
    return run_persist(__real_ble_store_config_persist_csfcs, "csfcs");
}

int __real_ble_store_config_persist_local_irk(void);
int __wrap_ble_store_config_persist_local_irk(void)
{
    return run_persist(__real_ble_store_config_persist_local_irk, "local irk");
}

int __real_ble_store_config_persist_rpa_recs(void);
int __wrap_ble_store_config_persist_rpa_recs(void)
{
    return run_persist(__real_ble_store_config_persist_rpa_recs, "rpa records");
}

#if CONFIG_BT_NIMBLE_ENC_ADV_DATA
int __real_ble_store_config_persist_eads(void);
int __wrap_ble_store_config_persist_eads(void)
{
    return run_persist(__real_ble_store_config_persist_eads,
                       "encrypted advertising data");
}
#endif

#if CONFIG_BT_NIMBLE_HOST_BASED_PRIVACY
int __real_ble_store_persist_peer_records(void);
int __wrap_ble_store_persist_peer_records(void)
{
    return run_persist(__real_ble_store_persist_peer_records, "peer records");
}
#endif

#endif /* CONFIG_BT_NIMBLE_NVS_PERSIST */
