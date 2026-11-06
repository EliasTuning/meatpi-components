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
 * @file sleep_manager.c
 * @brief Lifecycle and the state task: policy at 1 Hz, entry sequence,
 *        the light-sleep nap loop with OBD-asleep verification, the
 *        boot-loop guard, and wake-by-reboot (see
 *        include/sleep_manager.h for the model). Settings live in
 *        sleep_manager_settings.c (standard §4.1).
 */
#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "battery_monitor.h"
#include "dev_status_manager.h"
#include "log_manager.h"
#include "obd_chip.h"
#include "restart_tracker.h"

#include "sleep_manager_private.h"

static const char *TAG = "sleep_manager";

/* WiCAN Pro power pins (legacy hw_config.h) */
#define SM_CAN_STDBY_GPIO CONFIG_WICAN_SLEEP_CAN_STDBY_GPIO
#define SM_USB_PWR_GPIO   CONFIG_WICAN_SLEEP_USB_PWR_GPIO

static sm_policy_t s_policy;
static sm_critical_t s_crit;  /* the critical battery floor tracker */
static sleep_manager_prepare_cb_t s_prepare_cb;
static volatile bool s_run;
static volatile uint32_t s_test_wake_s; /* !=0: forced bench sleep */
static TaskHandle_t s_task;
static StaticTask_t s_tcb;                    /* internal: FreeRTOS */
/* INTERNAL stack: this task calls esp_light_sleep_start, which runs
 * with the cache disabled; a PSRAM stack here = interrupt-watchdog
 * reset at the first nap (hit on the bench 2026-07-07; legacy's
 * static-DRAM stack was load-bearing, not habit) */
static StackType_t s_stack[4096];

static sleep_manager_status_t s_status;

/* The countdown the surfaces report ("sleeps in N s"): cause and deadline
 * as ONE word (sm_pending_pack), written under s_lock, so a reader on
 * another task never sees half an update. */
static volatile uint32_t s_pending;

/* The hold (Ali, 2026-10-06). The policy and the floor tracker belong to
 * the state task; a request from the web UI lands on the httpd task, so
 * the two deadlines are touched under this lock only: the task holds it
 * for the evaluation of a pass, never across the entry sequence or a nap. */
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;          /* internal: FreeRTOS */
static uint32_t s_hold_until;                 /* ms, while s_hold_active */
static bool     s_hold_active;
static uint8_t  s_holds_used;                 /* per boot */
/* The activity hold (2026-11-06): a log read while a countdown runs. Edited
 * under s_lock like the hold; the httpd task brings the reads. */
static sm_activity_t s_activity;

/* ---- sleep entry / nap loop ---------------------------------------------- */

/* Wall-clock ms for policy deadlines. NOT esp_log_timestamp(): manual
 * esp_light_sleep_start does NOT step the FreeRTOS tick (only the
 * esp_pm auto-sleep path does), so tick time crawls ~40x slow inside
 * the nap loop: a tick-based 1 s wake-stable window took ~80 s of
 * real time (2026-07-21 wake bug). esp_timer IS slept-time
 * compensated. */
static uint32_t sm_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static const char *state_name(sleep_manager_state_t st)
{
    switch (st)
    {
        case SLEEP_MANAGER_LOW_VOLTAGE:  return "low_voltage";
        case SLEEP_MANAGER_SLEEPING:     return "sleeping";
        case SLEEP_MANAGER_WAKE_PENDING: return "wake_pending";
        default:                         return "normal";
    }
}

void sleep_manager_board_down(void)
{
    /* CAN transceiver to standby */
    gpio_set_direction(SM_CAN_STDBY_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(SM_CAN_STDBY_GPIO, 1);

    (void)obd_chip_sleep(true);

    /* keep the USB power rail off through the naps (legacy) */
    gpio_set_direction(SM_USB_PWR_GPIO, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(SM_USB_PWR_GPIO, 0);
    gpio_sleep_set_pull_mode(SM_USB_PWR_GPIO, GPIO_PULLDOWN_ONLY);
    gpio_pulldown_en(SM_USB_PWR_GPIO);
    gpio_hold_en(SM_USB_PWR_GPIO);
}

/* A chip firmware update owns the chip's wire for about 90 s (3557 records);
 * the sleep entry pulls the chip's sleep pin, and a half-written image
 * strands the chip in its bootloader (issue #1023, 2026-10-28). Bounded:
 * a wedged update must never hold a flat battery awake for good. */
#define SM_FW_UPDATE_WAIT_MS (180 * 1000)

static void enter_sleep_sequence(float volts)
{
    if (obd_chip_update_in_progress())
    {
        ESP_LOGW(TAG, "OBD chip firmware update in progress: sleep waits "
                 "for it (up to %u s)", SM_FW_UPDATE_WAIT_MS / 1000u);

        for (uint32_t waited = 0;
             obd_chip_update_in_progress() && waited < SM_FW_UPDATE_WAIT_MS;
             waited += 1000)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    ESP_LOGW(TAG, "entering sleep (%.2f V)", volts);
    sm_events_entering(volts); /* rules get one shot BEFORE teardown */
    vTaskDelay(pdMS_TO_TICKS(500)); /* let the dispatcher drain */

    dev_status_manager_clear(DEV_STATUS_BIT_AWAKE);
    dev_status_manager_set(DEV_STATUS_BIT_SLEEP);

    if (dev_status_manager_get() & DEV_STATUS_BIT_AUTOPID_ENABLED)
    {
        (void)dev_status_manager_wait_all(DEV_STATUS_BIT_AUTOPID_IDLE,
                                          pdMS_TO_TICKS(
                                              SM_AUTOPID_IDLE_MS));
    }

    if (s_prepare_cb != NULL)
    {
        s_prepare_cb(); /* main's ordered component stops */
    }

    sleep_manager_board_down();

    ESP_LOGI(TAG, "components down; napping (wake >= %.2f V)",
             sleep_settings_config()->wake_v);
    vTaskDelay(pdMS_TO_TICKS(1000));
}

/* one 2 s light-sleep nap + OBD-still-asleep verification */
static void nap_and_verify(void)
{
    esp_sleep_enable_timer_wakeup(SM_NAP_US);
    esp_light_sleep_start();
    s_status.naps++;

    static uint8_t s_resleeps;

    if (obd_chip_status_ok()) /* chip reports READY = it woke up */
    {
        s_status.chip_resleeps++;

        if (s_resleeps < SM_RESLEEP_MAX)
        {
            s_resleeps++;
            ESP_LOGW(TAG, "OBD chip awake during sleep; re-sleeping "
                     "(%u/%u)", s_resleeps, SM_RESLEEP_MAX);
            /* hard reset FIRST (legacy parity): a chip mid-monitor
             * (ATMA) ignores the sleep pin, without the reset every
             * retry fails and the loop ends in a recovery reboot
             * (sleep matrix `elm_monitor`, 2026-07-21) */
            (void)obd_chip_hard_reset();
            vTaskDelay(pdMS_TO_TICKS(500));
            (void)obd_chip_sleep(true);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        else
        {
            ESP_LOGE(TAG, "OBD chip will not sleep; recovery reboot");
            restart_tracker_restart(
                RESTART_TRACKER_PLANNED_REASON_INTERNAL_RECOVERY,
                RESTART_TRACKER_SOURCE_SLEEP_MODE, 0);
        }
    }
    else
    {
        s_resleeps = 0;
    }
}

/* The planned reason is what the next boot reports (restart history, the web
 * UI's "Last wake-up"): a voltage recovery and the periodic check-in must be
 * told apart (meatpi 2026-09-07). */
static void wake_reboot(const char *why,
                        restart_tracker_planned_reason_t reason)
{
    ESP_LOGI(TAG, "waking by reboot (%s)", why);
    restart_tracker_restart(reason, RESTART_TRACKER_SOURCE_SLEEP_MODE, 0);
}

/* boot-loop guard: repeated unexpected resets on a sagging battery
 * mean crash-loop-until-flat: sleep instead (legacy parity) */
static bool bootloop_guard_trips(float volts)
{
    restart_tracker_state_t rt;

    if (restart_tracker_get_state(&rt) != ESP_OK)
    {
        return false;
    }

    return rt.unexpected_reset_count >= 3 && volts < SM_ERROR_V;
}

/* under s_lock: what will put the device to sleep first, and when; a hold
 * is over once its window has passed or nothing counts any more */
static void publish_pending(uint32_t now)
{
    uint32_t deadline = now;
    sleep_manager_pending_t cause =
        sm_pending_eval(&s_policy, s_run, &s_crit, now, &deadline);

    s_pending = sm_pending_pack(cause, deadline);

    if (s_hold_active && (cause == SLEEP_MANAGER_PENDING_NONE ||
                          (int32_t)(now - s_hold_until) >= 0))
    {
        s_hold_active = false;
    }

    if (cause == SLEEP_MANAGER_PENDING_NONE)
    {
        s_activity.active = false; /* the next countdown gets a fresh cap */
    }
}

/* ---- state task -------------------------------------------------------------- */

static void state_task(void *arg)
{
    const sm_cfg_t *cfg = sleep_settings_config();

    (void)arg;

    /* meatpi bench rule: a bootloop must leave a flash window */
    vTaskDelay(pdMS_TO_TICKS(SM_BOOT_GRACE_MS));
    sm_policy_init(&s_policy);
    sm_critical_init(&s_crit);
    sm_activity_init(&s_activity);
    ESP_LOGI(TAG, "armed (sleep %.2f V, wake %.2f V, wake after %lu ms, "
             "delay %lu min, periodic %s/%lu min)", cfg->sleep_v, cfg->wake_v,
             (unsigned long)cfg->wake_hold_ms,
             (unsigned long)(cfg->delay_ms / 60000),
             cfg->periodic ? "on" : "off",
             (unsigned long)(cfg->interval_ms / 60000));

    bool guard_tripped = false;

    while (true)
    {
        float volts;
        /* Napping (or about to): the sampler task's cache goes minutes
         * stale because the RTOS tick barely advances across manual
         * light sleep, read the ADC fresh, like legacy's in-loop read
         * (the frozen cache held wake off for good, 2026-07-21). */
        bool in_sleep_states =
            s_policy.state == SLEEP_MANAGER_SLEEPING ||
            s_policy.state == SLEEP_MANAGER_WAKE_PENDING;
        bool have_v = (in_sleep_states
                           ? battery_monitor_read_now(&volts)
                           : battery_monitor_voltage(&volts)) == ESP_OK;
        uint32_t now = sm_now_ms();

        if (have_v)
        {
            s_status.voltage = volts;

            if (volts >= cfg->sleep_v)
            {
                dev_status_manager_set(DEV_STATUS_BIT_WAKE_VOLTAGE_OK);
            }
            else
            {
                dev_status_manager_clear(
                    DEV_STATUS_BIT_WAKE_VOLTAGE_OK);
            }
        }

        /* bench hook: forced entry, timed wake, voltage ignored */
        if (s_test_wake_s != 0)
        {
            uint32_t naps = (s_test_wake_s * 1000u) / (SM_NAP_US / 1000u);

            enter_sleep_sequence(have_v ? volts : 0.0f);
            s_status.state = SLEEP_MANAGER_SLEEPING;

            for (uint32_t i = 0; i < naps; i++)
            {
                nap_and_verify();
            }

            wake_reboot("test timer", /* stands in for a voltage wake */
                        RESTART_TRACKER_PLANNED_REASON_POWER_WAKE);
        }

        /* LAST-LINE battery defense (legacy parity, meatpi 2026-07-21):
         * evaluated EVERY loop and INDEPENDENT of the sleep-enabled
         * setting, a crash-looping device on a sagging battery parks
         * itself no matter what. On trip the SLEEPING policy takes
         * over (voltage-recovery wake + periodic check-ins apply). */
        if (have_v && !guard_tripped && bootloop_guard_trips(volts))
        {
            guard_tripped = true;
            ESP_LOGE(TAG, "boot-loop guard: %d+ unexpected resets "
                     "below %.2f V, sleeping to protect the battery",
                     3, SM_ERROR_V);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_policy.state = SLEEP_MANAGER_SLEEPING;
            s_policy.t_periodic = now + cfg->interval_ms;
            s_hold_active = false;
            s_activity.active = false;
            xSemaphoreGive(s_lock);
            enter_sleep_sequence(volts);
            s_run = true; /* wake machinery runs even if sleep disabled */
        }

        /* Under the lock: the floor, the ladder and the countdown they
         * publish (a hold request from another task edits the same
         * deadlines). CRITICAL FLOOR (Ali, 2026-10-01): a battery under
         * SM_CRITICAL_V for SM_CRITICAL_DELAY_MS sleeps whatever the
         * setting says and without waiting out the user's delay. Awake
         * states only; the ladder's SLEEPING/WAKE_PENDING rules (wake
         * voltage + hold, no periodic check-in under the floor) take over
         * after the trip. */
        xSemaphoreTake(s_lock, portMAX_DELAY);

        bool floor_trip = have_v && !in_sleep_states &&
                          sm_critical_eval(&s_crit, volts, now);
        sm_action_t act = SM_ACT_NONE;

        if (floor_trip)
        {
            /* due: a status read during the entry sequence says 0 s */
            s_pending = sm_pending_pack(SLEEP_MANAGER_PENDING_CRITICAL, now);
            s_hold_active = false;
            s_activity.active = false;
            s_policy.state = SLEEP_MANAGER_SLEEPING;
            s_policy.t_periodic = now + cfg->interval_ms;
        }
        else
        {
            /* here too: with sleep disabled the floor is the only countdown */
            publish_pending(now);

            if (s_run && have_v)
            {
                act = sm_policy_eval(&s_policy, cfg, volts, now);
                publish_pending(now); /* the ladder may have armed or cleared */
            }
        }

        xSemaphoreGive(s_lock);

        if (floor_trip)
        {
            ESP_LOGW(TAG, "critical battery: %.2f V under %.2f V for %u s; "
                     "sleeping regardless of the setting", volts,
                     SM_CRITICAL_V, SM_CRITICAL_DELAY_MS / 1000u);
            enter_sleep_sequence(volts);
            s_status.state = SLEEP_MANAGER_SLEEPING;
            sm_events_state(state_name(s_policy.state), volts);
            s_run = true; /* wake machinery runs even if sleep disabled */
            continue;
        }

        if (!s_run || !have_v)
        {
            if (in_sleep_states)
            {
                /* one racy/failed ADC read must not park the loop
                 * AWAKE (tick delays run ~40x long here): keep
                 * napping, next cycle rereads */
                nap_and_verify();
            }
            else
            {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }

            continue;
        }

        if (s_status.state != s_policy.state)
        {
            s_status.state = s_policy.state;
            sm_events_state(state_name(s_policy.state), volts);
            ESP_LOGI(TAG, "state: %s (%.2f V)",
                     state_name(s_policy.state), volts);
        }

        switch (act)
        {
            case SM_ACT_ENTER_SLEEP:
                enter_sleep_sequence(volts);
                break;

            case SM_ACT_WAKE_REBOOT:
                wake_reboot("voltage recovered",
                            RESTART_TRACKER_PLANNED_REASON_POWER_WAKE);
                break;

            case SM_ACT_PERIODIC_WAKE:
                wake_reboot("periodic check-in",
                            RESTART_TRACKER_PLANNED_REASON_PERIODIC_WAKE);
                break;

            default:
                break;
        }

        if (s_policy.state == SLEEP_MANAGER_SLEEPING)
        {
            nap_and_verify(); /* ~2 s inside light sleep */
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

/* ---- lifecycle -------------------------------------------------------------- */

/* The sleep entry latches the USB rail's enable low with a pad hold
 * (enter_sleep_sequence), a wake is a reboot, and a hold outlives a reset.
 * The OBD sleep pin's hold is released at chip bring-up (obd_pin_wake());
 * this one never was: after the first wake the rail stayed off until the
 * next power cycle, every USB device on the connector unpowered, `usb vbus
 * 1` moving nothing and /api/usb still saying vbus on (bench 2026-10-05:
 * 89 mA awake after a wake against 165 mA after a power cycle; the switch
 * worth 76 mA after a power cycle and 0 after a wake). Give the pin back
 * before anyone drives it: released, the board's pull-up has the rail on,
 * as after a power-on, and usb_host_manager owns it from there. */
static void usb_rail_release(void)
{
    gpio_sleep_set_pull_mode(SM_USB_PWR_GPIO, GPIO_FLOATING);
    gpio_hold_dis(SM_USB_PWR_GPIO);
    gpio_reset_pin(SM_USB_PWR_GPIO);
}

esp_err_t sleep_manager_init(void)
{
    static const log_descriptor_t LOG_DESC =
        { "sleep_manager", ESP_LOG_INFO };

    usb_rail_release(); /* init runs before usb_host_manager starts */

    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }

    log_manager_register(&LOG_DESC);
    sm_events_register();
    return sleep_settings_register();
}

esp_err_t sleep_manager_start(void)
{
    if (!sleep_settings_is_configured())
    {
        ESP_LOGE(TAG, "unconfigured; not starting");
        return ESP_ERR_INVALID_STATE; /* standard §4.3 step 5 */
    }

    /* the task runs even when sleep is DISABLED: it carries the
     * boot-loop battery guard (last-line defense: legacy parity,
     * meatpi 2026-07-21). s_run gates only the voltage-policy part. */
    s_run = sleep_settings_enabled();

    if (!s_run)
    {
        ESP_LOGI(TAG, "disabled in settings (battery guard only)");
    }

    if (s_task == NULL)
    {
        s_task = xTaskCreateStatic(state_task, "sleep_state",
                                   sizeof(s_stack) / sizeof(s_stack[0]),
                                   NULL, 5, s_stack, &s_tcb);

        if (s_task == NULL)
        {
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "started (grace %u s)", SM_BOOT_GRACE_MS / 1000);
    return ESP_OK;
}

esp_err_t sleep_manager_stop(void)
{
    s_run = false;
    return ESP_OK;
}

/* ---- public API --------------------------------------------------------------- */

esp_err_t sleep_manager_status(sleep_manager_status_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out = s_status;
    /* boot-applied fields live with the settings (sleep_manager_settings.c) */
    out->enabled = sleep_settings_enabled();
    out->sleep_v = sleep_settings_config()->sleep_v;
    out->wake_v = sleep_settings_config()->wake_v;

    /* one read of the word: cause and deadline agree; the seconds are
       counted against the clock now, not against the publisher's pass */
    out->pending = sm_pending_unpack(s_pending, sm_now_ms(),
                                     &out->sleep_in_s);
    out->critical_v = SM_CRITICAL_V;
    out->critical_s = SM_CRITICAL_DELAY_MS / 1000u;
    out->hold_s = 0;
    out->holds_max = SM_HOLD_MAX;
    out->holds_left = SM_HOLD_MAX;
    out->activity_s = 0;

    if (s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE)
    {
        uint32_t now = sm_now_ms();
        int32_t left = (int32_t)(s_hold_until - now);
        int32_t act = (int32_t)(s_activity.until - now);

        out->hold_s = (s_hold_active && left > 0)
                          ? ((uint32_t)left + 999u) / 1000u : 0;
        out->holds_left = (uint8_t)(SM_HOLD_MAX - s_holds_used);
        out->activity_s = (s_activity.active && act > 0)
                              ? ((uint32_t)act + 999u) / 1000u : 0;
        xSemaphoreGive(s_lock);
    }

    return ESP_OK;
}

esp_err_t sleep_manager_activity(uint32_t seconds)
{
    if (seconds < 1 || seconds > 600)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_task == NULL || s_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    bool was_active;
    bool counting;
    uint32_t now;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    now = sm_now_ms();
    was_active = s_activity.active;
    counting = sm_activity_apply(&s_policy, s_run, &s_crit, &s_activity, now,
                                 seconds * 1000u, SM_ACTIVITY_CAP_MS);

    if (counting)
    {
        publish_pending(now);
    }

    xSemaphoreGive(s_lock);

    if (!counting)
    {
        return ESP_ERR_INVALID_STATE; /* nothing counts: nothing to hold */
    }

    if (!was_active)
    {
        /* once per countdown: the reads come every second during a backfill */
        ESP_LOGI(TAG, "the log is being read while a countdown runs: held "
                 "awake %lu s at a time, %u min at most",
                 (unsigned long)seconds, SM_ACTIVITY_CAP_MS / 60000u);
    }

    return ESP_OK;
}

esp_err_t sleep_manager_set_prepare_cb(sleep_manager_prepare_cb_t cb)
{
    s_prepare_cb = cb;
    return ESP_OK;
}

esp_err_t sleep_manager_hold(uint32_t minutes)
{
    if (minutes < SM_HOLD_MIN_MIN || minutes > SM_HOLD_MAX_MIN)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_task == NULL || s_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_OK;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (s_holds_used >= SM_HOLD_MAX)
    {
        err = ESP_ERR_NOT_ALLOWED;
    }
    else
    {
        uint32_t now = sm_now_ms();

        if (!sm_hold_apply(&s_policy, s_run, &s_crit, now, minutes * 60000u))
        {
            err = ESP_ERR_INVALID_STATE; /* nothing is counting */
        }
        else
        {
            s_holds_used++;
            s_hold_active = true;
            s_hold_until = now + minutes * 60000u;
            publish_pending(now);
        }
    }

    xSemaphoreGive(s_lock);

    if (err == ESP_OK)
    {
        sleep_manager_status_t st;

        (void)sleep_manager_status(&st);
        ESP_LOGI(TAG, "held awake %lu min on request (%u of %u this boot): "
                 "sleeps in %lu s", (unsigned long)minutes,
                 (unsigned)s_holds_used, (unsigned)SM_HOLD_MAX,
                 (unsigned long)st.sleep_in_s);
    }

    return err;
}

esp_err_t sleep_manager_test_sleep(uint32_t wake_after_s)
{
    if (wake_after_s < 4 || wake_after_s > 600)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_task == NULL)
    {
        return ESP_ERR_INVALID_STATE; /* needs the (enabled) task */
    }

    ESP_LOGW(TAG, "TEST sleep: entering for ~%lu s",
             (unsigned long)wake_after_s);
    s_test_wake_s = wake_after_s;
    return ESP_OK;
}
