/**
 * @file ml_derp.c
 * @brief Unified DERP I/O Task + Connection Management
 *
 * Single task handles BOTH reading and writing to DERP TLS connection.
 * This eliminates the need for a TLS mutex since only one task touches
 * the SSL context. Matches v1's single-threaded DERP model.
 *
 * Architecture:
 * - Poll for incoming DERP frames (TLS read) every iteration
 * - Drain TX queue between reads (TLS write)
 * - No mutex needed: single task owns the SSL context exclusively
 *
 * Backpressure strategy (from tailscaled):
 * When queue is full, dequeue oldest packet and retry up to 3 times.
 * If still full, drop the new packet.
 *
 * Reference: tailscale/wgengine/magicsock/derp.go (runDerpWriter)
 *            tailscale/derp/derphttp/derphttp_client.go
 *
 * WiCAN (2026-11-03): the connection primitives take the connection
 * (ml_derp_conn_t) instead of the context, because the home relay is no
 * longer the only one: a packet for a peer goes through the PEER's home
 * region (ml_derp_pool.c), dialled on demand, the way Tailscale does it.
 * Sent through our own home region alone, a peer homed elsewhere was
 * answered with PeerGone and never heard from us.
 */

#include "microlink_internal.h"
#include "ml_derp_private.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/error.h"
#include "nacl_box.h"
#include <string.h>
#include <errno.h>
#include <fcntl.h>

static const char *TAG = "ml_derp";

/* Timeout for DERP connection handshake operations */
#define DERP_CONNECT_TIMEOUT_MS  10000

/* ============================================================================
 * Custom BIO callbacks for non-blocking TLS I/O
 *
 * These wrap lwIP recv/send with guaranteed timeout behavior.
 * We don't trust mbedtls_net_recv_timeout on lwIP because lwIP's select()
 * can sometimes block indefinitely on ESP32.
 * ========================================================================== */

/**
 * Custom recv with timeout for mbedtls BIO.
 * Uses SO_RCVTIMEO on the socket as the timeout mechanism (simpler than select).
 * Returns bytes read, or MBEDTLS_ERR_SSL_TIMEOUT, MBEDTLS_ERR_SSL_WANT_READ.
 */
static int ml_derp_bio_recv_timeout(void *ctx, unsigned char *buf, size_t len,
                                      uint32_t timeout) {
    int fd = *(int *)ctx;
    if (fd < 0) return MBEDTLS_ERR_NET_INVALID_CONTEXT;

    /* Set SO_RCVTIMEO to the requested timeout.
     * If timeout is 0 (mbedTLS default = "no timeout"), use 10s as a sane
     * default to avoid indefinite blocking on AT sockets. */
    uint32_t effective_timeout = (timeout > 0) ? timeout : DERP_CONNECT_TIMEOUT_MS;
    struct timeval tv;
    tv.tv_sec = effective_timeout / 1000;
    tv.tv_usec = (effective_timeout % 1000) * 1000;
    ml_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int ret = (int)ml_read_sock(fd, buf, len);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return MBEDTLS_ERR_SSL_TIMEOUT;
        }
        if (errno == EPIPE || errno == ECONNRESET) {
            return MBEDTLS_ERR_NET_CONN_RESET;
        }
        if (errno == EINTR) {
            return MBEDTLS_ERR_SSL_WANT_READ;
        }
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return ret;
}

/**
 * Custom send for mbedtls BIO.
 */
static int ml_derp_bio_send(void *ctx, const unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    if (fd < 0) return MBEDTLS_ERR_NET_INVALID_CONTEXT;

    int ret = (int)ml_write_sock(fd, buf, len);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        }
        if (errno == EPIPE || errno == ECONNRESET) {
            return MBEDTLS_ERR_NET_CONN_RESET;
        }
        if (errno == EINTR) {
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        }
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return ret;
}

/* WiCAN: DERP transport I/O, TLS for real Tailscale DERP (:443), plain
 * TCP otherwise (Headscale's embedded DERP is served over plain HTTP).
 * The plain path reuses the mbedTLS BIO callbacks so the error-code
 * semantics (WANT_READ / SSL_TIMEOUT / CONN_RESET) match
 * mbedtls_ssl_read/mbedtls_ssl_write exactly at every call site. */
static int derp_io_read(ml_derp_conn_t *c, unsigned char *buf, size_t len) {
    if (c->use_tls) {
        int ret = mbedtls_ssl_read(&c->ssl, buf, len);
        /* WiCAN (2026-10-31): under TLS 1.3 the relay sends session
         * tickets right after the handshake and mbedtls_ssl_read reports
         * each one as MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET (it is
         * consumed inside the library): no data yet, the same thing as
         * WANT_READ to every reader here. Left as it was, the HTTP upgrade
         * reader took it for a fatal error and closed the relay it had just
         * connected to. */
        if (ret == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
            return MBEDTLS_ERR_SSL_WANT_READ;
        }
        return ret;
    }
    return ml_derp_bio_recv_timeout(&c->sockfd, buf, len, 100);
}

static int derp_io_write(ml_derp_conn_t *c, const unsigned char *buf, size_t len) {
    if (c->use_tls) {
        return mbedtls_ssl_write(&c->ssl, buf, len);
    }
    return ml_derp_bio_send(&c->sockfd, buf, len);
}

/* DERP frame types */
#define DERP_FRAME_SERVER_KEY   0x01
#define DERP_FRAME_CLIENT_INFO  0x02
#define DERP_FRAME_SERVER_INFO  0x03
#define DERP_FRAME_SEND_PACKET  0x04
#define DERP_FRAME_RECV_PACKET  0x05
#define DERP_FRAME_KEEP_ALIVE   0x06
#define DERP_FRAME_NOTE_PREFERRED 0x07
#define DERP_FRAME_PEER_GONE    0x08
#define DERP_FRAME_PING         0x12
#define DERP_FRAME_PONG         0x13

/* DISCO magic bytes: "TS" + sparkles emoji UTF-8 */
static const uint8_t DISCO_MAGIC[6] = { 'T', 'S', 0xf0, 0x9f, 0x92, 0xac };

/* ============================================================================
 * TLS Read/Write Helpers
 * ========================================================================== */

/**
 * Read exactly `len` bytes via TLS with timeout and WANT_READ retry.
 * Returns number of bytes read on success, -1 on error, -2 on timeout.
 */
static int derp_tls_read_all(ml_derp_conn_t *c, uint8_t *data, size_t len, int timeout_ms) {
    size_t received = 0;
    uint64_t start_ms = ml_get_time_ms();

    while (received < len) {
        if (timeout_ms > 0 && (ml_get_time_ms() - start_ms) > (uint64_t)timeout_ms) {
            ESP_LOGW(TAG, "derp_tls_read_all timeout (%d/%d bytes in %dms)",
                     (int)received, (int)len, timeout_ms);
            return -2;
        }

        int ret = derp_io_read(c, data + received, len - received);
        if (ret < 0) {
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
                ret == MBEDTLS_ERR_SSL_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                ESP_LOGW(TAG, "DERP server closed connection");
                return -1;
            }
            ESP_LOGE(TAG, "TLS read failed: -0x%04x", -ret);
            return -1;
        }
        if (ret == 0) {
            ESP_LOGW(TAG, "TLS connection closed by peer (%d/%d bytes)",
                     (int)received, (int)len);
            return -1;
        }
        received += ret;
    }
    return (int)received;
}

/**
 * Read a DERP frame header (5 bytes: type + 4-byte BE length) with timeout.
 */
static esp_err_t derp_recv_frame_header(ml_derp_conn_t *c, uint8_t *type,
                                          uint32_t *len, int timeout_ms) {
    uint8_t header[5];
    int ret = derp_tls_read_all(c, header, 5, timeout_ms);
    if (ret < 0) {
        return (ret == -2) ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }

    *type = header[0];
    *len = ((uint32_t)header[1] << 24) |
           ((uint32_t)header[2] << 16) |
           ((uint32_t)header[3] << 8) |
           (uint32_t)header[4];

    return ESP_OK;
}

/**
 * Write exactly `len` bytes via TLS with WANT_WRITE retry.
 * Returns bytes written on success, -1 on error.
 * No mutex needed: called only from the DERP I/O task.
 */
static int derp_tls_write_all(ml_derp_conn_t *c, const uint8_t *data, size_t len) {
    size_t written = 0;
    int retries = 0;
    const int max_retries = 50;  /* 50 * 10ms = 500ms max */

    while (written < len) {
        int ret = derp_io_write(c, data + written, len - written);
        if (ret < 0) {
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
                ret == MBEDTLS_ERR_SSL_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(10));
                if (++retries > max_retries) {
                    ESP_LOGW(TAG, "TLS write timeout after %d retries", retries);
                    return -1;
                }
                continue;
            }
            ESP_LOGE(TAG, "TLS write failed: -0x%04x", -ret);
            return -1;
        }
        written += ret;
        retries = 0;
    }
    return (int)written;
}

/* Write a complete DERP frame via TLS */
static int derp_write_frame(ml_derp_conn_t *c, uint8_t type,
                             const uint8_t *payload, uint32_t len) {
    /* 5-byte header: 1 type + 4 length (big-endian) */
    uint8_t header[5];
    header[0] = type;
    header[1] = (len >> 24) & 0xFF;
    header[2] = (len >> 16) & 0xFF;
    header[3] = (len >> 8) & 0xFF;
    header[4] = len & 0xFF;

    if (derp_tls_write_all(c, header, 5) < 0) return -1;

    if (len > 0 && payload) {
        if (derp_tls_write_all(c, payload, len) < 0) return -1;
    }
    return 0;
}

/* Send a packet to a peer via DERP (on the given connection) */
int ml_derp_conn_send(ml_derp_conn_t *c, const uint8_t *dest_key,
                      const uint8_t *data, size_t len) {
    /* SendPacket frame: 32-byte dest key + payload */
    size_t frame_len = 32 + len;
    uint8_t *frame = malloc(frame_len);
    if (!frame) return -1;

    memcpy(frame, dest_key, 32);
    memcpy(frame + 32, data, len);

    int ret = derp_write_frame(c, DERP_FRAME_SEND_PACKET, frame, frame_len);
    if (ret < 0) {
        ESP_LOGW(TAG, "derp_send_packet FAILED: dest=%02x%02x%02x%02x len=%d region=%u",
                 dest_key[0], dest_key[1], dest_key[2], dest_key[3], (int)len,
                 (unsigned)c->region);
    }
    free(frame);
    return ret;
}

/* ============================================================================
 * DERP Frame Reading and Dispatch (runs on DERP I/O task)
 * ========================================================================== */

/* Packet classification */
typedef enum {
    PKT_DISCO,
    PKT_STUN,
    PKT_WIREGUARD,
    PKT_UNKNOWN,
} pkt_type_t;

static pkt_type_t classify_packet(const uint8_t *data, size_t len) {
    if (len >= 20 && (data[0] == 0x00 || data[0] == 0x01) && data[1] == 0x01) {
        return PKT_STUN;
    }
    if (len >= 62 && memcmp(data, DISCO_MAGIC, 6) == 0) {
        return PKT_DISCO;
    }
    if (len >= 4) {
        return PKT_WIREGUARD;
    }
    return PKT_UNKNOWN;
}

static void route_derp_packet(microlink_t *ml, uint8_t *data, size_t len,
                               const uint8_t *src_pubkey) {
    pkt_type_t type = classify_packet(data, len);

    ml_rx_packet_t pkt = {
        .data = data,
        .len = len,
        .via_derp = true,
    };
    memcpy(pkt.src_pubkey, src_pubkey, 32);

    QueueHandle_t target = (type == PKT_DISCO) ? ml->disco_rx_queue : ml->wg_rx_queue;
    if (xQueueSend(target, &pkt, 0) != pdTRUE) {
        free(data);
    }
}

/* Dispatch a received DERP frame (from connection c) */
static void dispatch_derp_frame(microlink_t *ml, ml_derp_conn_t *c, uint8_t frame_type,
                                 uint8_t *src_key, uint8_t *payload, size_t payload_len) {
    switch (frame_type) {
    case DERP_FRAME_RECV_PACKET:
        if (payload) {
            ESP_LOGD(TAG, "DERP RecvPacket: %d bytes from %02x%02x%02x%02x, hdr=%02x, region %u",
                     (int)payload_len,
                     src_key[0], src_key[1], src_key[2], src_key[3],
                     payload_len > 0 ? payload[0] : 0xFF, (unsigned)c->region);
            /* the peer is reachable through this region (its reverse route) */
            ml_derp_pool_note_heard(ml, src_key, c->region);
            route_derp_packet(ml, payload, payload_len, src_key);
            return;  /* payload ownership transferred */
        }
        break;

    case DERP_FRAME_KEEP_ALIVE:
        ESP_LOGD(TAG, "DERP KeepAlive received");
        break;

    case DERP_FRAME_PING:
        /* Echo ping data back as PONG directly (single-threaded, safe to write) */
        if (payload && payload_len > 0) {
            ESP_LOGD(TAG, "DERP PING received, sending PONG");
            derp_write_frame(c, DERP_FRAME_PONG, payload, payload_len);
        }
        break;

    case DERP_FRAME_PONG:
        ESP_LOGD(TAG, "DERP PONG received");
        break;

    case DERP_FRAME_PEER_GONE:
        /* the key, then (newer relays) a reason byte: 0 = the peer
         * disconnected from this relay, 1 = it is not here at all (a peer
         * homed in another region, or offline). Either way this region is
         * no route to it any more; the next packet goes through the peer's
         * own region (ml_derp_pool_pick). Expected traffic, debug level. */
        if (payload && payload_len >= 32) {
            ESP_LOGD(TAG, "DERP PeerGone on region %u: %02x%02x%02x%02x reason %u",
                     (unsigned)c->region, payload[0], payload[1], payload[2], payload[3],
                     payload_len >= 33 ? payload[32] : 0u);
            ml_derp_pool_forget_heard(ml, payload, c->region);
        }
        break;

    default:
        ESP_LOGD(TAG, "DERP frame type 0x%02x ignored (%d bytes)",
                 frame_type, (int)payload_len);
        break;
    }

    if (payload) free(payload);
}

/**
 * Try to read one DERP frame from connection c.
 * Uses mbedtls recv_timeout (100ms) so ssl_read never blocks indefinitely.
 * Returns: 1 = frame read and dispatched, 0 = timeout (no data), <0 = error
 */
int ml_derp_conn_poll(microlink_t *ml, ml_derp_conn_t *c) {
    if (!c->connected || c->sockfd < 0) return -1;

    /* Read 5-byte frame header.
     * SO_RCVTIMEO=100ms ensures read() returns within 100ms if no data. */
    uint8_t header[5];
    int n = derp_io_read(c, header, 5);
    if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE ||
        n == MBEDTLS_ERR_SSL_TIMEOUT) {
        return 0;  /* No data available / timeout */
    }
    if (n <= 0) {
        ESP_LOGW(TAG, "DERP header read returned %d (0x%04x)", n, n < 0 ? -n : 0);
        return n;
    }
    if (n < 5) {
        ESP_LOGW(TAG, "DERP partial header: got %d of 5 bytes", n);
        return -1;
    }

    uint8_t frame_type = header[0];
    uint32_t len = (header[1] << 24) | (header[2] << 16) | (header[3] << 8) | header[4];

    uint8_t src_key[32] = {0};
    uint8_t *payload = NULL;
    size_t payload_len = 0;

    c->last_use_ms = ml_get_time_ms();
    if (len == 0) {
        dispatch_derp_frame(ml, c, frame_type, src_key, NULL, 0);
        return 1;
    }

    if (len > 65536) {
        ESP_LOGW(TAG, "DERP frame too large: %lu", (unsigned long)len);
        return -1;
    }

    /* Read frame payload - we already got the header so payload should follow.
     * Use longer timeout (2s) since we KNOW data is coming. */
    uint8_t *buf = ml_psram_malloc(len);
    if (!buf) return -1;

    size_t total_read = 0;
    uint64_t payload_start = ml_get_time_ms();
    while (total_read < len) {
        /* Safety timeout: 5 seconds for payload */
        if (ml_get_time_ms() - payload_start > 5000) {
            ESP_LOGW(TAG, "DERP payload timeout at %d/%lu bytes",
                     (int)total_read, (unsigned long)len);
            free(buf);
            return -1;
        }
        n = derp_io_read(c, buf + total_read, len - total_read);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE ||
            n == MBEDTLS_ERR_SSL_TIMEOUT) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (n <= 0) {
            ESP_LOGW(TAG, "DERP payload read error: %d (0x%04x) at %d/%lu bytes",
                     n, n < 0 ? -n : 0, (int)total_read, (unsigned long)len);
            free(buf);
            return n;
        }
        total_read += n;
    }

    /* For RecvPacket (0x05): first 32 bytes are sender's public key */
    if (frame_type == DERP_FRAME_RECV_PACKET && len > 32) {
        memcpy(src_key, buf, 32);
        payload = malloc(len - 32);
        if (payload) {
            memcpy(payload, buf + 32, len - 32);
            payload_len = len - 32;
        }
        free(buf);
    } else {
        payload = buf;
        payload_len = len;
    }

    dispatch_derp_frame(ml, c, frame_type, src_key, payload, payload_len);
    return 1;
}

/* ============================================================================
 * DERP TX Queue Processing
 * ========================================================================== */

/* Queue a packet for DERP TX with backpressure */
esp_err_t ml_derp_queue_send(microlink_t *ml, const uint8_t *dest_key,
                              const uint8_t *data, size_t len) {
    if (!ml || !dest_key || !data || len == 0) return ESP_ERR_INVALID_ARG;

    uint8_t *pkt_data = malloc(len);
    if (!pkt_data) return ESP_ERR_NO_MEM;
    memcpy(pkt_data, data, len);

    ml_derp_tx_item_t item = {
        .data = pkt_data,
        .len = len,
        .frame_type = DERP_FRAME_SEND_PACKET,
    };
    memcpy(item.dest_pubkey, dest_key, 32);
    /* WiCAN (2026-11-03): the peer's home region decides the connection
     * (ml_derp_pool_pick on the I/O task); 0 = unknown, the home relay */
    item.region = ml_derp_pool_region_of(ml, dest_key);

    /* WG handshake packets (type 1=init, 2=response) get priority: front of queue.
     * This ensures handshake responses aren't delayed behind DISCO pings. */
    bool is_wg_handshake = (len >= 4 && (data[0] == 0x01 || data[0] == 0x02));

    /* Try to send to queue */
    if ((is_wg_handshake ? xQueueSendToFront(ml->derp_tx_queue, &item, 0)
                         : xQueueSend(ml->derp_tx_queue, &item, 0)) == pdTRUE) {
        return ESP_OK;
    }

    /* Queue full - backpressure: drop oldest, retry up to 3 times */
    for (int i = 0; i < 3; i++) {
        ml_derp_tx_item_t dropped;
        if (xQueueReceive(ml->derp_tx_queue, &dropped, 0) == pdTRUE) {
            free(dropped.data);  /* Drop oldest */
        }
        if (xQueueSend(ml->derp_tx_queue, &item, 0) == pdTRUE) {
            return ESP_OK;
        }
    }

    /* Still full after 3 attempts, drop new packet */
    free(pkt_data);
    return ESP_ERR_TIMEOUT;
}

/* ============================================================================
 * Unified DERP I/O Task
 * ========================================================================== */

/* WiCAN (2026-10-31): the wait after a round of failed connects */
#define DERP_BACKOFF_MIN_MS   10000u
#define DERP_BACKOFF_MAX_MS  300000u

static void derp_arm_backoff(microlink_t *ml, uint64_t now_ms) {
    uint32_t next = ml->derp.backoff_ms == 0 ? DERP_BACKOFF_MIN_MS
                                             : ml->derp.backoff_ms * 2u;
    if (next > DERP_BACKOFF_MAX_MS) {
        next = DERP_BACKOFF_MAX_MS;
    }
    ml->derp.backoff_ms = next;
    ml->derp.retry_after_ms = now_ms + next;
    ESP_LOGW(TAG, "DERP relay unreachable, next attempt in %lu s",
             (unsigned long)(next / 1000u));
}

void ml_derp_tx_task(void *arg) {
    microlink_t *ml = (microlink_t *)arg;
    ESP_LOGI(TAG, "DERP I/O task started (Core %d)", xPortGetCoreID());

    uint32_t frames_rx = 0;
    uint32_t frames_tx = 0;
    uint64_t last_status_ms = 0;
    uint32_t loop_count = 0;
    uint64_t last_heartbeat_ms = 0;
    uint64_t connected_since_ms = 0;
    bool verbose_phase = false;  /* verbose logging for first 15s after connect */

    while (!(xEventGroupGetBits(ml->events) & ML_EVT_SHUTDOWN_REQUEST)) {
        loop_count++;
        uint64_t loop_start = ml_get_time_ms();

        /* Unconditional heartbeat - proves task is alive (WiCAN: debug level,
         * it fired every 5 s at W) */
        if (loop_start - last_heartbeat_ms > 5000) {
            ESP_LOGD(TAG, "HEARTBEAT: loop=%lu conn=%d rx=%lu tx=%lu stack_free=%lu",
                     (unsigned long)loop_count, ml->derp.connected,
                     (unsigned long)frames_rx, (unsigned long)frames_tx,
                     (unsigned long)uxTaskGetStackHighWaterMark(NULL));
            last_heartbeat_ms = loop_start;
        }

        /* ---- Periodic status logging (always, even when disconnected) ---- */
        {
            uint64_t now_ms = loop_start;
            if (now_ms - last_status_ms > 10000) {
                ESP_LOGD(TAG, "DERP status: connected=%d fd=%d rx=%lu tx=%lu loops=%lu",
                         ml->derp.connected, ml->derp.sockfd,
                         (unsigned long)frames_rx, (unsigned long)frames_tx,
                         (unsigned long)loop_count);
                last_status_ms = now_ms;
            }
        }

        /* ---- Handle DERP connect request from coord task ----
         * WiCAN (2026-10-31): a round of three attempts that all fail arms a
         * backoff (10 s, doubling, 5 min at most). The request bit stays set
         * through it, so the next round starts by itself when the backoff
         * ends and does not depend on the coordinator asking again (it asks
         * after every map update, which made a dead relay cost three TLS
         * handshakes every few seconds). The attempts log at debug level,
         * the state of the relay once per round. */
        {
            EventBits_t bits = xEventGroupGetBits(ml->events);
            if ((bits & ML_EVT_DERP_CONNECT_REQ) && !ml->derp.connected &&
                loop_start >= ml->derp.retry_after_ms) {
                xEventGroupClearBits(ml->events, ML_EVT_DERP_CONNECT_REQ);
                /* Retry up to 3 times with 2s backoff */
                for (int attempt = 0; attempt < 3 && !ml->derp.connected; attempt++) {
                    if (attempt > 0) {
                        ESP_LOGD(TAG, "DERP connect retry %d/3 in 2s...", attempt + 1);
                        vTaskDelay(pdMS_TO_TICKS(2000));
                    } else {
                        ESP_LOGI(TAG, "DERP connect requested, connecting from I/O task");
                    }
                    if (ml_derp_connect(ml) == ESP_OK) {
                        connected_since_ms = ml_get_time_ms();
                        verbose_phase = true;
                        ml->derp.backoff_ms = 0;
                        break;
                    }
                    ESP_LOGD(TAG, "DERP connect attempt %d failed", attempt + 1);
                }
                if (!ml->derp.connected) {
                    derp_arm_backoff(ml, ml_get_time_ms());
                }
            }
            if (bits & ML_EVT_DERP_RECONNECT) {
                xEventGroupClearBits(ml->events, ML_EVT_DERP_RECONNECT);
                ESP_LOGW(TAG, "DERP reconnect requested (was %s)",
                         ml->derp.connected ? "connected" : "disconnected");
                ml_derp_disconnect(ml);
                verbose_phase = false;
                /* Auto-reconnect after disconnect */
                vTaskDelay(pdMS_TO_TICKS(1000));
                for (int attempt = 0; attempt < 3 && !ml->derp.connected; attempt++) {
                    if (attempt > 0) {
                        ESP_LOGD(TAG, "DERP reconnect retry %d/3 in 2s...", attempt + 1);
                        vTaskDelay(pdMS_TO_TICKS(2000));
                    }
                    if (ml_derp_connect(ml) == ESP_OK) {
                        connected_since_ms = ml_get_time_ms();
                        verbose_phase = true;
                        ml->derp.backoff_ms = 0;
                        break;
                    }
                    ESP_LOGD(TAG, "DERP reconnect attempt %d failed", attempt + 1);
                }
                if (!ml->derp.connected) {
                    /* the relay is gone: the connect-request path owns the
                     * retries from here, under the backoff */
                    derp_arm_backoff(ml, ml_get_time_ms());
                    xEventGroupSetBits(ml->events, ML_EVT_DERP_CONNECT_REQ);
                }
            }
        }

        /* Disable verbose logging after 15s */
        if (verbose_phase && ml_get_time_ms() - connected_since_ms > 15000) {
            verbose_phase = false;
        }

        if (!ml->derp.connected) {
            /* Not connected - just wait and check again */
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* ---- Phase 1: Drain TX queue FIRST (prioritize outgoing) ---- */
        {
            for (int tx_count = 0; tx_count < 8; tx_count++) {
                ml_derp_tx_item_t item;
                if (xQueueReceive(ml->derp_tx_queue, &item, 0) != pdTRUE) {
                    break;
                }
                if (!ml->derp.connected) {
                    free(item.data);
                    continue;
                }
                int ret;
                ml_derp_conn_t *c = &ml->derp;
                if (item.frame_type == DERP_FRAME_SEND_PACKET) {
                    /* WiCAN (2026-11-03): through the peer's own region
                     * (dialled on demand), the home relay when that is
                     * where the peer lives or nothing better is open */
                    c = ml_derp_pool_pick(ml, &item);
                    ESP_LOGD(TAG, "DERP TX: SendPacket %d bytes, dest=%02x%02x%02x%02x, hdr=%02x, region %u",
                             (int)item.len, item.dest_pubkey[0], item.dest_pubkey[1],
                             item.dest_pubkey[2], item.dest_pubkey[3],
                             item.data[0], (unsigned)c->region);
                    ret = ml_derp_conn_send(c, item.dest_pubkey, item.data, item.len);
                } else {
                    ret = derp_write_frame(c, item.frame_type, item.data, item.len);
                }
                if (ret < 0 && c != &ml->derp) {
                    ESP_LOGW(TAG, "relay region %u dropped the connection", (unsigned)c->region);
                    ml_derp_conn_close(c);
                } else if (ret < 0) {
                    ESP_LOGW(TAG, "DERP write failed");
                    ml->derp.connected = false;
                    xEventGroupSetBits(ml->events, ML_EVT_DERP_RECONNECT);
                } else {
                    frames_tx++;
                    c->last_use_ms = ml_get_time_ms();
                }
                free(item.data);
            }
        }

        if (!ml->derp.connected) continue;

        /* ---- Phase 2: Poll for incoming DERP frames ---- */
        {
            int burst;
            for (burst = 0; burst < 4; burst++) {
                int ret = ml_derp_conn_poll(ml, &ml->derp);
                if (ret > 0) {
                    frames_rx++;
                    ml->derp.last_recv_ms = ml_get_time_ms();
                } else if (ret == 0) {
                    break;  /* No more data / timeout */
                } else {
                    ESP_LOGW(TAG, "DERP read error: %d", ret);
                    ml->derp.connected = false;
                    xEventGroupSetBits(ml->events, ML_EVT_DERP_RECONNECT);
                    break;
                }
            }
        }

        /* ---- Phase 3: the connections to the peers' regions (WiCAN
         * 2026-11-03): their frames, and the idle ones closed ---- */
        ml_derp_pool_service(ml, &frames_rx);

        /* Yield briefly */
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    ESP_LOGI(TAG, "DERP I/O task exiting");
    vTaskDelete(NULL);
}

/* ============================================================================
 * DERP Connection Management (called from coord task)
 * ========================================================================== */

/* WiCAN (2026-10-31, issue 1024): every failure exit of ml_derp_connect()
 * from the TLS setup onwards goes through here, so the TLS context (two
 * record buffers and the handshake state, PSRAM) and its config are freed
 * with the socket. Before this, a relay refused at the handshake (the
 * Ed25519 meta certificate, see ml_x509_wrap.c) cost about 21 KB of PSRAM
 * and 230 B of internal RAM per attempt, three attempts per connect
 * request, a request after every map update: the device ran dry within
 * minutes and a task's first name lookup then crashed it. On the plain-TCP
 * path (Headscale's embedded DERP) there is no TLS context to free. */
static esp_err_t derp_connect_fail(ml_derp_conn_t *c, int sock) {
    if (c->use_tls) {
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->ssl_conf);
    }
    ml_close_sock(sock);
    c->sockfd = -1;
    return ESP_FAIL;
}

/* the home relay: where the peers reach this node */
esp_err_t ml_derp_connect(microlink_t *ml) {
    /* WiCAN: never dial a region that isn't in the DERPMap, a custom
     * coordinator (Headscale) serves its own region IDs and the public
     * ML_DERP_HOST fallback is useless there. */
    uint16_t dial_region = ml_effective_derp_region(ml);
    if (dial_region != ml->derp_home_region) {
        ESP_LOGW(TAG, "Home DERP region %d not in map, dialing region %d",
                 ml->derp_home_region, dial_region);
    }

    esp_err_t err = ml_derp_conn_open(ml, &ml->derp, dial_region, true);
    if (err == ESP_OK) {
        xEventGroupSetBits(ml->events, ML_EVT_DERP_CONNECTED);
    }
    return err;
}

/* WiCAN (2026-11-03): one connection to one region: the home one
 * (preferred, where the peers reach us) or a peer's own region
 * (ml_derp_pool.c). Everything from the dial to the DERP handshake. */
esp_err_t ml_derp_conn_open(microlink_t *ml, ml_derp_conn_t *c, uint16_t region,
                            bool preferred) {
    /* Determine DERP host/port from DERPMap with node failover.
     * Always start from node 0 (the first/preferred node in the DERPMap).
     * Only rotate to a different node after a SUCCESSFUL connection drops,
     * NOT on connection failure (to avoid bouncing between nodes). */
    const char *derp_host = ML_DERP_HOST;
    int derp_port = ML_DERP_PORT;
    uint16_t dial_region = region;

    if (ml->derp_region_count > 0 && dial_region > 0) {
        for (int i = 0; i < ml->derp_region_count; i++) {
            if (ml->derp_regions[i].region_id == dial_region) {
                /* Always use the first non-stun-only node (preferred node).
                 * This ensures we connect to the same node as most peers. */
                for (int attempt = 0; attempt < ml->derp_regions[i].node_count; attempt++) {
                    if (!ml->derp_regions[i].nodes[attempt].stun_only &&
                        ml->derp_regions[i].nodes[attempt].hostname[0]) {
                        derp_host = ml->derp_regions[i].nodes[attempt].hostname;
                        if (ml->derp_regions[i].nodes[attempt].derp_port > 0) {
                            derp_port = ml->derp_regions[i].nodes[attempt].derp_port;
                        }
                        break;
                    }
                }
                break;
            }
        }
    }

    int64_t t_derp_start = esp_timer_get_time();

    ESP_LOGI(TAG, "Connecting to DERP %s:%d (region %d)",
             derp_host, derp_port, dial_region ? dial_region : ML_DERP_REGION);

    /* DNS resolve: accept IPv4 or IPv6 (carrier may be IPv6-only) */
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%d", derp_port);

    if (ml_getaddrinfo(derp_host, port_str, &hints, &res) != 0 || !res) {
        ESP_LOGW(TAG, "DNS resolve failed for %s", derp_host);
        return ESP_FAIL;
    }

    int64_t t_derp_dns = esp_timer_get_time();
    ESP_LOGI(TAG, "[TIMING] DERP DNS: %lld ms", (t_derp_dns - t_derp_start) / 1000);

    /* TCP connect: use address family from DNS result */
    int sock = ml_socket(res->ai_family, SOCK_STREAM, 0);
    if (sock < 0) {
        ml_freeaddrinfo(res);
        return ESP_FAIL;
    }

    /* Set connect timeout */
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    ml_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    ml_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (ml_connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        ESP_LOGW(TAG, "TCP connect failed: %d", errno);
        ml_close_sock(sock);
        ml_freeaddrinfo(res);
        return ESP_FAIL;
    }
    ml_freeaddrinfo(res);

    int64_t t_derp_tcp = esp_timer_get_time();
    ESP_LOGI(TAG, "[TIMING] DERP TCP connect: %lld ms", (t_derp_tcp - t_derp_dns) / 1000);

    /* WiCAN: real Tailscale DERP is always TLS on :443; a custom
     * coordinator's embedded DERP (Headscale) is plain HTTP on its
     * control port. Dispatch on the port. */
    c->sockfd = sock;
    c->use_tls = (derp_port == 443);
    int ret = 0;
    int64_t t_derp_tls = t_derp_tcp;
    if (c->use_tls) {
    /* TLS setup */
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->ssl_conf);
    /* WiCAN/IDF v6 (mbedtls 4): global PSA RNG replaces the
     * entropy+ctr_drbg pair and mbedtls_ssl_conf_rng, see PROVENANCE */
    psa_crypto_init();

    mbedtls_ssl_config_defaults(&c->ssl_conf,
                                 MBEDTLS_SSL_IS_CLIENT,
                                 MBEDTLS_SSL_TRANSPORT_STREAM,
                                 MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_conf_authmode(&c->ssl_conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_read_timeout(&c->ssl_conf, DERP_CONNECT_TIMEOUT_MS);

    if (mbedtls_ssl_setup(&c->ssl, &c->ssl_conf) != 0) {
        ESP_LOGW(TAG, "TLS setup failed (no memory for the record buffers?)");
        return derp_connect_fail(c, sock);
    }
    mbedtls_ssl_set_hostname(&c->ssl, derp_host);
    /* Store socket fd BEFORE setting bio.
     * Use custom BIO callbacks that route through ml_read_sock/ml_write_sock,
     * which transparently support both lwIP and AT socket backends.
     * Timeout is handled via SO_RCVTIMEO. */
    mbedtls_ssl_set_bio(&c->ssl, &c->sockfd,
                         ml_derp_bio_send, NULL, ml_derp_bio_recv_timeout);

    /* TLS handshake - socket has 10s SO_RCVTIMEO from connect phase. */
    while ((ret = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        char err_buf[128];
        mbedtls_strerror(ret, err_buf, sizeof(err_buf));
        ESP_LOGW(TAG, "TLS handshake failed: %s", err_buf);
        return derp_connect_fail(c, sock);
    }

    t_derp_tls = esp_timer_get_time();
    ESP_LOGI(TAG, "[TIMING] DERP TLS handshake: %lld ms", (t_derp_tls - t_derp_tcp) / 1000);
    ESP_LOGI(TAG, "TLS connected to DERP");
    } else {
        ESP_LOGI(TAG, "Plain-TCP DERP (port %d, e.g. Headscale embedded DERP)", derp_port);
    }

    /* HTTP Upgrade: GET /derp with Upgrade: DERP header */
    char upgrade_req[256];
    snprintf(upgrade_req, sizeof(upgrade_req),
             "GET /derp HTTP/1.1\r\n"
             "Host: %s\r\n"
             "Connection: Upgrade\r\n"
             "Upgrade: DERP\r\n"
             "\r\n",
             derp_host);

    ret = derp_io_write(c, (const uint8_t *)upgrade_req, strlen(upgrade_req));
    if (ret < 0) {
        ESP_LOGW(TAG, "Failed to send HTTP upgrade");
        return derp_connect_fail(c, sock);
    }

    /* Read HTTP response byte-by-byte until \r\n\r\n to avoid over-reading
     * into the DERP binary frame stream (matching v1 approach) */
    {
        uint8_t resp_buf[512];
        int resp_len = 0;
        bool found_end = false;
        uint64_t http_start = ml_get_time_ms();

        while (resp_len < (int)sizeof(resp_buf) - 1) {
            if (ml_get_time_ms() - http_start > DERP_CONNECT_TIMEOUT_MS) {
                ESP_LOGW(TAG, "HTTP upgrade response timeout");
                return derp_connect_fail(c, sock);
            }

            ret = derp_io_read(c, resp_buf + resp_len, 1);
            if (ret < 0) {
                if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
                    ret == MBEDTLS_ERR_SSL_TIMEOUT) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
                ESP_LOGW(TAG, "HTTP upgrade read failed: -0x%04x", -ret);
                return derp_connect_fail(c, sock);
            }
            if (ret == 0) {
                ESP_LOGW(TAG, "Connection closed during HTTP upgrade");
                return derp_connect_fail(c, sock);
            }
            resp_len++;

            /* Check for \r\n\r\n */
            if (resp_len >= 4 &&
                resp_buf[resp_len - 4] == '\r' && resp_buf[resp_len - 3] == '\n' &&
                resp_buf[resp_len - 2] == '\r' && resp_buf[resp_len - 1] == '\n') {
                found_end = true;
                break;
            }
        }

        resp_buf[resp_len] = '\0';

        if (!found_end || strstr((char *)resp_buf, "101") == NULL) {
            ESP_LOGW(TAG, "DERP upgrade rejected: %.100s", resp_buf);
            return derp_connect_fail(c, sock);
        }
        ESP_LOGI(TAG, "HTTP 101 Switching Protocols received");
    }

    /* Match v1 exactly: O_NONBLOCK + short SO_RCVTIMEO + SO_SNDTIMEO.
     * O_NONBLOCK ensures read()/write() never block indefinitely.
     * SO_RCVTIMEO provides 100ms polling rhythm for reads.
     * SO_SNDTIMEO prevents writes from blocking too long. */
    {
        int flags = ml_fcntl(sock, F_GETFL, 0);
        if (flags >= 0) {
            ml_fcntl(sock, F_SETFL, flags | O_NONBLOCK);
        }
        struct timeval io_tv = { .tv_sec = 0, .tv_usec = 100000 };  /* 100ms */
        ml_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &io_tv, sizeof(io_tv));
        ml_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &io_tv, sizeof(io_tv));
    }

    /* ========================================================
     * DERP Handshake: ServerKey -> ClientInfo -> ServerInfo
     * ======================================================== */

    /* Step 1: Read ServerKey frame header using reliable read helper */
    uint8_t frame_type;
    uint32_t frame_len;
    esp_err_t err = derp_recv_frame_header(c, &frame_type, &frame_len, DERP_CONNECT_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read ServerKey frame header (err=%d)", err);
        return derp_connect_fail(c, sock);
    }

    if (frame_type != DERP_FRAME_SERVER_KEY || frame_len < 40) {
        ESP_LOGW(TAG, "Expected ServerKey frame (0x01), got 0x%02x len=%lu",
                 frame_type, (unsigned long)frame_len);
        return derp_connect_fail(c, sock);
    }

    /* Read and verify 8-byte magic */
    uint8_t magic[8];
    static const uint8_t DERP_MAGIC[8] = {0x44, 0x45, 0x52, 0x50, 0xf0, 0x9f, 0x94, 0x91};
    if (derp_tls_read_all(c, magic, 8, DERP_CONNECT_TIMEOUT_MS) < 0) {
        ESP_LOGW(TAG, "Failed to read ServerKey magic");
        return derp_connect_fail(c, sock);
    }

    if (memcmp(magic, DERP_MAGIC, 8) != 0) {
        ESP_LOGW(TAG, "Invalid DERP magic: %02x%02x%02x%02x%02x%02x%02x%02x",
                 magic[0], magic[1], magic[2], magic[3],
                 magic[4], magic[5], magic[6], magic[7]);
        return derp_connect_fail(c, sock);
    }
    ESP_LOGI(TAG, "DERP magic verified");

    /* Read 32-byte server public key */
    uint8_t derp_server_key[32];
    if (derp_tls_read_all(c, derp_server_key, 32, DERP_CONNECT_TIMEOUT_MS) < 0) {
        ESP_LOGW(TAG, "Failed to read server key");
        return derp_connect_fail(c, sock);
    }

    ESP_LOGI(TAG, "DERP server key received (first 8): %02x%02x%02x%02x%02x%02x%02x%02x",
             derp_server_key[0], derp_server_key[1], derp_server_key[2], derp_server_key[3],
             derp_server_key[4], derp_server_key[5], derp_server_key[6], derp_server_key[7]);

    /* Skip remaining bytes if frame_len > 40 */
    if (frame_len > 40) {
        uint8_t skip_buf[64];
        size_t remaining = frame_len - 40;
        while (remaining > 0) {
            size_t chunk = remaining > sizeof(skip_buf) ? sizeof(skip_buf) : remaining;
            if (derp_tls_read_all(c, skip_buf, chunk, DERP_CONNECT_TIMEOUT_MS) < 0) break;
            remaining -= chunk;
        }
    }

    /* Step 2: Send ClientInfo frame (type 0x02)
     * Payload: [our_nodekey(32)][nonce(24)][nacl_box(JSON)] */
    {
        const char *client_info_json = "{\"Version\":2,\"CanAckPings\":true,\"IsProber\":false}";
        size_t json_len = strlen(client_info_json);

        /* Generate random nonce */
        uint8_t nonce[NACL_BOX_NONCEBYTES];
        esp_fill_random(nonce, NACL_BOX_NONCEBYTES);

        /* Encrypt JSON with NaCl box: our WG private key -> DERP server public key */
        size_t ciphertext_len = json_len + NACL_BOX_MACBYTES;
        uint8_t *ciphertext = malloc(ciphertext_len);
        if (!ciphertext) {
            return derp_connect_fail(c, sock);
        }

        if (nacl_box(ciphertext,
                     (const uint8_t *)client_info_json, json_len,
                     nonce,
                     derp_server_key,       /* recipient: DERP server */
                     ml->wg_private_key     /* sender: our WG node key */
                     ) != 0) {
            ESP_LOGW(TAG, "NaCl box encrypt failed");
            free(ciphertext);
            return derp_connect_fail(c, sock);
        }

        /* Build ClientInfo frame payload: nodekey(32) + nonce(24) + ciphertext */
        size_t ci_payload_len = 32 + NACL_BOX_NONCEBYTES + ciphertext_len;
        uint8_t *ci_payload = malloc(ci_payload_len);
        if (!ci_payload) {
            free(ciphertext);
            return derp_connect_fail(c, sock);
        }

        memcpy(ci_payload, ml->wg_public_key, 32);
        memcpy(ci_payload + 32, nonce, NACL_BOX_NONCEBYTES);
        memcpy(ci_payload + 32 + NACL_BOX_NONCEBYTES, ciphertext, ciphertext_len);
        free(ciphertext);

        ESP_LOGI(TAG, "DERP ClientInfo node_key=%02x%02x%02x%02x%02x%02x%02x%02x",
                 ml->wg_public_key[0], ml->wg_public_key[1],
                 ml->wg_public_key[2], ml->wg_public_key[3],
                 ml->wg_public_key[4], ml->wg_public_key[5],
                 ml->wg_public_key[6], ml->wg_public_key[7]);

        /* Send ClientInfo frame */
        if (derp_write_frame(c, DERP_FRAME_CLIENT_INFO, ci_payload, ci_payload_len) < 0) {
            ESP_LOGW(TAG, "Failed to send ClientInfo");
            free(ci_payload);
            return derp_connect_fail(c, sock);
        }
        free(ci_payload);

        ESP_LOGI(TAG, "ClientInfo sent");
    }

    /* Step 3: Read ServerInfo frame (type 0x03) */
    {
        uint8_t si_type;
        uint32_t si_len;
        err = derp_recv_frame_header(c, &si_type, &si_len, DERP_CONNECT_TIMEOUT_MS);
        if (err == ESP_OK && si_type == DERP_FRAME_SERVER_INFO && si_len > 0) {
            /* Read and discard ServerInfo payload */
            uint8_t *si_buf = malloc(si_len);
            if (si_buf) {
                derp_tls_read_all(c, si_buf, si_len, DERP_CONNECT_TIMEOUT_MS);
                free(si_buf);
            }
            ESP_LOGI(TAG, "ServerInfo received (discarded)");
        } else if (err != ESP_OK) {
            ESP_LOGW(TAG, "No ServerInfo frame (continuing anyway)");
        }
    }

    /* Send NotePreferred (type 0x07): this is our preferred DERP (the home
     * connection only; a peer's region is not where we live) */
    if (preferred) {
        uint8_t pref = 0x01;
        derp_write_frame(c, DERP_FRAME_NOTE_PREFERRED, &pref, 1);
    }

    /* Switch socket to short timeout for data phase.
     * Long timeout was needed for TLS handshake, but polling must be fast. */
    {
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };  /* 200ms */
        ml_setsockopt(c->sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        mbedtls_ssl_conf_read_timeout(&c->ssl_conf, 200);
    }

    c->connected = true;
    c->region = region;
    c->last_recv_ms = ml_get_time_ms();
    c->last_use_ms = c->last_recv_ms;

    int64_t t_derp_done = esp_timer_get_time();
    ESP_LOGI(TAG, "[TIMING] DERP total: %lld ms (DNS=%lld, TCP=%lld, TLS=%lld, proto=%lld)",
             (t_derp_done - t_derp_start) / 1000,
             (t_derp_dns - t_derp_start) / 1000,
             (t_derp_tcp - t_derp_dns) / 1000,
             (t_derp_tls - t_derp_tcp) / 1000,
             (t_derp_done - t_derp_tls) / 1000);
    ESP_LOGI(TAG, "DERP handshake complete, connected (internal RAM free %u B, min %u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}

void ml_derp_conn_close(ml_derp_conn_t *c) {
    c->connected = false;
    if (c->sockfd >= 0) {
        if (c->use_tls) {
            mbedtls_ssl_close_notify(&c->ssl);
            mbedtls_ssl_free(&c->ssl);
            mbedtls_ssl_config_free(&c->ssl_conf);
        }
        ml_close_sock(c->sockfd);
        c->sockfd = -1;
    }
}

void ml_derp_disconnect(microlink_t *ml) {
    ml_derp_conn_close(&ml->derp);
    xEventGroupClearBits(ml->events, ML_EVT_DERP_CONNECTED);
    /* WiCAN (2026-11-03): the peers' region connections go with the home one */
    ml_derp_pool_close_all(ml);

    /* Drain TX queue */
    ml_derp_tx_item_t item;
    while (xQueueReceive(ml->derp_tx_queue, &item, 0) == pdTRUE) {
        free(item.data);
    }

    ESP_LOGI(TAG, "DERP disconnected");
}
