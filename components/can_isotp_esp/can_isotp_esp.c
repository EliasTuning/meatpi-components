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
 * @file can_isotp_esp.c
 * @brief ISO-TP provider: esp_isotp (externally managed) over can_manager.
 *
 * Threads:
 *   - a consumer's task calls open/close/send/recv (the can_isotp_ops_t);
 *   - can_core's RX task copies every frame matching an open session's
 *     rx id into s_frame_q (ONE queue, one subscription per session);
 *   - the "isotp" task drains s_frame_q, feeds each frame to its
 *     session's stack, and polls the stacks that are mid-transfer at tick
 *     cadence (consecutive-frame pacing, N_Bs/N_Cr expiry).
 *
 * Locking: esp_isotp has none of its own, so EVERY esp_isotp_* call on a
 * session happens under s->lock; the session table under s_tbl_lock (the
 * task holds it while feeding, so close() cannot free a session the task
 * is inside). The stack's callbacks run inside those locked calls and only
 * post to semaphores / the mailbox.
 *
 * Why not the stack's own TWAI binding: the bus is shared (monitor,
 * bridges, autopid on the MIC chip); can_manager owns the node and fans
 * frames out to all clients, so a session consumes only its own rx id
 * and nothing else on the bus changes while it is open.
 */
#include "can_isotp_esp.h"

#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_isotp.h"

#include "can_core.h"
#include "can_isotp.h"
#include "can_manager.h"
#include "log_manager.h"

#include "can_isotp_esp_private.h"

static const char *TAG = "can_isotp_esp";

#define ISOTP_MAX_SESSIONS   5    /* 1 uds_manager + J2534_MAX_CHANNELS   */
#define ISOTP_TASK_STACK     4096 /* PSRAM: no flash from this task        */
#define ISOTP_TASK_PRIO      9    /* just under can_core_rx (10)           */
#define ISOTP_FRAME_Q_LEN    128  /* can_core_frame_t, 16 B each           */
#define ISOTP_TX_FRAME_POOL  16
#define ISOTP_IDLE_WAIT_MS   100  /* wake cadence with nothing in flight   */

/* after any activity a session is polled at tick cadence for this long:
 * longer than the stack's N_Bs/N_Cr (Kconfig response timeout, 100 ms) so
 * a stalled transfer is always flagged by a poll and the link never stays
 * "in progress" forever */
#define ISOTP_BUSY_MS        300

struct can_isotp_session
{
    esp_isotp_handle_t  tp;
    SemaphoreHandle_t   lock;      /* every esp_isotp_* call on tp        */
    SemaphoreHandle_t   tx_done;   /* binary: tx-complete callback        */
    SemaphoreHandle_t   rx_sem;    /* counting: one token per mailbox PDU */
    isotp_mbox_t        mbox;
    isotp_mbox_slot_t  *slots;     /* PSRAM                               */
    int                 sub_idx;   /* can_manager queue subscription      */
    uint32_t            tx_id;
    uint32_t            rx_id;
    bool                ext;
    volatile bool       tx_inflight;    /* multi-frame send not complete  */
    volatile uint32_t   tx_deadline_ms; /* give up tracking it after this */
    volatile uint32_t   busy_until_ms;  /* tick-cadence polling until     */
};

static struct can_isotp_session *s_tbl[ISOTP_MAX_SESSIONS];
static SemaphoreHandle_t s_tbl_lock;
static StaticSemaphore_t s_tbl_lock_buf;

static QueueHandle_t s_frame_q;
static StaticQueue_t s_frame_q_buf;
EXT_RAM_BSS_ATTR static uint8_t
    s_frame_q_store[ISOTP_FRAME_Q_LEN * sizeof(can_core_frame_t)];

static TaskHandle_t s_task;
static StaticTask_t s_task_tcb;
EXT_RAM_BSS_ATTR static StackType_t s_task_stack[ISOTP_TASK_STACK];

static bool s_active;
static can_isotp_esp_stats_t s_stats;
static portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;

#define STAT_INC(field)                        \
    do                                         \
    {                                          \
        portENTER_CRITICAL(&s_stats_mux);      \
        s_stats.field++;                       \
        portEXIT_CRITICAL(&s_stats_mux);       \
    } while (0)

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool session_busy(const struct can_isotp_session *s, uint32_t now)
{
    return s->tx_inflight || (int32_t)(s->busy_until_ms - now) > 0;
}

/* ---- esp_isotp callbacks (inside a locked esp_isotp_* call) ------------- */

static void on_rx_pdu(esp_isotp_handle_t h, const uint8_t *data,
                      uint32_t size, void *arg)
{
    struct can_isotp_session *s = arg;

    (void)h;
    STAT_INC(pdus_rx);

    if (isotp_mbox_put(&s->mbox, data, size))
    {
        xSemaphoreGive(s->rx_sem);
    }
    else
    {
        STAT_INC(rx_dropped);
    }
}

static void on_tx_done(esp_isotp_handle_t h, uint32_t tx_size, void *arg)
{
    struct can_isotp_session *s = arg;

    (void)h;
    (void)tx_size;
    s->tx_inflight = false;
    STAT_INC(pdus_tx);
    xSemaphoreGive(s->tx_done);
}

static esp_err_t on_can_tx(uint32_t id, const uint8_t *data, uint8_t size,
                           void *arg)
{
    struct can_isotp_session *s = arg;

    /* blocks up to 100 ms when the TWAI TX queue is full; a failure makes
       the stack abort this transfer (send_status error -> our send times
       out) — exactly the "bus not delivering" outcome we want */
    return can_manager_send(id, s->ext, false, data, size);
}

/* ---- the provider task ----------------------------------------------------- */

static struct can_isotp_session *find_by_rx(uint32_t id, bool ext)
{
    for (int i = 0; i < ISOTP_MAX_SESSIONS; i++)
    {
        if (s_tbl[i] != NULL && s_tbl[i]->rx_id == id && s_tbl[i]->ext == ext)
        {
            return s_tbl[i];
        }
    }

    return NULL;
}

/* s_tbl_lock held */
static void feed_frame(const can_core_frame_t *fr)
{
    if (fr->rtr || fr->dlc > 8)
    {
        return;
    }

    struct can_isotp_session *s = find_by_rx(fr->id, fr->ext);

    if (s == NULL)
    {
        STAT_INC(frames_orphan); /* closed between dispatch and here */
        return;
    }

    uint8_t payload[8];
    twai_frame_t tf;

    memset(&tf, 0, sizeof(tf));
    tf.header.id = fr->id;
    tf.header.ide = fr->ext;
    tf.header.dlc = fr->dlc;
    memcpy(payload, fr->data, fr->dlc);
    tf.buffer = payload;
    tf.buffer_len = fr->dlc;

    xSemaphoreTake(s->lock, portMAX_DELAY);
    (void)esp_isotp_feed_can_frame(s->tp, &tf);
    (void)esp_isotp_poll(s->tp); /* a flow control just unblocked CFs */
    xSemaphoreGive(s->lock);

    s->busy_until_ms = now_ms() + ISOTP_BUSY_MS;
    STAT_INC(frames_fed);
}

/* s_tbl_lock held; returns whether any session still needs tick polling */
static bool poll_busy_sessions(void)
{
    uint32_t now = now_ms();
    bool any = false;

    for (int i = 0; i < ISOTP_MAX_SESSIONS; i++)
    {
        struct can_isotp_session *s = s_tbl[i];

        if (s == NULL)
        {
            continue;
        }

        if (s->tx_inflight && (int32_t)(now - s->tx_deadline_ms) > 0)
        {
            /* the sender gave up on it long ago; the stack has flagged
               N_Bs by now (busy window > N_Bs) — stop tracking */
            s->tx_inflight = false;
        }

        if (!session_busy(s, now))
        {
            continue;
        }

        any = true;
        xSemaphoreTake(s->lock, portMAX_DELAY);
        (void)esp_isotp_poll(s->tp);
        xSemaphoreGive(s->lock);
    }

    return any;
}

static void isotp_task(void *arg)
{
    TickType_t wait = pdMS_TO_TICKS(ISOTP_IDLE_WAIT_MS);

    (void)arg;

    for (;;)
    {
        can_core_frame_t fr;
        bool got = (xQueueReceive(s_frame_q, &fr, wait) == pdTRUE);

        xSemaphoreTake(s_tbl_lock, portMAX_DELAY);

        if (got)
        {
            feed_frame(&fr);
        }

        /* drain a burst before polling: consecutive frames arrive every
           ~250 us at 500 kbps */
        while (xQueueReceive(s_frame_q, &fr, 0) == pdTRUE)
        {
            feed_frame(&fr);
        }

        bool busy = poll_busy_sessions();

        xSemaphoreGive(s_tbl_lock);

        wait = busy ? 1 : pdMS_TO_TICKS(ISOTP_IDLE_WAIT_MS);
    }
}

/* s_tbl_lock held */
static esp_err_t ensure_started(void)
{
    if (s_frame_q == NULL)
    {
        s_frame_q = xQueueCreateStatic(ISOTP_FRAME_Q_LEN,
                                       sizeof(can_core_frame_t),
                                       s_frame_q_store, &s_frame_q_buf);

        if (s_frame_q == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_task == NULL)
    {
        s_task = xTaskCreateStatic(isotp_task, "isotp", ISOTP_TASK_STACK,
                                   NULL, ISOTP_TASK_PRIO, s_task_stack,
                                   &s_task_tcb);

        if (s_task == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

/* ---- sessions -------------------------------------------------------------- */

static void *psram_calloc(size_t n, size_t size)
{
    void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    return (p != NULL) ? p : calloc(n, size);
}

static void session_destroy(struct can_isotp_session *s)
{
    if (s->tp != NULL)
    {
        (void)esp_isotp_delete(s->tp);
    }

    if (s->lock != NULL)
    {
        vSemaphoreDelete(s->lock);
    }

    if (s->tx_done != NULL)
    {
        vSemaphoreDelete(s->tx_done);
    }

    if (s->rx_sem != NULL)
    {
        vSemaphoreDelete(s->rx_sem);
    }

    free(s->slots);
    free(s);
}

static esp_err_t isotp_open(const can_isotp_cfg_t *cfg,
                            can_isotp_session_t *out)
{
    if (cfg == NULL || out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out = NULL;

    if (can_manager_core_handle() == NULL)
    {
        STAT_INC(open_fails);
        return ESP_ERR_INVALID_STATE; /* bus down (contract) */
    }

    xSemaphoreTake(s_tbl_lock, portMAX_DELAY);

    esp_err_t err = ensure_started();
    int slot = -1;

    if (err == ESP_OK)
    {
        for (int i = 0; i < ISOTP_MAX_SESSIONS; i++)
        {
            if (s_tbl[i] == NULL)
            {
                if (slot < 0)
                {
                    slot = i;
                }
            }
            else if (s_tbl[i]->rx_id == cfg->rx_id &&
                     s_tbl[i]->ext == cfg->ext_id)
            {
                ESP_LOGW(TAG, "rx %lX already has a session",
                         (unsigned long)cfg->rx_id);
                err = ESP_ERR_INVALID_STATE;
            }
        }

        if (err == ESP_OK && slot < 0)
        {
            ESP_LOGW(TAG, "session cap (%d) reached", ISOTP_MAX_SESSIONS);
            err = ESP_ERR_NO_MEM;
        }
    }

    struct can_isotp_session *s = NULL;

    if (err == ESP_OK)
    {
        s = psram_calloc(1, sizeof(*s));

        if (s != NULL)
        {
            s->sub_idx = -1;
            s->slots = psram_calloc(CAN_ISOTP_ESP_MBOX_SLOTS,
                                    sizeof(isotp_mbox_slot_t));
            s->lock = xSemaphoreCreateMutex();
            s->tx_done = xSemaphoreCreateBinary();
            s->rx_sem = xSemaphoreCreateCounting(CAN_ISOTP_ESP_MBOX_SLOTS, 0);
        }

        if (s == NULL || s->slots == NULL || s->lock == NULL ||
            s->tx_done == NULL || s->rx_sem == NULL)
        {
            err = ESP_ERR_NO_MEM;
        }
    }

    if (err == ESP_OK)
    {
        isotp_mbox_init(&s->mbox, s->slots, CAN_ISOTP_ESP_MBOX_SLOTS);
        s->tx_id = cfg->tx_id;
        s->rx_id = cfg->rx_id;
        s->ext = cfg->ext_id;

        esp_isotp_config_t ic =
        {
            .tx_id                   = cfg->tx_id,
            .rx_id                   = cfg->rx_id,
            .tx_buffer_size          = CAN_ISOTP_ESP_MAX_PDU,
            .rx_buffer_size          = CAN_ISOTP_ESP_MAX_PDU,
            .tx_frame_pool_size      = ISOTP_TX_FRAME_POOL,
            .tx_use_padding          = cfg->use_padding,
            .tx_padding_value        = cfg->padding_byte,
            .rx_callback             = on_rx_pdu,
            .tx_callback             = on_tx_done,
            .callback_arg            = s,
            .externally_managed_twai = true,
            .can_tx_callback         = on_can_tx,
            .can_tx_callback_arg     = s,
        };

        err = esp_isotp_new_transport(NULL, &ic, &s->tp);

        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_isotp_new_transport: %s",
                     esp_err_to_name(err));
            s->tp = NULL;
        }
    }

    if (err == ESP_OK)
    {
        err = can_manager_subscribe_queue(s_frame_q, cfg->rx_id,
                                          cfg->ext_id ? 0x1FFFFFFFu : 0x7FFu,
                                          cfg->ext_id, false, &s->sub_idx);

        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "can_manager_subscribe_queue: %s (all %d "
                          "subscriber slots taken?)",
                     esp_err_to_name(err), CAN_CORE_MAX_QUEUE_SUBSCRIBERS);
            s->sub_idx = -1;
        }
    }

    if (err == ESP_OK)
    {
        if (cfg->block_size != 0 || cfg->stmin_ms != 0)
        {
            ESP_LOGW(TAG, "per-session BS/STmin not supported by esp_isotp "
                          "0.1.1: flow control uses Kconfig BS=%d STmin=%d us",
                     CONFIG_ISO_TP_DEFAULT_BLOCK_SIZE,
                     CONFIG_ISO_TP_DEFAULT_ST_MIN_US);
        }

        s_tbl[slot] = s;

        portENTER_CRITICAL(&s_stats_mux);
        s_stats.opens++;
        s_stats.sessions_open++;

        if (s_stats.sessions_open > s_stats.sessions_peak)
        {
            s_stats.sessions_peak = s_stats.sessions_open;
        }

        portEXIT_CRITICAL(&s_stats_mux);

        ESP_LOGI(TAG, "session open: tx %lX rx %lX %s pad=%d",
                 (unsigned long)cfg->tx_id, (unsigned long)cfg->rx_id,
                 cfg->ext_id ? "29-bit" : "11-bit", cfg->use_padding);
        *out = s;
    }

    xSemaphoreGive(s_tbl_lock);

    if (err != ESP_OK)
    {
        if (s != NULL)
        {
            session_destroy(s);
        }

        STAT_INC(open_fails);
    }

    return err;
}

static void isotp_close(can_isotp_session_t s)
{
    if (s == NULL)
    {
        return;
    }

    xSemaphoreTake(s_tbl_lock, portMAX_DELAY);

    for (int i = 0; i < ISOTP_MAX_SESSIONS; i++)
    {
        if (s_tbl[i] == s)
        {
            s_tbl[i] = NULL;
        }
    }

    if (s->sub_idx >= 0)
    {
        (void)can_manager_unsubscribe_queue(s->sub_idx);
        s->sub_idx = -1;
    }

    xSemaphoreGive(s_tbl_lock);

    /* the task cannot reach s any more (lookups happen under s_tbl_lock,
       which it holds while feeding). A consumer must not send/recv on s
       concurrently with close — both consumers serialize their own calls. */
    xSemaphoreTake(s->lock, portMAX_DELAY);
    xSemaphoreGive(s->lock);

    ESP_LOGI(TAG, "session close: tx %lX rx %lX",
             (unsigned long)s->tx_id, (unsigned long)s->rx_id);

    session_destroy(s);

    portENTER_CRITICAL(&s_stats_mux);

    if (s_stats.sessions_open > 0)
    {
        s_stats.sessions_open--;
    }

    portEXIT_CRITICAL(&s_stats_mux);
}

static esp_err_t isotp_send(can_isotp_session_t s, const uint8_t *data,
                            size_t len, uint32_t timeout_ms)
{
    if (s == NULL || data == NULL || len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (len > CAN_ISOTP_ESP_MAX_PDU)
    {
        return ESP_ERR_NO_MEM;
    }

    if (can_manager_core_handle() == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s->lock, portMAX_DELAY);
    (void)xSemaphoreTake(s->tx_done, 0); /* drop a stale completion */
    s->tx_deadline_ms = now_ms() + timeout_ms + 500;
    s->busy_until_ms = now_ms() + ISOTP_BUSY_MS;
    s->tx_inflight = (len > 7); /* a single frame completes inside send */

    esp_err_t err = esp_isotp_send(s->tp, data, (uint32_t)len);

    if (err != ESP_OK)
    {
        s->tx_inflight = false;
    }

    xSemaphoreGive(s->lock);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "send %u B on %lX: %s", (unsigned)len,
                 (unsigned long)s->tx_id, esp_err_to_name(err));

        switch (err)
        {
        case ESP_ERR_NOT_FINISHED: return ESP_ERR_INVALID_STATE;
        case ESP_ERR_NO_MEM:
        case ESP_ERR_INVALID_SIZE:  return ESP_ERR_NO_MEM;
        case ESP_ERR_TIMEOUT:       return ESP_ERR_TIMEOUT;
        default:                    return ESP_FAIL;
        }
    }

    /* single frame: the completion token was given inside esp_isotp_send;
       multi-frame: the task's polls send the CFs after the peer's flow
       control and the token comes from the last one */
    if (xSemaphoreTake(s->tx_done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE)
    {
        return ESP_OK;
    }

    /* the peer's flow control never came or its CFs stalled (contract:
       ESP_ERR_TIMEOUT). tx_inflight stays set so the task keeps polling
       until the stack flags N_Bs or the deadline passes — the link is
       never left "in progress" */
    STAT_INC(tx_timeouts);
    ESP_LOGW(TAG, "send %u B on %lX: no completion within %lu ms",
             (unsigned)len, (unsigned long)s->tx_id,
             (unsigned long)timeout_ms);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t isotp_recv(can_isotp_session_t s, uint8_t *buf, size_t cap,
                            size_t *len, uint32_t timeout_ms)
{
    if (s == NULL || buf == NULL || len == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *len = 0;

    if (xSemaphoreTake(s->rx_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    xSemaphoreTake(s->lock, portMAX_DELAY); /* the task puts under it */
    esp_err_t err = isotp_mbox_take(&s->mbox, buf, cap, len);
    xSemaphoreGive(s->lock);

    if (err == ESP_ERR_NOT_FOUND)
    {
        return ESP_ERR_TIMEOUT; /* token without a PDU: cannot happen */
    }

    if (err == ESP_ERR_NO_MEM)
    {
        STAT_INC(rx_oversize); /* consumed, per the contract */
    }

    return err;
}

/* ---- registration ---------------------------------------------------------- */

esp_err_t can_isotp_esp_init(void)
{
    static const log_descriptor_t LOG_DESC = { "can_isotp_esp", ESP_LOG_INFO };
    /* the vendored stack logs every send at INFO — keep it quiet unless
       someone turns it up in the log settings */
    static const log_descriptor_t STACK_LOG = { "esp_isotp", ESP_LOG_WARN };
    static const can_isotp_ops_t OPS =
    {
        .open  = isotp_open,
        .close = isotp_close,
        .send  = isotp_send,
        .recv  = isotp_recv,
    };

    log_manager_register(&LOG_DESC);
    log_manager_register(&STACK_LOG);

    if (s_tbl_lock == NULL)
    {
        s_tbl_lock = xSemaphoreCreateMutexStatic(&s_tbl_lock_buf);
    }

    if (s_active)
    {
        return ESP_OK;
    }

    if (can_isotp() != NULL)
    {
        ESP_LOGI(TAG, "an add-on pack already provides ISO-TP; the native "
                      "provider stands by");
        return ESP_OK;
    }

    esp_err_t err = can_isotp_provide(&OPS);

    if (err == ESP_OK)
    {
        s_active = true;
        ESP_LOGI(TAG, "native ISO-TP provider registered (esp_isotp over "
                      "can_manager; %d sessions, PDU <= %d B)",
                 ISOTP_MAX_SESSIONS, CAN_ISOTP_ESP_MAX_PDU);
    }

    return err;
}

bool can_isotp_esp_active(void)
{
    return s_active;
}

void can_isotp_esp_get_stats(can_isotp_esp_stats_t *out)
{
    if (out == NULL)
    {
        return;
    }

    portENTER_CRITICAL(&s_stats_mux);
    *out = s_stats;
    portEXIT_CRITICAL(&s_stats_mux);
}
