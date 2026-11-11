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
 * @file bridge_manager_pump.c
 * @brief The pump: one task per active bridge draining BOTH directions via a
 *        FreeRTOS queue set (README: one 4 KB PSRAM stack per bridge beats
 *        two slim tasks, the queue-set wait serializes the directions with
 *        no cross-blocking, and endpoint send() is bounded by the endpoint's
 *        own policy). The raw path moves bytes untouched; translators run
 *        per direction with a per-direction ctx from the static PSRAM pool.
 *
 * Slow-side policy: this task blocks only in endpoint send() (bounded by the
 * endpoint: socket SO_SNDTIMEO, obd TX mutex). While it is there, the
 * bridge-owned RX queues fill and the PRODUCING endpoint drops-and-counts
 * (obd fan-out drops, socket rx_drops): a bridge never blocks its faster
 * side indefinitely, and never crashes on overflow.
 */
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "bridge_manager.h"
#include "bridge_manager_private.h"

static const char *TAG = "bridge_manager";

/* 32 -> 64 (2026-09-08): an ATMA flood at 2500 frames/s arrives as one
 * ~29-byte chunk per frame from the event-driven obd_chip RX; the deeper
 * queue rides out send() hiccups (WiFi retries) without the fan-out
 * dropping. Cost: +33 KB PSRAM across 4 bridges x 2 directions. */
#define BM_QUEUE_DEPTH 64

/* Raw-bridge coalescing (2026-09-08): the pump gathers every chunk already
 * waiting in the queue into ONE send() of up to this many bytes. Costs
 * nothing when a single chunk is waiting (a short ELM reply goes out at
 * once), and turns 2500 socket sends/s into a few hundred under a flood -
 * measured: at 2500 frames/s the per-chunk pump dropped 6052 chunks at the
 * fan-out while every other counter was clean. */
#define BM_COALESCE_MAX 1024

typedef struct
{
    bool in_use;
    const bm_bridge_cfg_t *cfg;
    const bridge_endpoint_t *a;
    const bridge_endpoint_t *b;
    const bridge_translator_t *tr;      /* NULL = raw                     */

    QueueHandle_t qa;                   /* RX from endpoint a             */
    QueueHandle_t qb;                   /* RX from endpoint b             */
    QueueSetHandle_t set;
    TaskHandle_t task;
    volatile bool running;

    bridge_stats_t stats;

    int64_t last_flush_a2b;             /* accumulating-codec flush clock */
    int64_t last_flush_b2a;             /* (ms; see pump_flush_due)       */

    uint8_t ctx_a2b[BM_CTX_MAX];        /* translator reassembly contexts */
    uint8_t ctx_b2a[BM_CTX_MAX];

    uint8_t batch[BM_COALESCE_MAX];     /* raw coalescing buffer (PSRAM)  */
} bm_bridge_t;

static bm_bridge_t s_br[BRIDGE_MANAGER_MAX_BRIDGES] EXT_RAM_BSS_ATTR;

/* why a configured, ENABLED bridge is not running, by config index: the
 * jack or codec its row names was never registered in this composition (a
 * socket server or a WebSocket channel renamed or removed after the row
 * was written, the raw editor), or a single-stream provider refused the
 * second subscribe. "" = built, or not enabled. Read by the status
 * surface (/api/bridges "reason") so the user sees the row to fix. */
static char s_reason[BRIDGE_MANAGER_MAX_BRIDGES][64] EXT_RAM_BSS_ATTR;

/* static queue/task storage per bridge slot (standard §2: PSRAM data,
 * internal control blocks) */
static uint8_t s_qa_store[BRIDGE_MANAGER_MAX_BRIDGES]
                         [BM_QUEUE_DEPTH * sizeof(bridge_chunk_t)]
    EXT_RAM_BSS_ATTR;
static uint8_t s_qb_store[BRIDGE_MANAGER_MAX_BRIDGES]
                         [BM_QUEUE_DEPTH * sizeof(bridge_chunk_t)]
    EXT_RAM_BSS_ATTR;
static StaticQueue_t s_qa_buf[BRIDGE_MANAGER_MAX_BRIDGES]; /* internal */
static StaticQueue_t s_qb_buf[BRIDGE_MANAGER_MAX_BRIDGES]; /* internal */
static StackType_t s_stacks[BRIDGE_MANAGER_MAX_BRIDGES][4096]
    EXT_RAM_BSS_ATTR;
static StaticTask_t s_tcbs[BRIDGE_MANAGER_MAX_BRIDGES];    /* internal */

/* ---- translated send: sink feeds the destination endpoint ------------------- */

typedef struct
{
    const bridge_endpoint_t *dst;
    bm_bridge_t *br;
} bm_sink_arg_t;

static esp_err_t pump_sink(void *arg, const uint8_t *out, size_t out_len)
{
    bm_sink_arg_t *s = arg;
    esp_err_t err = s->dst->send(out, out_len);

    if (err != ESP_OK)
    {
        s->br->stats.send_errors++;
    }

    return err;
}

/* reply channel (§6): a sink that writes back to the near endpoint (the side
 * the command came from), stored in the codec ctx for wants_reply codecs. */
static esp_err_t reply_sink(void *arg, const uint8_t *out, size_t out_len)
{
    const bridge_endpoint_t *ep = arg;
    return ep->send(out, out_len);
}

static void pump_one(bm_bridge_t *br, const bridge_chunk_t *chunk, bool a2b);

/** Raw path: the chunk in hand plus every chunk already queued behind it
 *  (up to BM_COALESCE_MAX bytes) go out as ONE endpoint send.
 *
 *  Queue-set discipline (FreeRTOS): a member queue may only be read after
 *  xQueueSelectFromSet handed it out - reading it directly leaves the
 *  set's container holding stale notifications until it overflows
 *  (`assert prvNotifyQueueSetContainer queue.c:3362`, seen live at 2500
 *  frames/s on the first cut of this batching). So every extra chunk is
 *  claimed through the set; when the set hands out the OTHER direction's
 *  queue instead, that chunk is pumped right after this batch. */
static void pump_raw_batch(bm_bridge_t *br, QueueHandle_t q,
                           const bridge_chunk_t *first, bool a2b)
{
    const bridge_endpoint_t *dst = a2b ? br->b : br->a;
    size_t len = first->len;
    uint32_t chunks = 1;
    bridge_chunk_t more;
    bridge_chunk_t other;
    bool have_other = false;

    memcpy(br->batch, first->data, first->len);

    while (len + sizeof(more.data) <= BM_COALESCE_MAX)
    {
        QueueSetMemberHandle_t m = xQueueSelectFromSet(br->set, 0);

        if (m == NULL)
        {
            break;
        }

        if (m == (QueueSetMemberHandle_t)q)
        {
            if (xQueueReceive(q, &more, 0) != pdTRUE)
            {
                break;
            }

            memcpy(br->batch + len, more.data, more.len);
            len += more.len;
            chunks++;
        }
        else
        {
            /* the set handed us the other direction: take it (the set
               entry is consumed) and deliver it after this batch */
            if (xQueueReceive((QueueHandle_t)m, &other, 0) == pdTRUE)
            {
                have_other = true;
            }

            break;
        }
    }

    if (dst->send(br->batch, len) != ESP_OK)
    {
        br->stats.send_errors++;
    }

    if (a2b)
    {
        br->stats.a2b_chunks += chunks;
        br->stats.a2b_bytes += len;
    }
    else
    {
        br->stats.b2a_chunks += chunks;
        br->stats.b2a_bytes += len;
    }

    if (have_other)
    {
        pump_one(br, &other, !a2b);
    }
}

typedef struct
{
    uint8_t *buf;
    size_t len;
    size_t cap;
} tr_batch_t;

static esp_err_t tr_batch_sink(void *arg, const uint8_t *out, size_t out_len)
{
    tr_batch_t *b = arg;

    if (out_len > b->cap - b->len)
    {
        return ESP_ERR_NO_MEM;
    }

    memcpy(b->buf + b->len, out, out_len);
    b->len += out_len;
    return ESP_OK;
}

static void tr_batch_flush(bm_bridge_t *br, const bridge_endpoint_t *dst,
                           tr_batch_t *b)
{
    if (b->len == 0)
    {
        return;
    }

    if (dst->send(b->buf, b->len) != ESP_OK)
    {
        br->stats.send_errors++;
    }

    b->len = 0;
}

/** One codec call into the batch. A line that does not fit is sent only
 *  after the bytes already gathered, so a Lawicel line is never split. */
static void tr_feed(bm_bridge_t *br, const bridge_endpoint_t *dst,
                    tr_batch_t *batch, const bridge_chunk_t *chunk, bool a2b)
{
    void *ctx = a2b ? br->ctx_a2b : br->ctx_b2a;
    esp_err_t err;

    if (batch->cap - batch->len < 64)
    {
        tr_batch_flush(br, dst, batch);
    }

    err = a2b
        ? br->tr->decode(ctx, chunk->data, chunk->len, tr_batch_sink, batch)
        : br->tr->encode(ctx, chunk->data, chunk->len, tr_batch_sink, batch);

    if (err == ESP_ERR_NO_MEM)
    {
        tr_batch_flush(br, dst, batch);
        err = a2b
            ? br->tr->decode(ctx, chunk->data, chunk->len, tr_batch_sink, batch)
            : br->tr->encode(ctx, chunk->data, chunk->len, tr_batch_sink, batch);
    }

    if (err != ESP_OK)
    {
        br->stats.codec_errors++;
    }

    if (a2b)
    {
        br->stats.a2b_chunks++;
        br->stats.a2b_bytes += chunk->len;
    }
    else
    {
        br->stats.b2a_chunks++;
        br->stats.b2a_bytes += chunk->len;
    }
}

/** Translated path: gather queued chunks into one send, same cap as the
 *  raw coalescer. SLCAN lines concatenate. The queue set is drained the
 *  same way as pump_raw_batch so its container does not fill with stale
 *  notifications. */
static void pump_translated_batch(bm_bridge_t *br, QueueHandle_t q,
                                  const bridge_chunk_t *first, bool a2b)
{
    const bridge_endpoint_t *dst = a2b ? br->b : br->a;
    tr_batch_t batch = { .buf = br->batch, .len = 0, .cap = BM_COALESCE_MAX };
    bridge_chunk_t more;
    bridge_chunk_t other;
    bool have_other = false;

    tr_feed(br, dst, &batch, first, a2b);

    while (batch.len + 64 <= batch.cap)
    {
        QueueSetMemberHandle_t m = xQueueSelectFromSet(br->set, 0);

        if (m == NULL)
        {
            break;
        }

        if (m == (QueueSetMemberHandle_t)q)
        {
            if (xQueueReceive(q, &more, 0) != pdTRUE)
            {
                break;
            }

            tr_feed(br, dst, &batch, &more, a2b);
        }
        else
        {
            if (xQueueReceive((QueueHandle_t)m, &other, 0) == pdTRUE)
            {
                have_other = true;
            }
            break;
        }
    }

    tr_batch_flush(br, dst, &batch);

    if (have_other)
    {
        pump_one(br, &other, !a2b);
    }
}

static void pump_one(bm_bridge_t *br, const bridge_chunk_t *chunk, bool a2b)
{
    const bridge_endpoint_t *dst = a2b ? br->b : br->a;

    if (br->tr == NULL)
    {
        /* raw: pure move-bytes, the hot path does no parsing */
        if (dst->send(chunk->data, chunk->len) != ESP_OK)
        {
            br->stats.send_errors++;
        }
    }
    else
    {
        bm_sink_arg_t sink_arg = { .dst = dst, .br = br };
        void *ctx = a2b ? br->ctx_a2b : br->ctx_b2a;
        esp_err_t err = a2b
            ? br->tr->decode(ctx, chunk->data, chunk->len, pump_sink,
                             &sink_arg)
            : br->tr->encode(ctx, chunk->data, chunk->len, pump_sink,
                             &sink_arg);

        if (err != ESP_OK)
        {
            br->stats.codec_errors++;
        }
    }

    if (a2b)
    {
        br->stats.a2b_chunks++;
        br->stats.a2b_bytes += chunk->len;
    }
    else
    {
        br->stats.b2a_chunks++;
        br->stats.b2a_bytes += chunk->len;
    }
}

/* accumulating-codec flush (bridge_manager.h): bounded-latency batch
 * shipping for translators that buffer across chunks */
static void pump_flush_due(bm_bridge_t *br, int64_t now_ms)
{
    if (br->tr == NULL || br->tr->flush == NULL)
    {
        return;
    }

    if (now_ms - br->last_flush_a2b >= (int64_t)br->tr->flush_ms)
    {
        bm_sink_arg_t sink_arg = { .dst = br->b, .br = br };

        if (br->tr->flush(br->ctx_a2b, pump_sink, &sink_arg) != ESP_OK)
        {
            br->stats.codec_errors++;
        }
        br->last_flush_a2b = now_ms;
    }

    if (now_ms - br->last_flush_b2a >= (int64_t)br->tr->flush_ms)
    {
        bm_sink_arg_t sink_arg = { .dst = br->a, .br = br };

        if (br->tr->flush(br->ctx_b2a, pump_sink, &sink_arg) != ESP_OK)
        {
            br->stats.codec_errors++;
        }
        br->last_flush_b2a = now_ms;
    }
}

static void pump_task(void *arg)
{
    bm_bridge_t *br = arg;
    bridge_chunk_t chunk;

    ESP_LOGD(TAG, "%s: pump up", br->cfg->name);

    while (br->running)
    {
        QueueSetMemberHandle_t ready =
            xQueueSelectFromSet(br->set, pdMS_TO_TICKS(100));

        if (ready != NULL &&
            xQueueReceive((QueueHandle_t)ready, &chunk, 0) == pdTRUE)
        {
            bool a2b = (ready == (QueueSetMemberHandle_t)br->qa);

            if (br->tr == NULL)
            {
                pump_raw_batch(br, (QueueHandle_t)ready, &chunk, a2b);
            }
            else
            {
                pump_translated_batch(br, (QueueHandle_t)ready, &chunk, a2b);
            }
        }

        if (br->tr != NULL && br->tr->flush != NULL)
        {
            pump_flush_due(br, esp_timer_get_time() / 1000);
        }
    }

    br->task = NULL;
    vTaskDelete(NULL);
}

/* ---- build/teardown ----------------------------------------------------------- */

/* The two RX queues and their set of a slot whose pump is not running: the
 * teardown, and a build that stops after the allocations (a refused
 * subscribe). Until 2026-10-29 the refused-subscribe path returned with the
 * dynamic set still allocated (internal RAM, once per such row per start).
 * A queue that still holds chunks stays in the set (pdFAIL): harmless, the
 * set goes with it. */
static void drop_queues(bm_bridge_t *br)
{
    if (br->qa == NULL)
    {
        return;
    }

    if (br->set != NULL)
    {
        xQueueRemoveFromSet(br->qa, br->set);
        xQueueRemoveFromSet(br->qb, br->set);
    }

    vQueueDelete(br->qa);
    vQueueDelete(br->qb);

    if (br->set != NULL)
    {
        vQueueDelete(br->set);
        br->set = NULL;
    }

    br->qa = NULL;
    br->qb = NULL;
}

static void teardown_bridge(bm_bridge_t *br, int idx)
{
    (void)idx;

    if (!br->in_use)
    {
        return;
    }

    br->running = false;

    for (int i = 0; i < 25 && br->task != NULL; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    br->a->unsubscribe(br->qa);
    br->b->unsubscribe(br->qb);
    drop_queues(br);
    br->in_use = false;
}

static esp_err_t build_bridge(int idx, const bm_bridge_cfg_t *cfg)
{
    bm_bridge_t *br = &s_br[idx];

    memset(&br->stats, 0, sizeof(br->stats));
    br->last_flush_a2b = esp_timer_get_time() / 1000;
    br->last_flush_b2a = br->last_flush_a2b;
    br->cfg = cfg;
    br->a = bm_core_endpoint(cfg->a);
    br->b = bm_core_endpoint(cfg->b);
    br->tr = (strcmp(cfg->translator, "raw") == 0)
                 ? NULL : bm_core_translator(cfg->translator);

    if (br->a == NULL || br->b == NULL ||
        (br->tr == NULL && strcmp(cfg->translator, "raw") != 0))
    {
        /* a configuration gap, not a fault of this boot: the row names a
           jack this composition never registered (a server or channel
           renamed or removed after the row was written, the raw editor)
           or a codec it does not carry. W, not E: the device runs on
           without this bridge, and the health net must not latch a
           boot_errors fault at every start for a row the user can see
           and fix on the Connections page (the 2026-10-28 field report:
           such a fault, count 22). */
        const char *what = (br->tr == NULL && br->a != NULL &&
                            br->b != NULL) ? "translator" : "endpoint";
        const char *which = (br->a == NULL)   ? cfg->a
                            : (br->b == NULL) ? cfg->b
                                              : cfg->translator;

        snprintf(s_reason[idx], sizeof(s_reason[idx]),
                 "%s '%s' does not exist on this device", what, which);
        ESP_LOGW(TAG, "%s: not built: %s (a server or channel renamed or "
                 "removed? remove the row or add it back)", cfg->name,
                 s_reason[idx]);
        return ESP_ERR_NOT_FOUND;
    }

    if (br->tr != NULL)
    {
        if (br->tr->ctx_init != NULL &&
            (br->tr->ctx_init(br->ctx_a2b) != ESP_OK ||
             br->tr->ctx_init(br->ctx_b2a) != ESP_OK))
        {
            return ESP_FAIL;
        }

        /* reply channel: the encode ctx (b→a) answers the client on side b;
         * the decode ctx (a→b) answers side a. Fill AFTER ctx_init (which
         * zeroed the header). Requires ctx_size >= sizeof(bridge_reply_hdr_t). */
        if (br->tr->wants_reply)
        {
            ((bridge_reply_hdr_t *)br->ctx_b2a)->reply = reply_sink;
            ((bridge_reply_hdr_t *)br->ctx_b2a)->reply_arg = (void *)br->b;
            ((bridge_reply_hdr_t *)br->ctx_a2b)->reply = reply_sink;
            ((bridge_reply_hdr_t *)br->ctx_a2b)->reply_arg = (void *)br->a;
        }
    }

    br->qa = xQueueCreateStatic(BM_QUEUE_DEPTH, sizeof(bridge_chunk_t),
                                s_qa_store[idx], &s_qa_buf[idx]);
    br->qb = xQueueCreateStatic(BM_QUEUE_DEPTH, sizeof(bridge_chunk_t),
                                s_qb_store[idx], &s_qb_buf[idx]);
    /* the set is DYNAMIC (internal heap): under internal-RAM exhaustion
     * it can fail, and a NULL set asserts inside the pump's
     * xQueueSelectFromSet (found live 2026-07-08: boot loop at 7 KB free
     * instead of a degraded bridge) */
    br->set = xQueueCreateSet(2 * BM_QUEUE_DEPTH);

    if (br->set == NULL)
    {
        ESP_LOGE(TAG, "%s: queue set alloc failed (internal RAM low)",
                 cfg->name);
        drop_queues(br);
        return ESP_ERR_NO_MEM;
    }

    xQueueAddToSet(br->qa, br->set);
    xQueueAddToSet(br->qb, br->set);

    esp_err_t err = br->a->subscribe(br->qa);

    if (err == ESP_OK)
    {
        err = br->b->subscribe(br->qb);

        if (err != ESP_OK)
        {
            br->a->unsubscribe(br->qa);
        }
    }

    if (err != ESP_OK)
    {
        drop_queues(br);

        if (err == ESP_ERR_INVALID_STATE)
        {
            /* a single-stream provider already feeding another bridge:
               the BOOT apply cannot check the single-consumer rule before
               the jacks register, so this bridge degrades alone (W, see
               above) */
            snprintf(s_reason[idx], sizeof(s_reason[idx]),
                     "an endpoint is already used by another bridge");
            ESP_LOGW(TAG, "%s: not built: %s", cfg->name, s_reason[idx]);
        }
        else
        {
            ESP_LOGE(TAG, "%s: endpoint subscribe failed (%s)", cfg->name,
                     esp_err_to_name(err));
        }

        return err;
    }

    br->running = true;
    br->in_use = true;
    br->task = xTaskCreateStatic(pump_task, cfg->name,
                                 sizeof(s_stacks[idx]) /
                                     sizeof(s_stacks[idx][0]),
                                 br, 10, s_stacks[idx], &s_tcbs[idx]);
    ESP_LOGI(TAG, "%s: %s <-%s-> %s up", cfg->name, cfg->a, cfg->translator,
             cfg->b);
    return ESP_OK;
}

esp_err_t bm_pump_start_all(void)
{
    memset(s_reason, 0, sizeof(s_reason));

    for (int i = 0; bm_core_bridge_cfg(i) != NULL; i++)
    {
        const bm_bridge_cfg_t *cfg = bm_core_bridge_cfg(i);

        if (!cfg->enabled)
        {
            continue;
        }

        esp_err_t err = build_bridge(i, cfg);

        /* A bridge naming endpoints this composition never registered
           degrades ALONE (already logged): the schema default must not
           take every other bridge down with it in a composition without
           those endpoints. A refused second subscribe (INVALID_STATE
           from a single-stream provider: the BOOT apply can't check
           the single-consumer rule before the jacks register) degrades
           alone too. Anything else (OOM) still aborts the start. */
        if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_STATE)
        {
            continue;
        }

        if (err != ESP_OK)
        {
            bm_pump_stop_all();
            return err;
        }
    }

    return ESP_OK;
}

void bm_pump_stop_all(void)
{
    for (int i = 0; i < BRIDGE_MANAGER_MAX_BRIDGES; i++)
    {
        teardown_bridge(&s_br[i], i);
    }
}

const char *bm_pump_reason(const char *bridge_name)
{
    for (int i = 0; bm_core_bridge_cfg(i) != NULL; i++)
    {
        if (strcmp(bm_core_bridge_cfg(i)->name, bridge_name) == 0)
        {
            return (s_reason[i][0] != '\0') ? s_reason[i] : NULL;
        }
    }

    return NULL;
}

esp_err_t bm_pump_stats(const char *bridge_name, void *stats_out)
{
    for (int i = 0; i < BRIDGE_MANAGER_MAX_BRIDGES; i++)
    {
        if (s_br[i].in_use && strcmp(s_br[i].cfg->name, bridge_name) == 0)
        {
            *(bridge_stats_t *)stats_out = s_br[i].stats;
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}
