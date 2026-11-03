/**
 * @file ml_derp_pool.c
 * @brief WiCAN (2026-11-03): the relay connections to the peers' own regions
 *
 * Tailscale sends to a peer through the PEER's home relay region (its
 * HomeDERP in the map), dialling that region when no connection to it is
 * open, or through a region where that peer was heard from (its reverse
 * route); a node's own home connection is where the other peers reach it.
 * Upstream microlink sent everything through its own home region, which
 * answers PeerGone (not here) for a peer homed anywhere else, so the first
 * packet to a peer that had not contacted the device never arrived: a
 * subnet router at home, the WiCAN on a phone hotspot, and the broker
 * behind the router "won't connect" while the VPN page said connected.
 * Peers that contact the device first are reachable either way: they are
 * clients of the device's home region for as long as they talk to it.
 *
 * The pool: up to ML_DERP_EXTRA_CONNS connections beside the home one, each
 * to one region, opened by the first packet for a peer in that region and
 * closed after ML_DERP_EXTRA_IDLE_MS without a frame; a region that refuses
 * the dial waits a doubling backoff (30 s to 5 min) and its packets go
 * through the home relay meanwhile (the reverse route, when the peer is
 * there). Everything here runs on the DERP I/O task, the owner of every
 * relay socket; the peer table is only read, for a peer's region.
 *
 * Reference: tailscale/wgengine/magicsock/derp.go (derpWriteChanForRegion,
 * addDerpPeerRoute, removeDerpPeerRoute), tailscale/derp/derp_server.go
 * (PeerGoneReasonNotHere).
 */

#include "ml_derp_private.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include <string.h>

static const char *TAG = "ml_derp";

#define POOL_BACKOFF_MIN_MS   30000u
#define POOL_BACKOFF_MAX_MS  300000u
/* an extra connection busy within this window is not closed to make room */
#define POOL_BUSY_MS          10000u

void ml_derp_pool_reset(microlink_t *ml) {
    for (int i = 0; i < ML_DERP_EXTRA_CONNS; i++) {
        memset(&ml->derp_extra[i], 0, sizeof(ml->derp_extra[i]));
        ml->derp_extra[i].sockfd = -1;
    }
    ml->derp_heard_count = 0;
}

uint16_t ml_derp_pool_region_of(microlink_t *ml, const uint8_t *key) {
    for (int i = 0; i < ml->peer_count; i++) {
        if (ml->peers[i].active && memcmp(ml->peers[i].public_key, key, 32) == 0) {
            return ml->peers[i].derp_region;
        }
    }
    return 0;
}

/* ---- where a peer was heard from ------------------------------------------- */

static int heard_find(const microlink_t *ml, const uint8_t *key) {
    for (int i = 0; i < ml->derp_heard_count; i++) {
        if (memcmp(ml->derp_heard[i].key, key, 32) == 0) return i;
    }
    return -1;
}

void ml_derp_pool_note_heard(microlink_t *ml, const uint8_t *key, uint16_t region) {
    int i = heard_find(ml, key);
    if (i < 0) {
        if (ml->derp_heard_count < ML_MAX_PEERS) {
            i = ml->derp_heard_count++;
        } else {
            /* the table holds one entry per peer slot; a stranger beyond
             * that takes the oldest slot (the table is a hint, not state) */
            i = ML_MAX_PEERS - 1;
        }
        memcpy(ml->derp_heard[i].key, key, 32);
    }
    ml->derp_heard[i].region = region;
}

void ml_derp_pool_forget_heard(microlink_t *ml, const uint8_t *key, uint16_t region) {
    int i = heard_find(ml, key);
    if (i < 0 || ml->derp_heard[i].region != region) return;
    ESP_LOGI(TAG, "relay region %u no longer knows peer %02x%02x%02x%02x",
             (unsigned)region, key[0], key[1], key[2], key[3]);
    ml->derp_heard[i] = ml->derp_heard[ml->derp_heard_count - 1];
    ml->derp_heard_count--;
}

static uint16_t heard_region(const microlink_t *ml, const uint8_t *key) {
    int i = heard_find(ml, key);
    return (i < 0) ? 0 : ml->derp_heard[i].region;
}

/* ---- picking a connection -------------------------------------------------- */

static ml_derp_conn_t *open_to(microlink_t *ml, uint16_t region) {
    if (region == 0) return NULL;
    if (ml->derp.connected && ml->derp.region == region) return &ml->derp;
    for (int i = 0; i < ML_DERP_EXTRA_CONNS; i++) {
        if (ml->derp_extra[i].connected && ml->derp_extra[i].region == region) {
            return &ml->derp_extra[i];
        }
    }
    return NULL;
}

static bool region_in_map(const microlink_t *ml, uint16_t region) {
    for (int i = 0; i < ml->derp_region_count; i++) {
        if (ml->derp_regions[i].region_id == region && !ml->derp_regions[i].avoid &&
            ml->derp_regions[i].node_count > 0) {
            return true;
        }
    }
    return false;
}

/* a slot for a dial to `region`: a free slot that last served it (its
 * backoff counts), else any free slot, else the least recently used busy
 * one if it has been quiet for POOL_BUSY_MS (closed here); NULL when every
 * slot is busy */
static ml_derp_conn_t *slot_for(microlink_t *ml, uint16_t region, uint64_t now) {
    ml_derp_conn_t *free_slot = NULL;
    ml_derp_conn_t *lru = NULL;

    for (int i = 0; i < ML_DERP_EXTRA_CONNS; i++) {
        ml_derp_conn_t *e = &ml->derp_extra[i];
        if (!e->connected) {
            if (e->region == region) return e;
            if (free_slot == NULL) free_slot = e;
        } else if (lru == NULL || e->last_use_ms < lru->last_use_ms) {
            lru = e;
        }
    }
    if (free_slot != NULL) return free_slot;
    if (lru != NULL && now - lru->last_use_ms >= POOL_BUSY_MS) {
        ESP_LOGI(TAG, "relay region %u closed to make room for region %u",
                 (unsigned)lru->region, (unsigned)region);
        ml_derp_conn_close(lru);
        return lru;
    }
    return NULL;
}

ml_derp_conn_t *ml_derp_pool_pick(microlink_t *ml, const ml_derp_tx_item_t *item) {
    uint16_t want = item->region;
    ml_derp_conn_t *c;

    /* 1. a connection to the peer's own region (the home one included) */
    if ((c = open_to(ml, want)) != NULL) return c;

    /* 2. a connection to a region the peer was heard from: it dialled our
     * home region to reach us, say (Tailscale's reverse route, issue 150) */
    if ((c = open_to(ml, heard_region(ml, item->dest_pubkey))) != NULL) return c;

    /* 3. dial the peer's region; unknown, or not in this map: the home relay */
    if (want == 0 || !region_in_map(ml, want)) return &ml->derp;

    uint64_t now = ml_get_time_ms();
    ml_derp_conn_t *slot = slot_for(ml, want, now);
    if (slot == NULL) return &ml->derp;
    if (slot->region == want && slot->retry_after_ms > now) return &ml->derp;

    if (ml_derp_conn_open(ml, slot, want, false) == ESP_OK) {
        slot->backoff_ms = 0;
        slot->retry_after_ms = 0;
        ESP_LOGI(TAG, "relay region %u opened for peer %02x%02x%02x%02x (home region %u)",
                 (unsigned)want, item->dest_pubkey[0], item->dest_pubkey[1],
                 item->dest_pubkey[2], item->dest_pubkey[3], (unsigned)ml->derp.region);
        return slot;
    }

    uint32_t next = slot->backoff_ms == 0 ? POOL_BACKOFF_MIN_MS : slot->backoff_ms * 2u;
    if (next > POOL_BACKOFF_MAX_MS) next = POOL_BACKOFF_MAX_MS;
    slot->backoff_ms = next;
    slot->retry_after_ms = now + next;
    slot->region = want;
    ESP_LOGW(TAG, "relay region %u unreachable, next attempt in %lu s; "
             "sending through the home relay meanwhile",
             (unsigned)want, (unsigned long)(next / 1000u));
    return &ml->derp;
}

/* ---- reading and ageing ---------------------------------------------------- */

/* a read would return at once: bytes buffered in the TLS layer, or the
 * socket readable (a zero-timeout select; the home connection's reads
 * carry the 200 ms socket timeout that paces the I/O loop, an extra
 * connection must not add its own) */
static bool conn_readable(ml_derp_conn_t *e) {
    if (e->use_tls && mbedtls_ssl_get_bytes_avail(&e->ssl) > 0) return true;
    fd_set r;
    FD_ZERO(&r);
    FD_SET(e->sockfd, &r);
    struct timeval tv = { .tv_sec = 0, .tv_usec = 0 };
    return ml_select_fds(e->sockfd + 1, &r, NULL, NULL, &tv) > 0;
}

void ml_derp_pool_service(microlink_t *ml, uint32_t *frames_rx) {
    uint64_t now = ml_get_time_ms();

    for (int i = 0; i < ML_DERP_EXTRA_CONNS; i++) {
        ml_derp_conn_t *e = &ml->derp_extra[i];
        if (!e->connected) continue;

        for (int burst = 0; burst < 4 && conn_readable(e); burst++) {
            int ret = ml_derp_conn_poll(ml, e);
            if (ret > 0) {
                (*frames_rx)++;
                e->last_use_ms = now;
            } else if (ret == 0) {
                break;
            } else {
                ESP_LOGW(TAG, "relay region %u: read error %d, connection closed",
                         (unsigned)e->region, ret);
                ml_derp_conn_close(e);
                break;
            }
        }

        if (e->connected && now - e->last_use_ms > ML_DERP_EXTRA_IDLE_MS) {
            ESP_LOGI(TAG, "relay region %u closed: idle for %u s",
                     (unsigned)e->region, (unsigned)(ML_DERP_EXTRA_IDLE_MS / 1000u));
            ml_derp_conn_close(e);
        }
    }
}

void ml_derp_pool_close_all(microlink_t *ml) {
    for (int i = 0; i < ML_DERP_EXTRA_CONNS; i++) {
        if (ml->derp_extra[i].connected || ml->derp_extra[i].sockfd >= 0) {
            ml_derp_conn_close(&ml->derp_extra[i]);
        }
    }
    ml->derp_heard_count = 0;
}
