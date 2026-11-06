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
 * @file imu_manager.c
 * @brief ICM-42670 lifecycle: chip bring-up (vendored espressif driver on
 *        the shared i2c_bus), BOTH motion detectors configured via the
 *        driver's raw register access (it has no WoM/APEX API), the
 *        polling detector task, and event fan-out.
 *
 * Both detectors run side by side (meatpi 2026-07-04): WoM = per-sample
 * threshold with PREVIOUS-sample reference (differential, an initial-
 * sample reference latches forever after an orientation change), catches
 * door slams/knocks/vibration and is PUBLISHED as an event (throttled).
 * SMD = the DMP/APEX algorithm needing SUSTAINED motion, drives the
 * activity state ("vehicle is moving"). SMD recipe (DS-000451 v1.0):
 * accel on -> DMP SRAM reset -> DMP_INIT_EN (self-clears; other APEX
 * bits ignored while set) -> sensitivity in MREG1 APEX_CONFIG9 ->
 * SMD_ENABLE + DMP_ODR in APEX_CONFIG1. INT_STATUS2 (read-clear)
 * demuxes which detector fired.
 *
 * Settings live in imu_manager_settings.c (standard §4.1).
 */
#include "imu_manager.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "icm42670.h"

#include "dev_status_manager.h"
#include "i2c_bus.h"
#include "log_manager.h"

#include "imu_manager_private.h"

static const char *TAG = "imu_manager";

/* bank-0 registers the vendored driver doesn't name */
#define ICM42670_REG_MCLK_RDY    0x00 /* bit 3: the internal clock runs  */
#define ICM42670_REG_INT_CONFIG  0x06
#define ICM42670_REG_ACCEL_CFG0  0x21
#define ICM42670_REG_INT_STATUS2 0x3B /* R/C: SMD_INT bit3, WOM bits 2:0 */
#define ICM42670_REG_WHO_AM_I    0x75

#define MCLK_RDY_RUNNING  0x08 /* MCLK_RDY<3>                            */
#define PWR_MGMT0_IDLE    0x10 /* keep the RC oscillator on with the
                                  sensors off or in LP on the WU osc     */

/* MREG1 */
#define ICM42670_MREG1_APEX_CONFIG9 0x48 /* SMD_SENSITIVITY_SEL [3:1]    */

#define INT_CONFIG_INT1_PULSED_PP_HIGH 0x03 /* active-high, push-pull    */
#define INT_SOURCE1_SMD                0x08 /* SMD -> INT1               */
#define INT_SOURCE1_WOM_XYZ            0x07 /* WoM X|Y|Z -> INT1         */
#define APEX_CONFIG0_DMP_MEM_RESET     0x01
#define APEX_CONFIG0_DMP_INIT          0x04
#define APEX_CONFIG1_SMD_EN_ODR_50HZ   0x42 /* SMD_ENABLE | DMP_ODR=50  */
#define APEX_CONFIG1_ODR_50HZ          0x02 /* DMP_ODR=50, nothing on   */
#define WOM_CONFIG_EN_OR_PREV_FIRST    0x03 /* EN, PREVIOUS ref, OR, 1st */
#define INT_STATUS2_SMD_INT            0x08
#define INT_STATUS2_WOM_MASK           0x07 /* bit2=X bit1=Y bit0=Z      */
#define APEX_CONFIG9_SMD_SENS_MASK     0x0E /* SMD_SENSITIVITY_SEL [3:1] */

#define IMU_TICK_MS        200
#define IMU_WOM_PUB_GAP_MS 500 /* WOM event publish throttle             */

static icm42670_handle_t s_dev;
static TaskHandle_t s_task;
static StaticTask_t s_tcb;                            /* internal: FreeRTOS */
static StackType_t s_stack[3072] EXT_RAM_BSS_ATTR;
static imu_policy_t s_policy;
static volatile imu_manager_activity_t s_activity = IMU_MANAGER_UNKNOWN;
static bool s_started;
static uint8_t s_wom_thr_chip[3]; /* the thresholds read back at start   */

/* event subscribers (fan-out under s_sub_lock; publish from the task) */
static QueueHandle_t s_subs[IMU_MANAGER_MAX_SUBSCRIBERS];
static SemaphoreHandle_t s_sub_lock;
static StaticSemaphore_t s_sub_lock_buf; /* internal: FreeRTOS object */
static uint32_t s_pub_drops;

/* ---- detector task (POLLING: meatpi's call: nothing here is time-
 * critical, and polling avoids ISR complexity entirely. INT_STATUS2
 * LATCHES the WOM/SMD bits until read, so a 200 ms poll misses nothing;
 * worst-case detection latency is one tick.) ---------------------------------- */

/** Fan one event out to every subscriber; zero-timeout, drop-and-count. */
static void publish(imu_manager_event_type_t type, uint8_t wom_axes,
                    uint32_t now)
{
    imu_manager_event_t evt =
        { .type = type, .wom_axes = wom_axes, .uptime_ms = now };

    xSemaphoreTake(s_sub_lock, portMAX_DELAY);

    for (int i = 0; i < IMU_MANAGER_MAX_SUBSCRIBERS; i++)
    {
        if (s_subs[i] != NULL &&
            xQueueSend(s_subs[i], &evt, 0) != pdTRUE)
        {
            s_pub_drops++;

            if ((s_pub_drops % 100) == 1)
            {
                ESP_LOGW(TAG, "subscriber full (%lu event drops)",
                         (unsigned long)s_pub_drops);
            }
        }
    }

    xSemaphoreGive(s_sub_lock);
}

static void detector_task(void *arg)
{
    (void)arg;

    imu_policy_init(&s_policy, imu_settings_config()->stationary_ms,
                    IMU_WOM_PUB_GAP_MS, (uint32_t)(esp_log_timestamp()));
    s_activity = IMU_MANAGER_STATIONARY;

    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(IMU_TICK_MS));

        /* poll + demux: INT_STATUS2 latches SMD + per-axis WOM bits
         * since the last read and clears on read */
        bool smd = false;
        uint8_t wom_axes = 0;
        uint8_t status = 0;
        uint32_t now = esp_log_timestamp();

        if (icm42670_read_register(s_dev, ICM42670_REG_INT_STATUS2,
                                   &status) == ESP_OK)
        {
            smd = (status & INT_STATUS2_SMD_INT) != 0;
            wom_axes = status & INT_STATUS2_WOM_MASK;
        }

        if (wom_axes != 0 && imu_policy_wom_gate(&s_policy, now))
        {
            ESP_LOGD(TAG, "WOM event (axes 0x%02x)", wom_axes);
            publish(IMU_MANAGER_EVENT_WOM, wom_axes, now);
        }

        if (smd)
        {
            ESP_LOGD(TAG, "SMD event");
            publish(IMU_MANAGER_EVENT_SMD, 0, now);
        }

        /* the activity state is SMD-driven only: loading the trunk
         * (bumps) is not "the vehicle is moving" */
        int state = smd ? imu_policy_on_motion(&s_policy, now)
                        : imu_policy_on_tick(&s_policy, now);
        imu_manager_activity_t next =
            state ? IMU_MANAGER_ACTIVE : IMU_MANAGER_STATIONARY;

        if (next != s_activity)
        {
            s_activity = next;

            if (next == IMU_MANAGER_ACTIVE)
            {
                dev_status_manager_set(DEV_STATUS_BIT_MOTION);
            }
            else
            {
                dev_status_manager_clear(DEV_STATUS_BIT_MOTION);
            }

            publish((next == IMU_MANAGER_ACTIVE) ? IMU_MANAGER_EVENT_ACTIVE
                                                 : IMU_MANAGER_EVENT_STATIONARY,
                    0, now);
            ESP_LOGI(TAG, "activity: %s",
                     (next == IMU_MANAGER_ACTIVE) ? "ACTIVE" : "STATIONARY");
        }
    }
}

/* ---- detector bring-up (DS-000451) -------------------------------------------- */

/* Keep the chip's internal clock (MCLK) running: PWR_MGMT0.IDLE powers
 * the RC oscillator on whatever the sensor modes, then MCLK_RDY<3> says
 * it runs. The MREG banks (the WoM thresholds, the APEX parameters) are
 * reachable only then; the legacy driver refused an MREG access without
 * MCLK_RDY, the InvenSense driver requests the clock around each one.
 * IDLE stays set for the device's life (2026-11-06). */
static esp_err_t mclk_on(void)
{
    uint8_t pwr = 0;
    esp_err_t err = icm42670_read_register(s_dev, ICM42670_PWR_MGMT0, &pwr);

    if (err == ESP_OK && !(pwr & PWR_MGMT0_IDLE))
    {
        err = icm42670_write_register(s_dev, ICM42670_PWR_MGMT0,
                                      (uint8_t)(pwr | PWR_MGMT0_IDLE));
    }

    for (int i = 0; err == ESP_OK && i < 20; i++)
    {
        uint8_t v = 0;

        err = icm42670_read_register(s_dev, ICM42670_REG_MCLK_RDY, &v);

        if (err == ESP_OK && (v & MCLK_RDY_RUNNING))
        {
            return ESP_OK;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (err == ESP_OK)
    {
        ESP_LOGE(TAG, "internal clock (MCLK) did not start");
        err = ESP_ERR_TIMEOUT;
    }

    return err;
}

static esp_err_t detectors_configure(void)
{
    const imu_settings_t *settings = imu_settings_config();

    /* accel low-power @ 50 Hz feeds both detectors (ACCEL_ODR must be
     * >= the DMP_ODR selected below) */
    const icm42670_cfg_t cfg =
    {
        .acce_fs = ACCE_FS_2G,
        .acce_odr = ACCE_ODR_50HZ,
        .gyro_fs = GYRO_FS_2000DPS,
        .gyro_odr = GYRO_ODR_200HZ,
    };

    esp_err_t err = icm42670_config(s_dev, &cfg);

    if (err == ESP_OK)
    {
        err = icm42670_gyro_set_pwr(s_dev, GYRO_PWR_OFF);
    }

    if (err == ESP_OK)
    {
        err = icm42670_acce_set_pwr(s_dev, ACCE_PWR_LOWPOWER);
        vTaskDelay(pdMS_TO_TICKS(1)); /* no writes for 200 us after OFF -> LP */
    }

    /* --- the internal clock, permanently (2026-11-06): every MREG1
     * access below (the SMD sensitivity, the WoM thresholds) needs MCLK
     * running, and accel LP mode on the wake-up oscillator does not
     * provide it (DS-000451 section 13: LP mode with WUOSC does not
     * support MREG access, "power on the RC oscillator using register
     * field IDLE"). Until this line the threshold writes landed at
     * random: the bench unit held 32/1/1 for a setting of 32, a unit
     * from power-up keeps the reset value 0 on the axes whose write was
     * lost, and imu.bump then fires on every sample (the Nissan Ariya
     * report: axes 7 every 600 ms whatever the threshold). The RC
     * oscillator's current is insignificant here (meatpi): IDLE stays
     * set, which also keeps `imu -x` truthful. ------------------------------- */
    if (err == ESP_OK)
    {
        err = mclk_on();
    }

    /* --- SMD: DMP SRAM reset, then the SW init procedure (both
     * self-clear; all APEX enables are ignored while DMP_INIT_EN is
     * set), then sensitivity + enable ------------------------------------ */
    if (err == ESP_OK && settings->smd)
    {
        err = icm42670_write_register(s_dev, ICM42670_APEX_CONFIG0,
                                      APEX_CONFIG0_DMP_MEM_RESET);

        if (err == ESP_OK)
        {
            vTaskDelay(pdMS_TO_TICKS(2));
            err = icm42670_write_register(s_dev, ICM42670_APEX_CONFIG0,
                                          APEX_CONFIG0_DMP_INIT);
        }

        if (err == ESP_OK)
        {
            uint8_t v = APEX_CONFIG0_DMP_INIT;

            for (int i = 0; i < 20 && (v & APEX_CONFIG0_DMP_INIT); i++)
            {
                vTaskDelay(pdMS_TO_TICKS(5));
                err = icm42670_read_register(s_dev, ICM42670_APEX_CONFIG0,
                                             &v);

                if (err != ESP_OK)
                {
                    break;
                }
            }

            if (err == ESP_OK && (v & APEX_CONFIG0_DMP_INIT))
            {
                ESP_LOGE(TAG, "DMP init did not complete");
                err = ESP_ERR_TIMEOUT;
            }
        }

        /* robustness knob: 0 = highest detection rate, 4 = fewest false
         * positives (datasheet: high values hurt transport use cases) */
        if (err == ESP_OK)
        {
            uint8_t cfg9 = 0;

            err = icm42670_read_mreg_register(s_dev, 1,
                                              ICM42670_MREG1_APEX_CONFIG9,
                                              &cfg9);

            if (err == ESP_OK)
            {
                cfg9 = (uint8_t)((cfg9 & ~APEX_CONFIG9_SMD_SENS_MASK) |
                                 ((settings->smd_sensitivity << 1) &
                                  APEX_CONFIG9_SMD_SENS_MASK));
                err = icm42670_write_mreg_register(
                    s_dev, 1, ICM42670_MREG1_APEX_CONFIG9, cfg9);
            }
        }
    }

    if (err == ESP_OK)
    {
        err = icm42670_write_register(s_dev, ICM42670_APEX_CONFIG1,
                                      settings->smd
                                          ? APEX_CONFIG1_SMD_EN_ODR_50HZ
                                          : APEX_CONFIG1_ODR_50HZ);
    }

    /* --- WoM: per-axis thresholds (MREG bank 1), PREVIOUS-sample
     * reference so an orientation change can't latch it. Off first:
     * WOM_MODE/INT_MODE/INT_DUR cannot change while WOM_EN is set (a
     * soft reboot finds it set) and the thresholds belong before the
     * enable. Every threshold is read back: a mismatch leaves WoM OFF
     * (a chip at the reset value 0 fires on every sample) and fails the
     * start. ------------------------------------------------------------------ */
    if (err == ESP_OK)
    {
        err = icm42670_write_register(s_dev, ICM42670_WOM_CONFIG, 0x00);
    }

    if (err == ESP_OK && settings->wom)
    {
        for (int i = 0; err == ESP_OK && i < 3; i++)
        {
            uint8_t reg = (uint8_t)(ICM42670_MREG1_ACCEL_WOM_X_THR + i);
            uint8_t back = 0;

            err = icm42670_write_mreg_register(s_dev, 1, reg,
                                               settings->wom_threshold);

            if (err == ESP_OK)
            {
                err = icm42670_read_mreg_register(s_dev, 1, reg, &back);
            }

            if (err == ESP_OK && back != settings->wom_threshold)
            {
                ESP_LOGE(TAG, "WoM threshold %c: wrote %u, the chip holds "
                         "%u; WoM stays off", "XYZ"[i],
                         settings->wom_threshold, back);
                err = ESP_ERR_INVALID_RESPONSE;
            }

            s_wom_thr_chip[i] = back;
        }

        if (err == ESP_OK)
        {
            err = icm42670_write_register(s_dev, ICM42670_WOM_CONFIG,
                                          WOM_CONFIG_EN_OR_PREV_FIRST);
        }
    }

    /* --- INT1 routing: kept so status latching matches the verified
     * config; the pin itself goes unlistened (we poll INT_STATUS2) ---------- */
    if (err == ESP_OK)
    {
        err = icm42670_write_register(s_dev, ICM42670_REG_INT_CONFIG,
                                      INT_CONFIG_INT1_PULSED_PP_HIGH);
    }

    if (err == ESP_OK)
    {
        uint8_t sources =
            (uint8_t)((settings->smd ? INT_SOURCE1_SMD : 0) |
                      (settings->wom ? INT_SOURCE1_WOM_XYZ : 0));

        err = icm42670_write_register(s_dev, ICM42670_INT_SOURCE1,
                                      sources);
    }

    return err;
}

/* ---- lifecycle -------------------------------------------------------------- */

esp_err_t imu_manager_init(void)
{
    static const log_descriptor_t LOG_DESC =
        { "imu_manager", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);

    if (s_sub_lock == NULL)
    {
        s_sub_lock = xSemaphoreCreateMutexStatic(&s_sub_lock_buf);
    }

    imu_events_register();
    return imu_settings_register();
}

esp_err_t imu_manager_start(void)
{
    const imu_settings_t *settings = imu_settings_config();

    if (!imu_settings_is_configured())
    {
        ESP_LOGE(TAG, "unconfigured; not starting");
        return ESP_ERR_INVALID_STATE; /* standard §4.3 step 5 */
    }

    if (s_started)
    {
        return ESP_OK;
    }

    if (!settings->enabled)
    {
        ESP_LOGI(TAG, "disabled in settings");
        return ESP_OK;
    }

    esp_err_t err = icm42670_create(i2c_bus_handle(),
                                    ICM42670_I2C_ADDRESS, &s_dev);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "ICM-42670 not found: %s", esp_err_to_name(err));
        return err;
    }

    err = detectors_configure();

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "detector config failed: %s", esp_err_to_name(err));
        return err;
    }

    /* drop whatever INT_STATUS2 latched before this boot (a soft reboot
     * does not reset the chip): without this read the first poll
     * published a stale bump 0.4 s after every such start (2026-11-06) */
    uint8_t stale = 0;

    (void)icm42670_read_register(s_dev, ICM42670_REG_INT_STATUS2, &stale);

    s_task = xTaskCreateStatic(detector_task, "imu_detector",
                               sizeof(s_stack) / sizeof(s_stack[0]),
                               NULL, 5, s_stack, &s_tcb);

    if (s_task == NULL)
    {
        return ESP_FAIL;
    }

    s_started = true;
    ESP_LOGI(TAG, "started (SMD %s sens %u; WoM %s thr %u ~%umg, chip "
             "%u/%u/%u; stationary after %lus; polling %d ms)",
             settings->smd ? "on" : "off", settings->smd_sensitivity,
             settings->wom ? "on" : "off", settings->wom_threshold,
             (unsigned)(settings->wom_threshold * 39 / 10),
             s_wom_thr_chip[0], s_wom_thr_chip[1], s_wom_thr_chip[2],
             (unsigned long)(settings->stationary_ms / 1000), IMU_TICK_MS);
    imu_events_start();
    return ESP_OK;
}

esp_err_t imu_manager_stop(void)
{
    s_started = false;
    return ESP_OK;
}

/* ---- reads --------------------------------------------------------------------- */

imu_manager_activity_t imu_manager_activity(void)
{
    return s_started ? s_activity : IMU_MANAGER_UNKNOWN;
}

esp_err_t imu_manager_subscribe(QueueHandle_t q)
{
    if (q == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_ERR_NO_MEM;

    xSemaphoreTake(s_sub_lock, portMAX_DELAY);

    for (int i = 0; i < IMU_MANAGER_MAX_SUBSCRIBERS; i++)
    {
        if (s_subs[i] == q)
        {
            err = ESP_OK; /* already attached */
            break;
        }

        if (s_subs[i] == NULL && err == ESP_ERR_NO_MEM)
        {
            s_subs[i] = q;
            err = ESP_OK;
            break;
        }
    }

    xSemaphoreGive(s_sub_lock);
    return err;
}

esp_err_t imu_manager_unsubscribe(QueueHandle_t q)
{
    esp_err_t err = ESP_ERR_NOT_FOUND;

    xSemaphoreTake(s_sub_lock, portMAX_DELAY);

    for (int i = 0; i < IMU_MANAGER_MAX_SUBSCRIBERS; i++)
    {
        if (s_subs[i] == q)
        {
            s_subs[i] = NULL;
            err = ESP_OK;
        }
    }

    xSemaphoreGive(s_sub_lock);
    return err;
}

esp_err_t imu_manager_read_accel(float *ax, float *ay, float *az)
{
    if (s_dev == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    icm42670_value_t v;
    esp_err_t err = icm42670_get_acce_value(s_dev, &v);

    if (err == ESP_OK)
    {
        *ax = v.x;
        *ay = v.y;
        *az = v.z;
    }

    return err;
}

esp_err_t imu_manager_read_temp(float *temp)
{
    if (s_dev == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    return icm42670_get_temp_value(s_dev, temp);
}

esp_err_t imu_manager_device_id(uint8_t *id)
{
    if (s_dev == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    return icm42670_get_deviceid(s_dev, id);
}

esp_err_t imu_manager_read_regs(imu_manager_regs_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_dev == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    memset(out, 0, sizeof(*out));

    const struct { uint8_t reg; uint8_t *dst; } BANK0[] =
    {
        { ICM42670_REG_WHO_AM_I,   &out->who_am_i      },
        { ICM42670_REG_MCLK_RDY,   &out->mclk_rdy      },
        { ICM42670_REG_INT_CONFIG, &out->int_config    },
        { ICM42670_PWR_MGMT0,      &out->pwr_mgmt0     },
        { ICM42670_REG_ACCEL_CFG0, &out->accel_config0 },
        { ICM42670_APEX_CONFIG0,   &out->apex_config0  },
        { ICM42670_APEX_CONFIG1,   &out->apex_config1  },
        { ICM42670_WOM_CONFIG,     &out->wom_config    },
        { ICM42670_INT_SOURCE1,    &out->int_source1   },
    };
    esp_err_t err = ESP_OK;

    for (size_t i = 0; err == ESP_OK && i < sizeof(BANK0) / sizeof(BANK0[0]);
         i++)
    {
        err = icm42670_read_register(s_dev, BANK0[i].reg, BANK0[i].dst);
    }

    /* MREG1: truthful only while MCLK runs (mclk_on() at start keeps it
     * so; the caller reads out->mclk_rdy to know) */
    for (int i = 0; err == ESP_OK && i < 3; i++)
    {
        err = icm42670_read_mreg_register(
            s_dev, 1, (uint8_t)(ICM42670_MREG1_ACCEL_WOM_X_THR + i),
            &out->wom_thr[i]);
    }

    if (err == ESP_OK)
    {
        err = icm42670_read_mreg_register(s_dev, 1, ICM42670_MREG1_APEX_CONFIG9,
                                          &out->apex_config9);
    }

    return err;
}
