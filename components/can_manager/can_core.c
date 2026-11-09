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
 * @file can_core.c
 * @brief Shared CAN bus manager for the ELM327 emulator.
 *
 * Wraps the ESP-IDF TWAI (Two-Wire Automotive Interface) driver and provides
 * multi-client frame dispatch so that multiple AT engine instances can share
 * one physical CAN bus.
 *
 * Architecture
 * ------------
 * - One FreeRTOS task runs per can_core_handle_t to service the TWAI RX
 *   queue (on-device only).
 * - Each received frame is compared against every registered client's filter
 *   and mask.  Matching clients have their rx_cb invoked from the RX task.
 * - TX is performed synchronously from the caller's context.
 *
 * Host (non-ESP) build
 * --------------------
 * When built outside the ESP-IDF (e.g. for pytest / unit tests) the TWAI
 * driver calls are stubbed out so that the logic can be exercised without
 * real hardware.  The RX task is replaced by a no-op.
 */

#include <string.h>
#include <stdlib.h>
#include "can_core.h"
#include "can_core_filter.h"
#include "can_core_driver.h"
#include "can_core_private.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "can_core";

/* Deinit wakes can_rx_task with CAN_RX_RAW_EVT, then blocks on this
 * until the task has stored rx_exited and given it. A spin would starve
 * the RX task when deinit runs at equal or higher priority. */
static StaticSemaphore_t s_rx_exit_sem_buf;
static SemaphoreHandle_t s_rx_exit_sem;

/* esp_timer us immediately before the RX task's vTaskDelete(NULL).
 * can_core_init memsets the handle, so this cannot live there. The
 * 20 ms static-TCB settle is paid on the next start, and only for
 * however much of it is still left. */
static int64_t s_rx_deleted_us;

/* RX task stack size (bytes) */
#define CAN_CORE_RX_TASK_STACK   4096
#define CAN_CORE_RX_TASK_PRIO    10

static bool can_subscription_matches_frame(uint32_t filter,
                                           uint32_t mask,
                                           bool ext,
                                           bool monitor_all,
                                           const can_core_frame_t *frame);
static bool dispatch_frame_to_queue_subscriber(QueueHandle_t queue,
                                               const can_core_frame_t *frame);

/* -------------------------------------------------------------------------
 * Bus-off recovery servicing (runs in the RX task loop)
 *
 * Node-API flow (esp_driver_twai, 2026-07-21): on_state_change(BUS_OFF)
 * sets can_drv_evt_bus_off -> this loop calls twai_node_recover() (completes
 * once the bus shows 128 x 11 recessive bits; the node REJOINS on its
 * own) -> on_state_change(ERR_ACTIVE) sets can_drv_evt_recovered -> the
 * restart branch is bookkeeping + the policy backoff (thrash guard,
 * see can_core_recovery.h). CANREC-verified at 0.1 s (legacy driver's
 * initiate/stop/start dance took 0.6 s).
 * ------------------------------------------------------------------------- */
static void can_service_recovery(can_core_handle_t *handle)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    if (can_drv_evt_bus_off)
    {
        can_drv_evt_bus_off = false;
        handle->stats.bus_off_count++;

        uint32_t backoff =
            can_core_recovery_on_bus_off(&handle->recovery, now_ms);

        ESP_LOGE(TAG, "BUS-OFF #%lu -- initiating recovery "
                 "(restart backoff %lu ms)",
                 (unsigned long)handle->recovery.off_count,
                 (unsigned long)backoff);
        (void)twai_node_recover(can_drv_node);
    }

    if (can_drv_evt_recovered)
    {
        can_drv_evt_recovered = false;
        can_core_recovery_on_recovered(&handle->recovery, now_ms);
        ESP_LOGW(TAG, "bus recovered -- restarting in %lu ms",
                 (unsigned long)handle->recovery.backoff_ms);
    }

    if (can_drv_evt_storm)
    {
        /* the ISR masked the controller's interrupts (can_core_driver.c):
           the node is deaf until the link policy bounces it, which the RX
           line look makes it do within a few looks */
        can_drv_evt_storm = false;
        ESP_LOGW(TAG, "receive error storm at %lu kbit/s (%lu errors so far): "
                      "the controller's interrupts are masked until the node "
                      "is restarted",
                 (unsigned long)can_ab_baud(&handle->ab),
                 (unsigned long)handle->stats.rx_bad);
    }

    if (can_core_recovery_restart_due(&handle->recovery, now_ms))
    {
        /* node API: recovery re-joins the bus by itself once the 128
         * bus-idle sequences pass -- the legacy twai_start became
         * bookkeeping (the CANREC backoff geometry is preserved) */
        handle->stats.recovery_count++;
        ESP_LOGW(TAG, "bus restarted after bus-off (recovery #%lu)",
                 (unsigned long)handle->stats.recovery_count);
    }
}

/* -------------------------------------------------------------------------
 * Internal RX task
 * Runs until handle->initialised is cleared by can_core_deinit().
 * ------------------------------------------------------------------------- */
static void can_rx_task(void *arg)
{
    can_core_handle_t *handle = (can_core_handle_t *)arg;

    while (handle->initialised)
    {
        if (handle->reconfiguring)
        {
            /* rx_parked is the teardown rendezvous: the driver may only
             * be uninstalled while this task is provably parked here,
             * a twai_receive blocked on the RX queue when the queue is
             * deleted dies on its spinlock (hit at sleep entry
             * 2026-07-20) */
            handle->rx_parked = true;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        handle->rx_parked = false;

        can_service_recovery(handle);

        /* listen before talk: the bitrate / mode policy, which may
         * bounce the node right here (this task is outside a receive) */
        can_link_service(handle);

        can_rx_raw_t raw;

        /* OUR queue (static, never deleted) -- slices of at most 100 ms
         * so the initialised flag stays responsive, shorter while the
         * link policy has no verdict yet */
        if (xQueueReceive(can_drv_rx_q, &raw,
                          pdMS_TO_TICKS(can_link_poll_ms(handle))) != pdTRUE)
        {
            continue;
        }

        if (raw.flags & CAN_RX_RAW_EVT)
        {
            continue; /* a wake-up for the link policy above, not a frame */
        }

        handle->stats.rx_count++;
        handle->link_frames++;

        /* Build our frame structure */
        can_core_frame_t frame;
        frame.id  = raw.id;
        frame.ext = (raw.flags & CAN_RX_RAW_EXT) != 0;
        frame.rtr = (raw.flags & CAN_RX_RAW_RTR) != 0;
        frame.dlc = raw.dlc;
        frame.ts_us = raw.ts_us;
        memset(frame.data, 0, sizeof(frame.data));
        memcpy(frame.data, raw.data, frame.dlc);

        /* Dispatch to matching clients */
        for (int i = 0; i < CAN_CORE_MAX_CLIENTS; i++)
        {
            can_core_client_t *client = &handle->clients[i];
            if (!client->active || !client->rx_cb)
            {
                continue;
            }
            if (can_subscription_matches_frame(client->filter,
                                               client->mask,
                                               client->ext,
                                               client->monitor_all,
                                               &frame))
            {
                client->rx_cb(client->rx_ctx, &frame);
            }
        }

        for (int i = 0; i < CAN_CORE_MAX_QUEUE_SUBSCRIBERS; i++)
        {
            can_core_queue_subscriber_t *subscriber =
                &handle->queue_subscribers[i];

            if (!subscriber->active || subscriber->queue == NULL)
            {
                continue;
            }

            if (can_subscription_matches_frame(subscriber->filter,
                                               subscriber->mask,
                                               subscriber->ext,
                                               subscriber->monitor_all,
                                               &frame))
            {
                if (!dispatch_frame_to_queue_subscriber(subscriber->queue,
                                                        &frame))
                {
                    handle->stats.dispatch_drops++;
                    subscriber->drops++;
                }
            }
        }
    }

    handle->rx_exited = true; /* deinit may stop the driver now */
    if (s_rx_exit_sem != NULL)
    {
        (void)xSemaphoreGive(s_rx_exit_sem);
    }
    s_rx_deleted_us = esp_timer_get_time();
    vTaskDelete(NULL);
}

#endif /* ESP_PLATFORM */

/* =========================================================================
 * Public API implementation
 * ========================================================================= */

/* -------------------------------------------------------------------------
 * can_core_init
 * ------------------------------------------------------------------------- */
elm327_err_t can_core_init(can_core_handle_t *handle,
                              const can_core_config_t *config)
{
    if (!handle || !config)
    {
        return ELM327_ERR_INVALID_ARG;
    }

    memset(handle, 0, sizeof(*handle));
    handle->config = *config;

    handle->initialised = true;

#ifdef ESP_PLATFORM
    can_drv_rx_reset();
    can_core_reset_stats(handle); /* stats count from this init */

    /* the node starts listen-only and without its TX pin, whatever the
     * settings ask for: can_core_link.c promotes it when it may talk */
    elm327_err_t err = can_link_start(handle);
    if (err != ELM327_OK)
    {
        handle->initialised = false;
        return err;
    }

    /* Start the RX dispatch task. WiCAN: PSRAM stack (the task does
     * twai_receive + dispatch only, never flash) + internal TCB,
     * keeps the scarce internal heap for NVS-touching tasks. */
    static StaticTask_t s_rx_tcb;
    static EXT_RAM_BSS_ATTR StackType_t
        s_rx_stack[CAN_CORE_RX_TASK_STACK];

    if (s_rx_exit_sem == NULL)
    {
        s_rx_exit_sem = xSemaphoreCreateBinaryStatic(&s_rx_exit_sem_buf);
    }
    else
    {
        while (xSemaphoreTake(s_rx_exit_sem, 0) == pdTRUE)
        {
        }
    }

    /* s_rx_tcb is static. Reuse it only after the previous task's
     * vTaskDelete has had 20 ms. can_stop must not pay this: the pin
     * is already released, and a STIL retry starts hundreds of ms later. */
    if (s_rx_deleted_us != 0)
    {
        int64_t remain_us = 20000 - (esp_timer_get_time() - s_rx_deleted_us);
        if (remain_us > 0)
        {
            TickType_t ticks = pdMS_TO_TICKS((uint32_t)((remain_us + 999) / 1000));
            if (ticks == 0)
            {
                ticks = 1;
            }
            vTaskDelay(ticks);
        }
    }

    TaskHandle_t task_hdl =
        xTaskCreateStaticPinnedToCore(can_rx_task, "can_core_rx",
                                      CAN_CORE_RX_TASK_STACK, handle,
                                      CAN_CORE_RX_TASK_PRIO, s_rx_stack,
                                      &s_rx_tcb,
                                      CONFIG_WICAN_CAN_ISR_CORE);
    if (task_hdl == NULL)
    {
        ESP_LOGE(TAG, "rx task create failed");
        handle->initialised = false;
        can_drv_stop(handle);
        return ELM327_ERR_CAN;
    }
    handle->rx_task_handle = (void *)task_hdl;

#else
    /* Host stub */
#endif

    return ELM327_OK;
}

/* -------------------------------------------------------------------------
 * can_core_deinit
 * ------------------------------------------------------------------------- */
void can_core_deinit(can_core_handle_t *handle)
{
    if (!handle || !handle->initialised)
    {
        return;
    }

    handle->initialised = false;

#ifdef ESP_PLATFORM
    can_link_tx_close(handle); /* no transmit inside the driver below */

    /* The RX task blocks in xQueueReceive(can_drv_rx_q), a static queue
     * that is never deleted (up to CAN_AB_WATCH_MS while the link is up).
     * A CAN_RX_RAW_EVT wakes that receive. If the queue is full the task
     * is not blocked and sees initialised on its next loop; do not drop
     * a frame to make room. Then wait until it has stored rx_exited and
     * given s_rx_exit_sem. Taking the semaphore yields, so a deinit at
     * equal or higher priority cannot starve can_rx_task. If rx_exited
     * is already set, the give already happened: take it without waiting
     * for another one. Do not vTaskDelay here. The 20 ms static-TCB
     * settle lives in can_core_init, before s_rx_tcb is reused. The
     * 2026-07-20 crash was can_drv_stop while a receive still blocked
     * on the queue the driver was about to delete, so can_drv_stop runs
     * only after rx_exited, or after this 1 s hang timeout. */
    if (can_drv_rx_q != NULL)
    {
        can_rx_raw_t wake;
        memset(&wake, 0, sizeof(wake));
        wake.flags = CAN_RX_RAW_EVT;
        (void)xQueueSend(can_drv_rx_q, &wake, 0);
    }

    if (handle->rx_exited)
    {
        if (s_rx_exit_sem != NULL &&
            xSemaphoreTake(s_rx_exit_sem, 0) != pdTRUE)
        {
            (void)xSemaphoreTake(s_rx_exit_sem, pdMS_TO_TICKS(1000));
        }
    }
    else if (s_rx_exit_sem == NULL ||
             xSemaphoreTake(s_rx_exit_sem, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        if (!handle->rx_exited)
        {
            ESP_LOGE(TAG, "rx task did not exit; uninstalling anyway");
        }
        else if (s_rx_exit_sem != NULL)
        {
            (void)xSemaphoreTake(s_rx_exit_sem, 0);
        }
    }
    can_drv_stop(handle);
#endif

    memset(handle->clients, 0, sizeof(handle->clients));
    memset(handle->queue_subscribers, 0, sizeof(handle->queue_subscribers));
}

void can_core_quiesce(can_core_handle_t *handle)
{
#ifdef ESP_PLATFORM
    if (handle)
    {
        handle->tx_open = false; /* no wait: the restart path never blocks */
    }

    can_drv_quiesce();
#else
    (void)handle;
#endif
}

/* The client / queue-subscriber tables (register, unregister, filter,
 * name, count) live in can_core_clients.c. */

/* -------------------------------------------------------------------------
 * can_core_transmit
 * ------------------------------------------------------------------------- */
elm327_err_t can_core_transmit(can_core_handle_t *handle,
                                  const can_core_frame_t *frame,
                                  uint32_t timeout_ms)
{
    if (!handle || !frame || !handle->initialised)
    {
        return ELM327_ERR_INVALID_ARG;
    }

#ifdef ESP_PLATFORM
    /* the node talks only once its bitrate is proven (or a fixed-bitrate
     * bus stayed silent): until then, and during a bounce, refuse */
    if (!can_link_tx_enter(handle, timeout_ms))
    {
        return ELM327_ERR_BUSY;
    }

    /* from a slot the driver owns until the frame's tx_done (and retries
       a lost frame from there): can_core_driver.c */
    elm327_err_t rc = can_drv_transmit(frame, timeout_ms);

    can_link_tx_exit(handle);

    if (rc != ELM327_OK)
    {
        handle->stats.tx_errors++;
        return rc;
    }

    handle->stats.tx_count++;
#else
    /* Host stub: pretend TX succeeded */
    (void)timeout_ms;
    handle->stats.tx_count++;
#endif

    return ELM327_OK;
}

/* -------------------------------------------------------------------------
 * can_core_get_stats
 * ------------------------------------------------------------------------- */
void can_core_get_stats(can_core_handle_t *handle,
                           can_core_stats_t *stats)
{
    if (!handle || !stats)
    {
        return;
    }

#ifdef ESP_PLATFORM
    /* the ISR counts in internal RAM (it must not touch this handle: it
     * lives in PSRAM): fold its counters in here, in task context */
    can_drv_counters_t isr;

    can_drv_counters(&isr);
    handle->stats.rx_missed = isr.rx_missed - handle->isr_base_rx_missed;
    handle->stats.rx_overrun = isr.rx_overrun - handle->isr_base_rx_overrun;
    handle->stats.rx_bad = isr.rx_bad - handle->isr_base_rx_bad;
    handle->stats.arb_lost = isr.arb_lost - handle->isr_base_arb_lost;
    handle->stats.err_stuff = isr.err_stuff - handle->isr_base_err[0];
    handle->stats.err_form = isr.err_form - handle->isr_base_err[1];
    handle->stats.err_bit = isr.err_bit - handle->isr_base_err[2];
    handle->stats.err_ack = isr.err_ack - handle->isr_base_err[3];
    handle->stats.err_other = isr.err_other - handle->isr_base_err[4];
    handle->stats.tx_retries = isr.tx_retries - handle->isr_base_tx[0];
    handle->stats.tx_lost = isr.tx_lost - handle->isr_base_tx[1];
    handle->stats.tx_done = isr.tx_done - handle->isr_base_tx[2];
    handle->stats.rx_storms = isr.rx_storms - handle->isr_base_storms;

    twai_node_status_t info;
    twai_node_record_t rec;
    /* under the node lock: the link policy may be bouncing the node on
       the RX task this very moment (can_core_node.c) */
    if (can_drv_node_info(&info, &rec))
    {
        handle->stats.bus_errors = rec.bus_err_num;
        handle->stats.tx_errors  = info.tx_error_count;
        handle->stats.rx_errors  = info.rx_error_count;

        switch (info.state)
        {
            case TWAI_ERROR_BUS_OFF:
                handle->stats.bus_state = CAN_CORE_BUS_OFF;
                break;
            default:
                /* the node stays enabled through recovery -- RECOVERING
                 * is signalled by the pending-restart bookkeeping */
                handle->stats.bus_state = handle->recovery.restart_pending
                                          ? CAN_CORE_BUS_RECOVERING
                                          : CAN_CORE_BUS_RUNNING;
                break;
        }
    }
#else
    handle->stats.bus_state = handle->initialised ? CAN_CORE_BUS_RUNNING
                                                  : CAN_CORE_BUS_STOPPED;
#endif

    *stats = handle->stats;
}

/* -------------------------------------------------------------------------
 * can_core_reset_stats
 * ------------------------------------------------------------------------- */
void can_core_reset_stats(can_core_handle_t *handle)
{
    if (!handle)
    {
        return;
    }
    memset(&handle->stats, 0, sizeof(handle->stats));

#ifdef ESP_PLATFORM
    can_drv_counters_t isr;

    can_drv_counters(&isr);
    handle->isr_base_rx_missed = isr.rx_missed;
    handle->isr_base_rx_overrun = isr.rx_overrun;
    handle->isr_base_rx_bad = isr.rx_bad;
    handle->isr_base_arb_lost = isr.arb_lost;
    handle->isr_base_err[0] = isr.err_stuff;
    handle->isr_base_err[1] = isr.err_form;
    handle->isr_base_err[2] = isr.err_bit;
    handle->isr_base_err[3] = isr.err_ack;
    handle->isr_base_err[4] = isr.err_other;
    handle->isr_base_tx[0] = isr.tx_retries;
    handle->isr_base_tx[1] = isr.tx_lost;
    handle->isr_base_tx[2] = isr.tx_done;
    handle->isr_base_storms = isr.rx_storms;
#endif
}

/* can_core_set_silent_mode() and can_core_set_baud() live in
 * can_core_link.c: they go through the listen-before-talk policy. */

static bool can_subscription_matches_frame(uint32_t filter,
                                           uint32_t mask,
                                           bool ext,
                                           bool monitor_all,
                                           const can_core_frame_t *frame)
{
    if (!frame)
    {
        return false;
    }

    if (monitor_all)
    {
        return true;
    }

    if (frame->ext != ext)
    {
        return false;
    }

    return can_core_filter_match(frame->id, filter, mask);
}

/** @return false when the queue was full and the oldest frame was
 *          evicted to make room (the caller counts drops). */
static bool dispatch_frame_to_queue_subscriber(QueueHandle_t queue,
                                               const can_core_frame_t *frame)
{
#ifdef ESP_PLATFORM
    can_core_frame_t dropped_frame;

    if (queue == NULL || frame == NULL)
    {
        return true;
    }

    if (xQueueSendToBack(queue, frame, 0) == pdPASS)
    {
        return true;
    }

    (void)xQueueReceive(queue, &dropped_frame, 0);
    (void)xQueueSendToBack(queue, frame, 0);
    return false;
#else
    (void)queue;
    (void)frame;
    return true;
#endif
}

#ifdef ESP_PLATFORM
/* Rendezvous with the RX task before a driver bounce: caller sets
 * handle->reconfiguring, then waits for rx_parked -- the task must be
 * provably outside a receive when the node is torn down (belt to the
 * static-queue braces; kept from the 2026-07-21 legacy-driver fix). */
void can_core_rx_wait_parked(can_core_handle_t *handle)
{
    for (int i = 0; i < 50 && !handle->rx_parked && !handle->rx_exited;
         i++)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!handle->rx_parked && !handle->rx_exited)
    {
        ESP_LOGE(TAG, "rx task did not park; reconfiguring anyway");
    }
}
#endif
