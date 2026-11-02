/*
 * Copyright (c) 2022 Tomoyuki Sakurai <y@trombik.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *  list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this
 *  list of conditions and the following disclaimer in the documentation and/or
 *  other materials provided with the distribution.
 *
 * 3. Neither the name of "Floorsense Ltd", "Agile Workspace Ltd" nor the names of
 *  its contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <assert.h>
#include <string.h>
#include <inttypes.h>
#include <lwip/ip.h>
#include <lwip/netdb.h>
#include <lwip/err.h>
#include <lwip/tcpip.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_wireguard.h>
#include <mbedtls/base64.h>

#include "wireguard-platform.h"
#include "wireguardif.h"

#define TAG "esp_wireguard"
#define WG_KEY_LEN  (32)
#define WG_B64_KEY_LEN (4 * ((WG_KEY_LEN + 2) / 3))
#if defined(CONFIG_LWIP_IPV6)
#define WG_ADDRSTRLEN  INET6_ADDRSTRLEN
#else
#define WG_ADDRSTRLEN  INET_ADDRSTRLEN
#endif

static struct netif wg_netif_struct = {0};
static struct netif *wg_netif = NULL;
static struct wireguardif_peer peer = {0};
static uint8_t s_wg_peer_index = WIREGUARDIF_INVALID_INDEX;
static uint8_t preshared_key_decoded[WG_KEY_LEN];

/* WICAN FIX (2026-11-02, the two beta.5 crash reports in task vpn_state):
 * lwIP's core (netif, udp, the timeouts) runs on the tcpip thread only, and
 * this firmware has no core lock (CONFIG_LWIP_TCPIP_CORE_LOCKING off).
 * Upstream called netif_add, netif_set_up, netif_set_default,
 * netif_set_ipaddr and netif_remove from the caller's task; at the
 * disconnect, netif_set_ipaddr(ANY) made lwIP abort and free every TCP
 * connection on the tunnel address (MQTT, Home Assistant) from the vpn_state
 * task while the tcpip thread was using them: a double free in tlsf_free,
 * a freed callback pointer in tcp_abandon. Every lwIP core sequence of this
 * file now runs inside the tcpip thread through wg_in_tcpip(); only the
 * peer's name resolution (getaddrinfo, which needs the tcpip thread free)
 * stays on the caller. The callers are serialized by construction (one
 * tunnel, vpn_manager's state task), hence the one semaphore; a caller
 * must never be the tcpip thread itself (it would wait for itself). */
typedef esp_err_t (*wg_tcpip_fn)(void *arg);

struct wg_tcpip_job
{
    wg_tcpip_fn fn;
    void *arg;
    esp_err_t result;
    SemaphoreHandle_t done;
};

static void wg_tcpip_trampoline(void *ctx)
{
    struct wg_tcpip_job *job = (struct wg_tcpip_job *)ctx;

    job->result = job->fn(job->arg);
    xSemaphoreGive(job->done);
}

static esp_err_t wg_in_tcpip(wg_tcpip_fn fn, void *arg)
{
    static SemaphoreHandle_t s_done; /* internal: FreeRTOS object, 80 B */

    if (s_done == NULL)
    {
        s_done = xSemaphoreCreateBinary();

        if (s_done == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    struct wg_tcpip_job job =
    {
        .fn = fn, .arg = arg, .result = ESP_FAIL, .done = s_done,
    };

    if (tcpip_callback(wg_tcpip_trampoline, &job) != ERR_OK)
    {
        ESP_LOGE(TAG, "tcpip_callback: no message buffer");
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_done, portMAX_DELAY);
    return job.result;
}

static esp_err_t esp_wireguard_peer_init(const wireguard_config_t *config, struct wireguardif_peer *peer)
{
    esp_err_t err;
    char addr_str[WG_ADDRSTRLEN];
    struct addrinfo *res = NULL;
    struct addrinfo hints;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;

    if (!config || !peer) {
        err = ESP_ERR_INVALID_ARG;
        goto fail;
    }

    peer->public_key = config->public_key;
    if (config->preshared_key != NULL) {
        size_t len;
        int res;

        ESP_LOGI(TAG, "using preshared_key");
        ESP_LOGD(TAG, "preshared_key: %s", config->preshared_key);
#if defined(CONFIG_WIREGUARD_x25519_IMPLEMENTATION_DEFAULT)
        ESP_LOGI(TAG, "X25519: default");
#elif defined(CONFIG_WIREGUARD_x25519_IMPLEMENTATION_NACL)
        ESP_LOGI(TAG, "X25519: NaCL");
#endif
        res = mbedtls_base64_decode(preshared_key_decoded, WG_KEY_LEN, &len, (unsigned char *)config->preshared_key, WG_B64_KEY_LEN);
        if (res != 0 || len != WG_KEY_LEN) {
            err = ESP_FAIL;
            ESP_LOGE(TAG, "base64_decode: %i", res);
            if (len != WG_KEY_LEN) {
                ESP_LOGE(TAG, "invalid decoded length, len: %u, should be %u", len, WG_KEY_LEN);
            }
            goto fail;
        }
        peer->preshared_key = preshared_key_decoded;
    } else {
        peer->preshared_key = NULL;
    }
    peer->keep_alive = config->persistent_keepalive;

    /* Allow all IPs through tunnel */
    {
        ip_addr_t allowed_ip = IPADDR4_INIT_BYTES(0, 0, 0, 0);
        peer->allowed_ip = allowed_ip;
        ip_addr_t allowed_mask = IPADDR4_INIT_BYTES(0, 0, 0, 0);
        peer->allowed_mask = allowed_mask;
    }

    /* resolve peer name or IP address */
    {
        ip_addr_t endpoint_ip;
        memset(&endpoint_ip, 0, sizeof(endpoint_ip));

        /* XXX lwip_getaddrinfo returns only the first address of a host at the moment */
        if (getaddrinfo(config->endpoint, NULL, &hints, &res) != 0) {
            err = ESP_FAIL;

            /* XXX gai_strerror() is not implemented */
            ESP_LOGE(TAG, "getaddrinfo: unable to resolve `%s`", config->endpoint);
            goto fail;
        }

        if (res->ai_family == AF_INET) {
            struct in_addr addr4 = ((struct sockaddr_in *) (res->ai_addr))->sin_addr;
            inet_addr_to_ip4addr(ip_2_ip4(&endpoint_ip), &addr4);
        } else {
#if defined(CONFIG_LWIP_IPV6)
            struct in6_addr addr6 = ((struct sockaddr_in6 *) (res->ai_addr))->sin6_addr;
            inet6_addr_to_ip6addr(ip_2_ip6(&endpoint_ip), &addr6);
#endif
        }
        peer->endpoint_ip = endpoint_ip;

        if (inet_ntop(res->ai_family, &(peer->endpoint_ip), addr_str, WG_ADDRSTRLEN) == NULL) {
            ESP_LOGW(TAG, "inet_ntop: %i", errno);
        } else {
            ESP_LOGI(TAG, "Peer: %s (%s:%i)",
                                            config->endpoint,
                                            addr_str,
                                            config->port);
        }
    }
    peer->endport_port = config->port;
    peer->keep_alive = config->persistent_keepalive;
    err = ESP_OK;
fail:
    freeaddrinfo(res);
    return err;
}

/* in the tcpip thread: the netif into lwIP (netif_add runs wireguardif_init:
 * the UDP pcb, its receive callback and the periodic timeout) and up */
static esp_err_t netif_create_in_tcpip(void *arg)
{
    const wireguard_config_t *config = (const wireguard_config_t *)arg;
    esp_err_t err;
    ip_addr_t ip_addr;
    ip_addr_t netmask;
    ip_addr_t gateway = IPADDR4_INIT_BYTES(0, 0, 0, 0);
    struct wireguardif_init_data wg = {0};

    // Always start with a clean netif struct. If a previous session ended
    // via netif_remove(), the struct's `next` pointer is left stale; reusing
    // it without zeroing causes lwIP list corruption and routing failures.
    memset(&wg_netif_struct, 0, sizeof(wg_netif_struct));

    /* Setup the WireGuard device structure */
    wg.private_key = config->private_key;
    wg.listen_port = config->listen_port;
    wg.bind_netif = NULL;

    ESP_LOGI(TAG, "allowed_ip: %s", config->allowed_ip);

    if (ipaddr_aton(config->allowed_ip, &ip_addr) != 1) {
        ESP_LOGE(TAG, "ipaddr_aton: invalid allowed_ip: `%s`", config->allowed_ip);
        err = ESP_ERR_INVALID_ARG;
        goto fail;
    }
    if (ipaddr_aton(config->allowed_ip_mask, &netmask) != 1) {
        ESP_LOGE(TAG, "ipaddr_aton: invalid allowed_ip_mask: `%s`", config->allowed_ip_mask);
        err = ESP_ERR_INVALID_ARG;
        goto fail;
    }

    /* Register the new WireGuard network interface with lwIP */
    wg_netif = netif_add(
            &wg_netif_struct,
            ip_2_ip4(&ip_addr),
            ip_2_ip4(&netmask),
            ip_2_ip4(&gateway),
            &wg, &wireguardif_init,
            &ip_input);
    if (wg_netif == NULL) {
        ESP_LOGE(TAG, "netif_add: failed");
        err = ESP_FAIL;
        goto fail;
    }

    /* Mark the interface as administratively up, link up flag is set
     * automatically when peer connects */
    netif_set_up(wg_netif);
    err = ESP_OK;
fail:
    return err;
}

static esp_err_t esp_wireguard_netif_create(const wireguard_config_t *config)
{
    if (!config)
    {
        return ESP_ERR_INVALID_ARG;
    }

    return wg_in_tcpip(netif_create_in_tcpip, (void *)config);
}

/* in the tcpip thread: the peer into the device (its table is read by the
 * receive callback and the timer on that thread) */
static esp_err_t peer_add_in_tcpip(void *arg)
{
    (void)arg;
    err_t lwip_err = wireguardif_add_peer(wg_netif, &peer, &s_wg_peer_index);

    if (lwip_err != ERR_OK || s_wg_peer_index == WIREGUARDIF_INVALID_INDEX)
    {
        ESP_LOGE(TAG, "wireguardif_add_peer: %i", lwip_err);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* in the tcpip thread: the handshake starts (udp_sendto on the device's pcb) */
static esp_err_t connect_in_tcpip(void *arg)
{
    (void)arg;
    err_t lwip_err = wireguardif_connect(wg_netif, s_wg_peer_index);

    if (lwip_err != ERR_OK)
    {
        ESP_LOGE(TAG, "wireguardif_connect: %i", lwip_err);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* in the tcpip thread: the peer out, the timer and the pcb gone, the netif
 * out of lwIP (netif_remove aborts the TCP connections on its address when
 * it still has one: the field crash was this walk off the thread) */
static esp_err_t netif_destroy_in_tcpip(void *arg)
{
    struct netif *netif = (struct netif *)arg;

    if (s_wg_peer_index != WIREGUARDIF_INVALID_INDEX)
    {
        err_t lwip_err = wireguardif_disconnect(netif, s_wg_peer_index);

        if (lwip_err != ERR_OK)
        {
            ESP_LOGW(TAG, "wireguardif_disconnect: peer_index: %" PRIu8 " err: %i",
                     s_wg_peer_index, lwip_err);
        }

        lwip_err = wireguardif_remove_peer(netif, s_wg_peer_index);

        if (lwip_err != ERR_OK)
        {
            ESP_LOGW(TAG, "wireguardif_remove_peer: peer_index: %" PRIu8 " err: %i",
                     s_wg_peer_index, lwip_err);
        }

        s_wg_peer_index = WIREGUARDIF_INVALID_INDEX;
    }

    wireguardif_shutdown(netif);
    netif_remove(netif);
    wireguardif_fini(netif);
    return ESP_OK;
}

esp_err_t esp_wireguard_init(wireguard_config_t *config, wireguard_ctx_t *ctx)
{
    esp_err_t err = ESP_FAIL;

    if (!config || !ctx) {
        err = ESP_ERR_INVALID_ARG;
        goto fail;
    }

    ctx->config = config;
    ctx->netif = NULL;
    ctx->netif_default = netif_default;

    err = ESP_OK;
fail:
    return err;
}

esp_err_t esp_wireguard_connect(wireguard_ctx_t *ctx)
{
    esp_err_t err = ESP_FAIL;
    bool created_netif = false;

    if (!ctx) {
        err = ESP_ERR_INVALID_ARG;
        goto fail;
    }

    if (ctx->netif == NULL) {
        err = esp_wireguard_netif_create(ctx->config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wireguard_netif_create: %s", esp_err_to_name(err));
            goto fail;
        }
        created_netif = true;

        /* Initialize the first WireGuard peer structure */
        err = esp_wireguard_peer_init(ctx->config, &peer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wireguard_peer_init: %s", esp_err_to_name(err));
            goto fail;
        }

        /* Register the new WireGuard peer with the network interface */
        err = wg_in_tcpip(peer_add_in_tcpip, NULL);
        if (err != ESP_OK) {
            goto fail;
        }
        if (ip_addr_isany(&peer.endpoint_ip)) {
            err = ESP_FAIL;
            goto fail;
        }
        ctx->netif = wg_netif;
        ctx->netif_default = netif_default;
    }

    ESP_LOGI(TAG, "Connecting to %s:%i", ctx->config->endpoint, ctx->config->port);
    err = wg_in_tcpip(connect_in_tcpip, NULL);
fail:
    // If we created the netif in this call but failed before ctx->netif was set,
    // clean up the lwIP netif. Otherwise a later retry will hit:
    // assert failed: netif_add (... netif already added)
    if (err != ESP_OK && created_netif && ctx && ctx->netif == NULL && wg_netif != NULL)
    {
        // Best-effort cleanup (some steps may fail depending on how far we got)
        (void)wg_in_tcpip(netif_destroy_in_tcpip, wg_netif);
        wg_netif = NULL;
        memset(&wg_netif_struct, 0, sizeof(wg_netif_struct));
    }
    return err;
}

/* WICAN FIX (BUG_WG_DEFAULT_NETIF_CLOBBER.md): only a set_default that
 * actually ran may be undone at disconnect. Upstream restored the
 * default netif UNCONDITIONALLY from a value captured at init/connect
 * time: on a dual-uplink device (WiFi STA + USB-LTE) every failed
 * handshake retry then reverted the system default route to a STALE
 * netif, silently flipping all off-subnet traffic (found live: the WG
 * underlay hairpinned via WiFi while the LTE uplink sat idle). */
static bool s_default_route_taken = false;

/* in the tcpip thread: the tunnel becomes the default route */
static esp_err_t set_default_in_tcpip(void *arg)
{
    wireguard_ctx_t *ctx = (wireguard_ctx_t *)arg;

    /* capture at TAKEOVER time (not init) so the restore target is the
     * default that was actually in effect when we took the route */
    ctx->netif_default = netif_default;
    netif_set_default(ctx->netif);
    s_default_route_taken = true;
    return ESP_OK;
}

esp_err_t esp_wireguard_set_default(wireguard_ctx_t *ctx)
{
    if (!ctx)
    {
        return ESP_ERR_INVALID_ARG;
    }

    return wg_in_tcpip(set_default_in_tcpip, ctx);
}

/* in the tcpip thread: the address cleared (lwIP aborts every TCP
 * connection on it: the walk of the beta.5 crashes, on its own thread now),
 * the peer and the netif out, the default route given back */
static esp_err_t disconnect_in_tcpip(void *arg)
{
    wireguard_ctx_t *ctx = (wireguard_ctx_t *)arg;

    // Clear the IP address to gracefully disconnect any clients while the
    // peers are still valid
    netif_set_ipaddr(ctx->netif, IP4_ADDR_ANY4);

    (void)netif_destroy_in_tcpip(ctx->netif);

    /* WICAN FIX (BUG_WG_DEFAULT_NETIF_CLOBBER.md): restore ONLY what we
     * took. default_route=false sessions never touched the default,
     * restoring here clobbered another owner's route pin on every
     * reconnect cycle. */
    if (s_default_route_taken)
    {
        netif_set_default(ctx->netif_default);
        s_default_route_taken = false;
    }

    return ESP_OK;
}

esp_err_t esp_wireguard_disconnect(wireguard_ctx_t *ctx)
{
    esp_err_t err;

    if (!ctx) {
        err = ESP_ERR_INVALID_ARG;
        goto fail;
    }

    err = wg_in_tcpip(disconnect_in_tcpip, ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "disconnect: %s", esp_err_to_name(err));
        goto fail;
    }
    ctx->netif = NULL;

    // Zero the static netif struct so that the next netif_add() call starts
    // with a clean slate (no stale `next` pointer, stale `num`, etc.).
    memset(&wg_netif_struct, 0, sizeof(wg_netif_struct));
    wg_netif = NULL;
    // Reset the static peer configuration struct (used by esp_wireguard_peer_init)
    // so it doesn't carry stale keys/endpoint from a previous session.
    memset(&peer, 0, sizeof(peer));

    err = ESP_OK;
fail:
    return err;
}

esp_err_t esp_wireguardif_peer_is_up(wireguard_ctx_t *ctx)
{
    esp_err_t err;
    err_t lwip_err;

    if (!ctx) {
        err = ESP_ERR_INVALID_ARG;
        goto fail;
    }

    lwip_err = wireguardif_peer_is_up(
            ctx->netif,
            s_wg_peer_index,
            &peer.endpoint_ip,
            &peer.endport_port);

    if (lwip_err != ERR_OK) {
        err = ESP_FAIL;
        goto fail;
    }
    err = ESP_OK;
fail:
    return err;
}
