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
 * @file script_engine.c
 * @brief Berry VM lifecycle + the run entry point (bindings in
 *        script_engine_bind.c; settings in script_engine_settings.c,
 *        standard §4.1).
 */
#include "script_engine.h"
#include "script_engine_obd.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "be_debug.h"          /* be_tracestack: the traceback into the run output */
#include "berry.h"
#include "filesystem.h"
#include "log_manager.h"

#include "script_engine_doc.h"
#include "script_engine_private.h"

static const char *TAG = "script_engine";

/* The Berry interpreter + a binding's call chain (uds_request → isotp,
 * etc.) is deep — it overflows a 4 KB httpd worker stack. Scripts run on
 * a dedicated runner task with a big PSRAM stack (the VM heap is PSRAM
 * and no v1 binding touches flash, so §2 corollary permits it). */
#define SE_RUNNER_STACK (12 * 1024)
#define SE_RUNNER_PRIO  4

static bool s_started;

static volatile bool s_busy;
static volatile bool s_kill;
static volatile bool s_run_check;   /* compile only (POST /api/scripts/check) */
static uint32_t s_run_start_ms;

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;

/* runner task handoff */
static TaskHandle_t s_runner;
static StaticTask_t s_runner_tcb;
EXT_RAM_BSS_ATTR static StackType_t s_runner_stack[SE_RUNNER_STACK];
static SemaphoreHandle_t s_go, s_done;
static StaticSemaphore_t s_go_buf, s_done_buf;

/* shared run slot (guarded by s_lock) */
static const char *s_run_src;
static char *s_run_out;
static size_t s_run_out_cap;
static esp_err_t s_run_result;

/* Cooperative budget: the bindings call this at every I/O point (uds,
 * sleep, can, …) and it raises a Berry exception if the script was
 * killed or ran past max_runtime_ms. A pure-CPU spin isn't preempted in
 * v1 (enable BE_USE_DEBUG_HOOK for that) — real scripts do I/O. */
void se_check_budget(bvm *vm)
{
    if (s_kill)
    {
        be_raise(vm, "killed", "script stopped by request");
    }

    uint32_t budget_ms = se_settings_max_runtime_ms();

    if (budget_ms > 0 &&
        (esp_log_timestamp() - s_run_start_ms) > budget_ms)
    {
        be_raise(vm, "timeout", "script exceeded max_runtime_ms");
    }
}

/* ---- the actual VM run (on the runner task's big stack) --------------------- */

static void do_run(void)
{
    const char *src = s_run_src;
    char *out = s_run_out;
    size_t cap = s_run_out_cap;

    if (out != NULL && cap > 0)
    {
        out[0] = '\0';
        berry_port_set_capture(out, cap);
    }

    bvm *vm = be_vm_new();

    if (vm == NULL)
    {
        if (out) snprintf(out, cap, "vm alloc failed");
        s_run_result = ESP_ERR_NO_MEM;
    }
    else
    {
        se_bindings_register(vm);

        int r = be_loadstring(vm, src);

        if (r == BE_OK && !s_run_check)
        {
            r = be_pcall(vm, 0);
        }

        if (r != BE_OK)
        {
            /* the exception value ("syntax_error") sits under its message
               ("string:3: unexpected symbol …"): both reach the caller, and
               a runtime error adds Berry's stack traceback (string:<line>:
               in function `main`). Written through the Berry writer so the
               capture sink stays in step with what print()/log() wrote. */
            const char *kind = (be_top(vm) >= 2) ? be_tostring(vm, -2) : "error";
            const char *msg = (be_top(vm) >= 1) ? be_tostring(vm, -1) : "?";

            ESP_LOGW(TAG, "script error: %s: %s", kind ? kind : "?",
                     msg ? msg : "?");

            if (out != NULL)
            {
                if (!s_run_check)
                {
                    be_writestring("\nERROR: ");
                }

                be_writestring(kind ? kind : "?");
                be_writestring(": ");
                be_writestring(msg ? msg : "?");
                be_writestring("\n");

                if (r == BE_EXCEPTION)
                {
                    be_tracestack(vm);
                }
            }

            s_run_result = ESP_FAIL;
        }
        else
        {
            s_run_result = ESP_OK;
        }

        be_vm_delete(vm);
    }

    /* A script can never leak the OBD claim: normal end, error, and
     * kill all land here (tester-present disarmed, transport freed). */
    se_obd_autorelease();
    se_bus_hold_release(); /* and the exclusive-bus hold, if it took one */

    berry_port_set_capture(NULL, 0);
}

static void runner_task(void *arg)
{
    (void)arg;

    while (true)
    {
        xSemaphoreTake(s_go, portMAX_DELAY);
        do_run();
        xSemaphoreGive(s_done);
    }
}

/* ---- run (caller side; hands off to the runner task) ----------------------- */

static esp_err_t run_common(const char *src, char *out, size_t cap, bool check)
{
    if (!s_started || !se_settings_enabled() || s_runner == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (src == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        return ESP_ERR_INVALID_STATE; /* busy */
    }

    s_busy = true;
    s_kill = false;
    s_run_check = check;
    s_run_start_ms = esp_log_timestamp();
    s_run_src = src;
    s_run_out = out;
    s_run_out_cap = cap;
    s_run_result = ESP_FAIL;

    xSemaphoreGive(s_go);

    /* wait out the budget plus margin; if the runner is truly stuck the
     * kill switch + cooperative checks bring it back */
    TickType_t wait = pdMS_TO_TICKS(se_settings_max_runtime_ms() + 3000);

    if (xSemaphoreTake(s_done, wait) != pdTRUE)
    {
        s_kill = true;                    /* ask it to unwind */
        xSemaphoreTake(s_done, portMAX_DELAY);
    }

    esp_err_t result = s_run_result;
    s_busy = false;
    xSemaphoreGive(s_lock);
    return result;
}

esp_err_t script_engine_run(const char *src, char *out, size_t cap)
{
    return run_common(src, out, cap, false);
}

esp_err_t script_engine_check(const char *src, char *out, size_t cap)
{
    return run_common(src, out, cap, true);
}

void script_engine_kill(void)
{
    s_kill = true;
}

/* ---- stored scripts (/data/scripts/<name>.be) ------------------------------- */

/* se_script_name_ok moved to script_engine_name.c (pure — host-tested) */

/* Load /data/scripts/<name>[.be] into a fresh PSRAM buffer. MUST run on
 * an internal-RAM stack: littlefs reads go through esp_partition_read,
 * which disables the flash cache — a PSRAM stack asserts in
 * spi_flash_disable_interrupts_caches_and_other_cpu (§2 corollary
 * applies to READS too; bench-proven 2026-07-07). */
static esp_err_t load_script(const char *name, char **out_src)
{
    char path[80];
    size_t n = strlen(name);
    bool has_ext = (n > 3 && strcmp(name + n - 3, ".be") == 0);

    snprintf(path, sizeof(path), SE_SCRIPTS_DIR "/%s%s", name,
             has_ext ? "" : ".be");

    FILE *f = filesystem_open(path, "r");

    if (f == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0 || sz > SE_SCRIPT_FILE_MAX)
    {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    char *src = heap_caps_malloc((size_t)sz + 1,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (src == NULL)
    {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t rd = fread(src, 1, (size_t)sz, f);

    fclose(f);
    src[rd] = '\0';
    *out_src = src;
    return ESP_OK;
}

/* One-shot internal-stack loader — the marshal for PSRAM-stack callers
 * (the event dispatcher). Transient 4 KB internal alloc; script-run
 * frequency is human-scale, task churn is irrelevant. */
typedef struct
{
    const char       *name;
    char             *src;
    esp_err_t         err;
    SemaphoreHandle_t done;
} se_loader_req_t;

static void loader_task(void *arg)
{
    se_loader_req_t *req = arg;

    req->err = load_script(req->name, &req->src);
    xSemaphoreGive(req->done);
    vTaskDelete(NULL);
}

esp_err_t script_engine_run_file(const char *name, char *out, size_t cap)
{
    if (!se_script_name_ok(name))
    {
        return ESP_ERR_INVALID_ARG;
    }

    char *src = NULL;
    esp_err_t err;
    int probe = 0;                 /* only its ADDRESS matters (stack loc) */

    if (!esp_ptr_external_ram(&probe))
    {
        err = load_script(name, &src);        /* internal stack: inline */
    }
    else
    {
        StaticSemaphore_t done_buf;
        se_loader_req_t req =
        {
            .name = name,
            .done = xSemaphoreCreateBinaryStatic(&done_buf),
        };

        if (xTaskCreate(loader_task, "se_load", 4096, &req, 5, NULL)
            != pdPASS)
        {
            return ESP_ERR_NO_MEM;
        }

        xSemaphoreTake(req.done, portMAX_DELAY);
        err = req.err;
        src = req.src;
    }

    if (err != ESP_OK)
    {
        return err;
    }

    err = script_engine_run(src, out, cap);
    free(src);
    return err;
}

bool script_engine_busy(void)
{
    return s_busy;
}

/* ---- lifecycle -------------------------------------------------------------- */

esp_err_t script_engine_init(void)
{
    static const log_descriptor_t LOG_DESC = { "script_engine", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);
    se_events_register();       /* `script.run` rule action (static write) */
    se_obd_port_install();      /* obd.* bindings -> uds_manager */
    (void)se_bindings_selfcheck(); /* bindings vs. reference drift -> log */

    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }

    return se_settings_register();
}

esp_err_t script_engine_start(void)
{
    if (!se_settings_is_configured())
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_started)
    {
        return ESP_OK;
    }

    if (se_settings_enabled() && s_runner == NULL)
    {
        s_go = xSemaphoreCreateBinaryStatic(&s_go_buf);
        s_done = xSemaphoreCreateBinaryStatic(&s_done_buf);
        s_runner = xTaskCreateStatic(runner_task, "script_run",
                                     SE_RUNNER_STACK, NULL, SE_RUNNER_PRIO,
                                     s_runner_stack, &s_runner_tcb);
    }

    s_started = true;
    ESP_LOGI(TAG, "started (%s, max_runtime %lu ms)",
             se_settings_enabled() ? "enabled" : "disabled",
             (unsigned long)se_settings_max_runtime_ms());
    return ESP_OK;
}

esp_err_t script_engine_stop(void)
{
    s_kill = true;
    s_started = false;
    return ESP_OK;
}
