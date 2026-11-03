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
 * @file vpn_manager.c
 * @brief Lifecycle, the state task, and the DNS override (see
 *        include/vpn_manager.h for the model). Settings live in
 *        vpn_manager_settings.c (standard §4.1).
 *
 * State task ladder: WAITING (network bit + valid clock) → CONNECTING
 * (vpn_wg_up + peer poll, 20 s budget) → CONNECTED (VPN bit, event,
 * optional DNS override/default route) → loss of network or peer
 * silence tears down and re-enters WAITING with backoff.
 */
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "cJSON.h"
#include "dev_status_manager.h"
#include "log_manager.h"
#include "rtc_manager.h"
#include "settings_manager.h"
#include "wifi_manager.h"

#include "vpn_manager_private.h"

static const char *TAG = "vpn_manager";

#define VPN_POLL_MS        1000
#define VPN_HANDSHAKE_S    20    /* peer-up budget per attempt        */
#define VPN_PEER_LOST_S    30    /* silent peer => reconnect          */
#define VPN_BACKOFF_MIN_S  5
#define VPN_BACKOFF_MAX_S  60

static volatile bool s_run;
static TaskHandle_t s_task;
/* The state task's stack lives in PSRAM since 2026-11-01 (it was 6 KB of
 * internal RAM: the Tailscale path reads and writes the device keys in NVS
 * from this task, which the standard's section 2 corollary forbids on a
 * PSRAM stack; microlink runs that work through its flash hook now, the
 * flash_jobs worker, see vpn_manager_ts.c). The stack is taken from the
 * PSRAM heap at the first start and kept (the task lives for the firmware's
 * life, s_run parks it); the TCB is a static (internal: FreeRTOS). A PSRAM
 * stack overflows silently: the watermark is on /api/status/tasks and
 * `.\test.ps1 stackaudit` reads it. */
#define VPN_TASK_STACK 6144 /* watermark-checked on the bench 2026-07-07; was 7168, see TASK_tailscale.md internal-RAM note */
static StaticTask_t  s_task_tcb;   /* internal: FreeRTOS */
static StackType_t  *s_task_stack; /* PSRAM, VPN_TASK_STACK bytes */

static vpn_manager_status_t s_status;
static uint32_t s_up_since_ms;
/* PSRAM: why Tailscale is held in waiting (issue 1024); "" while not held.
 * Written by the state task, read by the status surfaces. */
static char s_hold_reason[176] EXT_RAM_BSS_ATTR;

/* DNS override bookkeeping */
static esp_netif_dns_info_t s_saved_dns;
static bool s_dns_overridden;

/* ---- DNS override ----------------------------------------------------------- */

static esp_netif_t *sta_netif(void)
{
    return esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
}

static void dns_override_apply(void)
{
    const vpn_config_t *cfg = vpn_settings_config();
    esp_netif_t *netif = sta_netif();
    esp_netif_dns_info_t dns = { 0 };

    if (cfg->dns[0] == '\0' || netif == NULL)
    {
        return;
    }

    if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN,
                               &s_saved_dns) != ESP_OK)
    {
        return;
    }

    dns.ip.type = ESP_IPADDR_TYPE_V4;

    if (inet_pton(AF_INET, cfg->dns, &dns.ip.u_addr.ip4) != 1)
    {
        ESP_LOGW(TAG, "dns '%s' is not an IPv4 address", cfg->dns);
        return;
    }

    if (esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK)
    {
        s_dns_overridden = true;
        ESP_LOGI(TAG, "DNS override -> %s", cfg->dns);
    }
}

static void dns_override_restore(void)
{
    esp_netif_t *netif = sta_netif();

    if (!s_dns_overridden || netif == NULL)
    {
        return;
    }

    (void)esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &s_saved_dns);
    s_dns_overridden = false;
    ESP_LOGI(TAG, "DNS restored");
}

/* ---- state task -------------------------------------------------------------- */

static bool network_up(void)
{
    return (dev_status_manager_get() &
            DEV_STATUS_NETWORK_CONNECTED_MASK) != 0;
}

/* the station's network, for the status, the log lines and the event */
static void sta_network(char *buf, size_t len)
{
    if (wifi_manager_get_sta_ssid(buf, len) != ESP_OK)
    {
        buf[0] = '\0';
    }
}

/* "Pause VPN" (issue #976, 2026-10-11): the station is on a network the
 * user marked under Settings > WiFi; the tunnel stays down meanwhile */
static bool paused_here(void)
{
    return wifi_manager_sta_vpn_pause();
}

static void enter_paused(void)
{
    char net[33];

    sta_network(net, sizeof(net));
    s_status.state = VPN_STATE_PAUSED;
    ESP_LOGI(TAG, "paused on %s (marked Pause VPN): the tunnel stays down "
             "until the station leaves it", net);
    vpn_events_state(false, true, net);
}

static void enter_connected(void)
{
    const vpn_config_t *cfg = vpn_settings_config();
    char net[33];

    s_status.state = VPN_STATE_CONNECTED;
    s_status.connects++;
    s_up_since_ms = (uint32_t)(esp_log_timestamp());
    dev_status_manager_set(DEV_STATUS_BIT_VPN_ENABLED);

    if (!cfg->tailscale)
    {
        /* wireguard-only concerns: microlink owns its own netif,
         * routes and (Magic)DNS */
        dns_override_apply();

        if (cfg->default_route)
        {
            (void)vpn_wg_set_default_route();
        }
    }

    sta_network(net, sizeof(net));
    vpn_events_state(true, false, net);
    /* the internal RAM left at this moment: the tunnel's own cost is read
     * against the boot's floor (2026-10-31: Tailscale takes about 26 KB) */
    ESP_LOGI(TAG, "connected (%s), internal RAM free %u B",
             vpn_settings_endpoint(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

/* @p paused: the teardown is a pause (the station joined a marked
 * network), told to the event's readers in the same publish */
static void teardown(bool was_connected, bool paused)
{
    if (vpn_settings_config()->tailscale)
    {
        vpn_ts_down();
    }
    else
    {
        dns_override_restore();
        vpn_wg_down();
    }

    dev_status_manager_clear(DEV_STATUS_BIT_VPN_ENABLED);

    if (was_connected)
    {
        char net[33];

        sta_network(net, sizeof(net));
        vpn_events_state(false, paused, net);
    }
}

/* type-dispatched primitives for the state ladder */
static esp_err_t tunnel_up(void)
{
    const vpn_config_t *cfg = vpn_settings_config();

    return cfg->tailscale ? vpn_ts_up(cfg) : vpn_wg_up(cfg);
}

static bool tunnel_alive(void)
{
    return vpn_settings_config()->tailscale ? vpn_ts_connected()
                                            : vpn_wg_peer_up();
}

/* A bool / an enum word of another component's stored settings, read
 * through the Settings Manager (no dependency on the component): only on
 * the first lap of a hold, never on the hot path. */
static bool setting_bool(const char *component, const char *key)
{
    cJSON *obj = NULL;
    bool value = false;

    if (settings_manager_get(component, &obj) == ESP_OK && obj != NULL)
    {
        value = cJSON_IsTrue(cJSON_GetObjectItem(obj, key));
    }

    cJSON_Delete(obj);
    return value;
}

static bool setting_is(const char *component, const char *key, const char *word)
{
    cJSON *obj = NULL;
    bool value = false;

    if (settings_manager_get(component, &obj) == ESP_OK && obj != NULL)
    {
        const cJSON *item = cJSON_GetObjectItem(obj, key);

        value = cJSON_IsString(item) && strcmp(item->valuestring, word) == 0;
    }

    cJSON_Delete(obj);
    return value;
}

/* The internal-RAM floor (issue 1024, 2026-10-31): Tailscale starts only
 * with VPN_TS_INTERNAL_FLOOR_KB free at that moment; otherwise the tunnel
 * holds in WAITING with a reason that names what the device can change,
 * one W line per hold, and starts by itself when RAM frees up. With
 * Bluetooth on (about 40 KB) a bench unit's connect left 1.2 KB: sockets,
 * the SD card's DMA buffers and the web server failed, and before the
 * guard a task's first name lookup crashed it. */
static bool ts_ram_ok(void)
{
    size_t free_b = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    if (free_b >= VPN_TS_INTERNAL_FLOOR_KB * 1024u)
    {
        if (s_hold_reason[0] != '\0')
        {
            ESP_LOGI(TAG, "tailscale: %u KB of internal RAM free, starting",
                     (unsigned)(free_b / 1024u));
            s_hold_reason[0] = '\0';
        }

        return true;
    }

    if (s_hold_reason[0] == '\0')
    {
        vpn_ts_hold_reason((unsigned)(free_b / 1024u), VPN_TS_INTERNAL_FLOOR_KB,
                           setting_bool("ble_manager", "enabled"),
                           !setting_is("wifi_manager", "wifi_ram_profile", "lean"),
                           s_hold_reason, sizeof(s_hold_reason));
        ESP_LOGW(TAG, "tailscale %s", s_hold_reason);
    }

    return false;
}

static void state_task(void *arg)
{
    const vpn_config_t *cfg = vpn_settings_config();
    uint32_t backoff_s = VPN_BACKOFF_MIN_S;

    (void)arg;

    while (true)
    {
        if (!s_run)
        {
            if (s_status.state >= VPN_STATE_CONNECTING)
            {
                teardown(s_status.state == VPN_STATE_CONNECTED, false);
            }

            s_status.state = VPN_STATE_DISABLED;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        /* ---- PAUSED: the station is on a network marked "Pause VPN"
         * (issue #976): park until it leaves; the backoff starts from
         * its minimum when it does */
        if (paused_here())
        {
            if (s_status.state != VPN_STATE_PAUSED)
            {
                enter_paused();
            }

            backoff_s = VPN_BACKOFF_MIN_S;
            vTaskDelay(pdMS_TO_TICKS(VPN_POLL_MS));
            continue;
        }

        if (s_status.state == VPN_STATE_PAUSED)
        {
            ESP_LOGI(TAG, "left the paused network: resuming");
        }

        /* ---- WAITING: uplink + a sane clock (WG handshake needs it) */
        s_status.state = VPN_STATE_WAITING;

        if (!network_up())
        {
            vTaskDelay(pdMS_TO_TICKS(VPN_POLL_MS));
            continue;
        }

        if (!rtc_manager_time_valid())
        {
            static bool s_warned;

            if (!s_warned)
            {
                ESP_LOGW(TAG, "waiting for a valid clock (WireGuard "
                         "handshakes need one)");
                s_warned = true;
            }

            vTaskDelay(pdMS_TO_TICKS(VPN_POLL_MS));
            continue;
        }

        /* ---- the internal-RAM floor (issue 1024): hold here, say why */
        if (cfg->tailscale && !ts_ram_ok())
        {
            vTaskDelay(pdMS_TO_TICKS(VPN_POLL_MS));
            continue;
        }

        /* ---- CONNECTING */
        s_status.state = VPN_STATE_CONNECTING;

        if (tunnel_up() != ESP_OK)
        {
            s_status.failures++;
            teardown(false, false);
            vTaskDelay(pdMS_TO_TICKS(backoff_s * 1000));
            backoff_s = (backoff_s * 2 > VPN_BACKOFF_MAX_S)
                            ? VPN_BACKOFF_MAX_S : backoff_s * 2;
            continue;
        }

        bool up = false;
        bool paused_meanwhile = false;
        /* tailscale needs the full control-plane dance (register +
         * map + DERP/DISCO): give it a longer budget than a plain
         * WG handshake */
        int budget_s = cfg->tailscale ? 90 : VPN_HANDSHAKE_S;

        for (int i = 0; i < budget_s && s_run; i++)
        {
            if (!network_up())
            {
                break;
            }

            if (paused_here())
            {
                /* the station landed on a marked network mid-attempt:
                 * not a failure, the top of the loop parks */
                paused_meanwhile = true;
                break;
            }

            if (cfg->tailscale && vpn_ts_failed())
            {
                break;
            }

            if (tunnel_alive())
            {
                up = true;
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        if (paused_meanwhile)
        {
            teardown(false, true);
            continue;
        }

        if (!up)
        {
            ESP_LOGW(TAG, "no handshake within %d s", budget_s);
            s_status.failures++;
            teardown(false, false);
            vTaskDelay(pdMS_TO_TICKS(backoff_s * 1000));
            backoff_s = (backoff_s * 2 > VPN_BACKOFF_MAX_S)
                            ? VPN_BACKOFF_MAX_S : backoff_s * 2;
            continue;
        }

        /* ---- CONNECTED */
        backoff_s = VPN_BACKOFF_MIN_S;
        enter_connected();

        uint32_t silent_s = 0;

        while (s_run && network_up() && !paused_here())
        {
            vTaskDelay(pdMS_TO_TICKS(VPN_POLL_MS));

            if (tunnel_alive())
            {
                silent_s = 0;
                continue;
            }

            if (++silent_s >= VPN_PEER_LOST_S)
            {
                ESP_LOGW(TAG, "peer silent for %lu s; reconnecting",
                         (unsigned long)silent_s);
                break;
            }
        }

        bool pause_now = paused_here();

        teardown(true, pause_now);

        if (pause_now)
        {
            /* the station joined a marked network: one publish (the
             * teardown's) and one line, then the top of the loop parks */
            char net[33];

            sta_network(net, sizeof(net));
            s_status.state = VPN_STATE_PAUSED;
            ESP_LOGI(TAG, "paused on %s (marked Pause VPN): tunnel down "
                     "until the station leaves it", net);
        }
        else
        {
            ESP_LOGI(TAG, "left connected state (network %s)",
                     network_up() ? "up" : "down");
        }
    }
}

/* ---- lifecycle -------------------------------------------------------------- */

esp_err_t vpn_manager_init(void)
{
    static const log_descriptor_t LOG_DESC =
        { "vpn_manager", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);
    vpn_events_register();
    return vpn_settings_register();
}

esp_err_t vpn_manager_start(void)
{
    const vpn_config_t *cfg = vpn_settings_config();

    if (!vpn_settings_is_configured())
    {
        ESP_LOGE(TAG, "unconfigured; not starting");
        return ESP_ERR_INVALID_STATE; /* standard §4.3 step 5 */
    }

    if (!cfg->enabled)
    {
        ESP_LOGI(TAG, "disabled in settings");
        return ESP_OK;
    }

    s_run = true;

    if (s_task == NULL)
    {
        if (s_task_stack == NULL)
        {
            s_task_stack = heap_caps_malloc(VPN_TASK_STACK,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }

        if (s_task_stack != NULL)
        {
            s_task = xTaskCreateStatic(state_task, "vpn_state", VPN_TASK_STACK,
                                       NULL, 3, s_task_stack, &s_task_tcb);
        }

        if (s_task == NULL)
        {
            ESP_LOGE(TAG, "state task not created (no PSRAM for its stack)");
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "started (%s, default_route %s)",
             vpn_settings_endpoint(),
             cfg->default_route ? "on" : "off");
    return ESP_OK;
}

esp_err_t vpn_manager_stop(void)
{
    s_run = false; /* the state task tears down on its next lap */
    return ESP_OK;
}

esp_err_t vpn_manager_status(vpn_manager_status_t *out)
{
    const vpn_config_t *cfg = vpn_settings_config();

    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out = s_status;
    /* boot-applied fields live with the settings (vpn_manager_settings.c) */
    strlcpy(out->endpoint, vpn_settings_endpoint(),
            sizeof(out->endpoint));
    out->tailscale = cfg->tailscale;
    out->paused = (s_status.state == VPN_STATE_PAUSED);
    strlcpy(out->reason, s_hold_reason, sizeof(out->reason));
    sta_network(out->network, sizeof(out->network));
    out->uptime_s = (s_status.state == VPN_STATE_CONNECTED)
        ? (esp_log_timestamp() - s_up_since_ms) / 1000 : 0;

    if (cfg->tailscale)
    {
        vpn_ts_status(out->ts_ip, sizeof(out->ts_ip), &out->ts_peers,
                      &out->ts_derp, &out->ts_derp_region);
    }

    return ESP_OK;
}

esp_err_t vpn_manager_tunnel_ip(char *buf, size_t len)
{
    if (buf == NULL || len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    buf[0] = '\0';

    if (s_status.state != VPN_STATE_CONNECTED)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const vpn_config_t *cfg = vpn_settings_config();

    if (cfg->tailscale)
    {
        int peers = 0;
        bool derp = false;
        uint16_t region = 0;

        vpn_ts_status(buf, len, &peers, &derp, &region);
    }
    else
    {
        strlcpy(buf, cfg->address, len);

        /* the setting allows an optional /cidr suffix: the tunnel IP
           consumers (HA vpn_ip) want the bare address */
        char *slash = strchr(buf, '/');

        if (slash != NULL)
        {
            *slash = '\0';
        }
    }

    return buf[0] != '\0' ? ESP_OK : ESP_ERR_INVALID_STATE;
}
