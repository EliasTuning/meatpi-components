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
 * @file socket_manager_net.c
 * @brief The net task: one shared lwIP select() loop multiplexing every
 *        server (README: 1×6 KB PSRAM stack beats 4×4 KB per-server tasks;
 *        the loop touches no flash/DMA so a PSRAM stack is safe per §2).
 *
 * Robustness (spec §2): listener creation failures retry with bounded
 * backoff (pure smp_backoff_next_ms progression, never busy-spins); a client
 * is closed on a hard send error (reset, broken pipe) without disturbing
 * the server. A short or timed-out send keeps the client and the unsent
 * tail. At max_clients the newest connection is accept-then-closed (counted
 * as refused). A subscriber's full queue drops that chunk and counts it:
 * RX never blocks.
 */
#include <errno.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "socket_manager.h"
#include "socket_manager_private.h"

static const char *TAG = "socket_manager";

#define SM_SELECT_TICK_MS 200
#define SM_SEND_TIMEOUT_MS 100
/* One coalesced bridge send (bridge_manager BM_COALESCE_MAX). A short
 * write keeps at most this many bytes and tries them again next time. */
#define SM_PENDING_MAX 1024

typedef struct
{
    int    fd;                 /* listener (TCP) or bound socket (UDP); -1 */
    int    clients[SOCKET_MANAGER_MAX_CLIENTS];
    uint8_t  pend[SOCKET_MANAGER_MAX_CLIENTS][SM_PENDING_MAX];
    uint16_t pend_len[SOCKET_MANAGER_MAX_CLIENTS];
    uint32_t generation[SOCKET_MANAGER_MAX_CLIENTS];
    struct sockaddr_in last_peer;
    bool   has_peer;
    QueueHandle_t sub;
    socket_stats_t stats;
    uint32_t backoff_ms;
    int64_t  retry_at_us;
} sm_server_t;

static sm_server_t s_srv[SOCKET_MANAGER_MAX_SERVERS] EXT_RAM_BSS_ATTR;

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;      /* internal: FreeRTOS object */

static TaskHandle_t s_task;
static StaticTask_t s_tcb;                /* internal: FreeRTOS object */
static StackType_t  s_stack[6144] EXT_RAM_BSS_ATTR;
static volatile bool s_running;

/* ---- socket helpers -------------------------------------------------------- */

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);

    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void set_keepalive(int fd, uint16_t idle_s)
{
    int on = 1;
    int idle = (idle_s > 0) ? idle_s : 30;
    int intvl = 5;
    int cnt = 3;

    /* interactive request/response streams (ELM327 apps): a response
       often leaves the chip as two small writes (data line, then the
       prompt after its multi-ECU wait) - with Nagle on, the second one
       sits until the peer ACKs the first (delayed ACK: 40-200 ms). Ship
       every write at once; the bridge already batches by chunk. */
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));

    struct timeval tv = { .tv_sec = 0,
                          .tv_usec = SM_SEND_TIMEOUT_MS * 1000 };

    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/** Create + bind (+listen) one server socket. Plaintext, INADDR_ANY (v1). */
static int open_listener(const smp_server_cfg_t *cfg)
{
    int fd = socket(AF_INET, cfg->is_udp ? SOCK_DGRAM : SOCK_STREAM,
                    cfg->is_udp ? IPPROTO_UDP : IPPROTO_TCP);

    if (fd < 0)
    {
        return -1;
    }

    int on = 1;

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in addr =
    {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(cfg->port),
    };

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        (!cfg->is_udp && listen(fd, 2) != 0))
    {
        close(fd);
        return -1;
    }

    set_nonblock(fd);
    return fd;
}

static void close_client(sm_server_t *srv, int slot)
{
    if (srv->clients[slot] >= 0)
    {
        close(srv->clients[slot]);
        srv->clients[slot] = -1;
        srv->stats.clients--;
    }
    srv->pend_len[slot] = 0;
    srv->generation[slot]++;
}

static int client_count(const sm_server_t *srv)
{
    int n = 0;

    for (int i = 0; i < SOCKET_MANAGER_MAX_CLIENTS; i++)
    {
        n += (srv->clients[i] >= 0) ? 1 : 0;
    }

    return n;
}

/* ---- RX paths (net task context) -------------------------------------------- */

static void deliver(sm_server_t *srv, const uint8_t *data, int len)
{
    srv->stats.bytes_in += (uint32_t)len;

    if (srv->sub == NULL)
    {
        return; /* nobody attached yet: bytes fall on the floor by design */
    }

    socket_chunk_t chunk;

    while (len > 0)
    {
        int n = (len > SOCKET_MANAGER_CHUNK_SIZE)
                    ? SOCKET_MANAGER_CHUNK_SIZE : len;

        chunk.len = (uint16_t)n;
        memcpy(chunk.data, data, (size_t)n);

        if (xQueueSend(srv->sub, &chunk, 0) != pdTRUE)
        {
            srv->stats.rx_drops++; /* never block the net task (§9.5 family) */
        }

        data += n;
        len -= n;
    }
}

static void handle_tcp_accept(sm_server_t *srv, const smp_server_cfg_t *cfg)
{
    int fd = accept(srv->fd, NULL, NULL);

    if (fd < 0)
    {
        return;
    }

    if (smp_accept_decision(client_count(srv), cfg->max_clients)
            == SMP_REJECT)
    {
        close(fd); /* accept-then-close policy (§2), counted */
        srv->stats.refused++;
        ESP_LOGD(TAG, "%s: refused client (max %u)", cfg->name,
                 (unsigned)cfg->max_clients);
        return;
    }

    set_nonblock(fd);
    set_keepalive(fd, cfg->keepalive_s);

    for (int i = 0; i < SOCKET_MANAGER_MAX_CLIENTS; i++)
    {
        if (srv->clients[i] < 0)
        {
            srv->clients[i] = fd;
            srv->pend_len[i] = 0;
            srv->stats.clients++;
            ESP_LOGI(TAG, "%s: client connected (%d total)", cfg->name,
                     client_count(srv));

            if (srv->sub == NULL)
            {
                /* a server nobody bridged accepts clients and discards
                   their bytes (deliver): a field report read that as
                   "slcan connects but never receives" (2026-10-16) with
                   nothing in the log to say why. Say it where the user
                   looks; once per connect, like the line above. */
                ESP_LOGW(TAG, "%s: no connection uses this server, its "
                         "data goes nowhere (Settings, Connections)",
                         cfg->name);
            }
            return;
        }
    }

    close(fd); /* unreachable: verdict already bounded us */
}

static void handle_tcp_rx(sm_server_t *srv, const smp_server_cfg_t *cfg,
                          int slot)
{
    /* internal scratch is fine on our PSRAM stack: recv copies via lwIP */
    uint8_t buf[SOCKET_MANAGER_CHUNK_SIZE];
    int n = recv(srv->clients[slot], buf, sizeof(buf), 0);

    if (n <= 0)
    {
        ESP_LOGI(TAG, "%s: client closed (%d left)", cfg->name,
                 client_count(srv) - 1);
        close_client(srv, slot);
        return;
    }

    deliver(srv, buf, n);
}

static void handle_udp_rx(sm_server_t *srv, const smp_server_cfg_t *cfg)
{
    uint8_t buf[SOCKET_MANAGER_CHUNK_SIZE];
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    int n = recvfrom(srv->fd, buf, sizeof(buf), 0,
                     (struct sockaddr *)&from, &from_len);

    if (n <= 0)
    {
        return;
    }

    if (!srv->has_peer && srv->sub == NULL)
    {
        /* the first datagram of the boot on a server nobody bridged:
           the same hint as the TCP accept path, once */
        ESP_LOGW(TAG, "%s: no connection uses this server, its data goes "
                 "nowhere (Settings, Connections)", cfg->name);
    }

    srv->last_peer = from; /* UDP "connection": TX targets the last peer */
    srv->has_peer = true;
    srv->stats.clients = 1;
    deliver(srv, buf, n);
}

/* ---- the net task ------------------------------------------------------------ */

static void net_task(void *arg)
{
    (void)arg;
    ESP_LOGD(TAG, "net task up");

    while (s_running)
    {
        fd_set rfds;
        int max_fd = -1;

        FD_ZERO(&rfds);
        xSemaphoreTake(s_lock, portMAX_DELAY);

        for (int i = 0; sm_core_config(i) != NULL; i++)
        {
            const smp_server_cfg_t *cfg = sm_core_config(i);
            sm_server_t *srv = &s_srv[i];

            if (!cfg->enabled)
            {
                continue;
            }

            /* (re)create the listener with bounded backoff */
            if (srv->fd < 0 && esp_timer_get_time() >= srv->retry_at_us)
            {
                srv->fd = open_listener(cfg);

                if (srv->fd >= 0)
                {
                    srv->backoff_ms = 0;
                    srv->stats.reconnects++;
                    ESP_LOGI(TAG, "%s: listening on %s:%u", cfg->name,
                             cfg->is_udp ? "udp" : "tcp",
                             (unsigned)cfg->port);
                }
                else
                {
                    srv->backoff_ms = smp_backoff_next_ms(srv->backoff_ms);
                    srv->retry_at_us = esp_timer_get_time() +
                                       (int64_t)srv->backoff_ms * 1000;
                    ESP_LOGD(TAG, "%s: listen failed; retry in %lu ms",
                             cfg->name, (unsigned long)srv->backoff_ms);
                }
            }

            if (srv->fd >= 0)
            {
                FD_SET(srv->fd, &rfds);
                max_fd = (srv->fd > max_fd) ? srv->fd : max_fd;
            }

            for (int c = 0; c < SOCKET_MANAGER_MAX_CLIENTS; c++)
            {
                if (srv->clients[c] >= 0)
                {
                    FD_SET(srv->clients[c], &rfds);
                    max_fd = (srv->clients[c] > max_fd) ? srv->clients[c]
                                                        : max_fd;
                }
            }
        }

        xSemaphoreGive(s_lock);

        struct timeval tv = { .tv_sec = 0,
                              .tv_usec = SM_SELECT_TICK_MS * 1000 };
        int ready = select(max_fd + 1, &rfds, NULL, NULL, &tv);

        if (ready <= 0)
        {
            continue; /* timeout tick (drives the backoff retries) */
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);

        for (int i = 0; sm_core_config(i) != NULL; i++)
        {
            const smp_server_cfg_t *cfg = sm_core_config(i);
            sm_server_t *srv = &s_srv[i];

            if (!cfg->enabled || srv->fd < 0)
            {
                continue;
            }

            if (FD_ISSET(srv->fd, &rfds))
            {
                if (cfg->is_udp)
                {
                    handle_udp_rx(srv, cfg);
                }
                else
                {
                    handle_tcp_accept(srv, cfg);
                }
            }

            for (int c = 0; c < SOCKET_MANAGER_MAX_CLIENTS; c++)
            {
                if (srv->clients[c] >= 0 &&
                    FD_ISSET(srv->clients[c], &rfds))
                {
                    handle_tcp_rx(srv, cfg, c);
                }
            }
        }

        xSemaphoreGive(s_lock);
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

/* ---- internal API -------------------------------------------------------------- */

esp_err_t sm_net_start(void)
{
    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }

    for (int i = 0; i < SOCKET_MANAGER_MAX_SERVERS; i++)
    {
        s_srv[i].fd = -1;

        for (int c = 0; c < SOCKET_MANAGER_MAX_CLIENTS; c++)
        {
            s_srv[i].clients[c] = -1;
        }
    }

    s_running = true;
    s_task = xTaskCreateStatic(net_task, "socket_net",
                               sizeof(s_stack) / sizeof(s_stack[0]), NULL,
                               10, s_stack, &s_tcb);
    return (s_task != NULL) ? ESP_OK : ESP_FAIL;
}

void sm_net_stop(void)
{
    s_running = false;

    for (int i = 0; i < 25 && s_task != NULL; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    for (int i = 0; i < SOCKET_MANAGER_MAX_SERVERS; i++)
    {
        for (int c = 0; c < SOCKET_MANAGER_MAX_CLIENTS; c++)
        {
            close_client(&s_srv[i], c);
        }

        if (s_srv[i].fd >= 0)
        {
            close(s_srv[i].fd);
            s_srv[i].fd = -1;
        }

        s_srv[i].has_peer = false;
        s_srv[i].sub = NULL;
    }

    xSemaphoreGive(s_lock);
}

esp_err_t sm_net_subscribe(int idx, void *queue_handle)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);

    esp_err_t err = ESP_OK;

    if (s_srv[idx].sub != NULL)
    {
        err = ESP_ERR_INVALID_STATE; /* one subscriber per server (README) */
    }
    else
    {
        s_srv[idx].sub = (QueueHandle_t)queue_handle;
    }

    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t sm_net_unsubscribe(int idx, void *queue_handle)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);

    esp_err_t err = ESP_ERR_NOT_FOUND;

    if (s_srv[idx].sub == (QueueHandle_t)queue_handle)
    {
        s_srv[idx].sub = NULL;
        err = ESP_OK;
    }

    xSemaphoreGive(s_lock);
    return err;
}

static bool send_fatal(int n)
{
    if (n >= 0)
    {
        return false;
    }

    switch (errno)
    {
    case EPIPE:
    case ECONNRESET:
    case ENOTCONN:
    case EBADF:
    case ECONNABORTED:
    case ENETDOWN:
    case ENETRESET:
        return true;
    default:
        return false; /* EAGAIN / EWOULDBLOCK / ETIMEDOUT: keep the client */
    }
}

/** Blocking send outside s_lock. >=0 is bytes written (0 = timed out).
 *  -1 is a hard error; the caller closes the client. */
static int write_some(int fd, const uint8_t *buf, size_t len)
{
    int fl = fcntl(fd, F_GETFL, 0);
    int n;

    fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    n = send(fd, buf, len, 0);
    fcntl(fd, F_SETFL, fl);

    if (n < 0)
    {
        return send_fatal(n) ? -1 : 0;
    }

    return n;
}

static void store_tail(sm_server_t *srv, int slot, const uint8_t *buf,
                       size_t len, int wrote)
{
    size_t left = len - (size_t)wrote;

    if (left > SM_PENDING_MAX)
    {
        left = SM_PENDING_MAX;
        srv->stats.tx_drops++;
    }

    memcpy(srv->pend[slot], buf + wrote, left);
    srv->pend_len[slot] = (uint16_t)left;
}

esp_err_t sm_net_send(int idx, const uint8_t *data, size_t len)
{
    const smp_server_cfg_t *cfg = sm_core_config(idx);
    sm_server_t *srv = &s_srv[idx];

    if (cfg == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    esp_err_t err = ESP_OK;

    if (cfg->is_udp)
    {
        if (!srv->has_peer || srv->fd < 0)
        {
            err = ESP_ERR_INVALID_STATE; /* no datagram heard yet */
        }
        else if (sendto(srv->fd, data, len, 0,
                        (struct sockaddr *)&srv->last_peer,
                        sizeof(srv->last_peer)) != (int)len)
        {
            err = ESP_FAIL;
        }
        else
        {
            srv->stats.bytes_out += (uint32_t)len;
        }

        xSemaphoreGive(s_lock);
        return err;
    }

    /* Fan-out. The lock is dropped around send() so the net task can still
     * accept and read. A short write keeps the tail; only a hard error
     * closes that client. */
    for (int c = 0; c < SOCKET_MANAGER_MAX_CLIENTS; c++)
    {
        int fd;
        uint32_t gen;
        uint8_t pending_copy[SM_PENDING_MAX];
        uint16_t pending_n;
        int n;

        if (srv->clients[c] < 0)
        {
            continue;
        }

        fd = srv->clients[c];
        gen = srv->generation[c];
        pending_n = srv->pend_len[c];

        if (pending_n > 0)
        {
            memcpy(pending_copy, srv->pend[c], pending_n);
        }

        xSemaphoreGive(s_lock);
        n = (pending_n > 0) ? write_some(fd, pending_copy, pending_n) : 0;
        xSemaphoreTake(s_lock, portMAX_DELAY);

        if (srv->clients[c] != fd || srv->generation[c] != gen)
        {
            continue; /* closed or replaced while we were in send() */
        }

        if (pending_n > 0)
        {
            if (n < 0)
            {
                close_client(srv, c);
                ESP_LOGW(TAG, "%s: client dropped (send error)", cfg->name);
                continue;
            }

            if ((size_t)n < pending_n)
            {
                store_tail(srv, c, pending_copy, pending_n, n);
                srv->stats.bytes_out += (uint32_t)n;
                srv->stats.tx_drops++; /* this call's new bytes are not queued */
                continue;
            }

            srv->pend_len[c] = 0;
            srv->stats.bytes_out += (uint32_t)n;
        }

        if (len == 0)
        {
            continue;
        }

        xSemaphoreGive(s_lock);
        n = write_some(fd, data, len);
        xSemaphoreTake(s_lock, portMAX_DELAY);

        if (srv->clients[c] != fd || srv->generation[c] != gen)
        {
            continue;
        }

        if (n < 0)
        {
            close_client(srv, c);
            ESP_LOGW(TAG, "%s: client dropped (send error)", cfg->name);
            continue;
        }

        srv->stats.bytes_out += (uint32_t)n;

        if ((size_t)n < len)
        {
            store_tail(srv, c, data, len, n);
        }
    }

    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t sm_net_stats(int idx, void *stats_out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *(socket_stats_t *)stats_out = s_srv[idx].stats;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
