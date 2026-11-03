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
 * @file vpn_manager_ts.c
 * @brief Tailscale glue (vendored microlink): the `type = tailscale`
 *        path of the state task. Everything microlink allocates lives
 *        only between vpn_ts_up() and vpn_ts_down(): zero memory cost
 *        while tailscale is disabled or wireguard is selected.
 *        Called from the state task ONLY.
 */
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "flash_jobs.h"
#include "ip4_routes.h"
#include "microlink.h"

#include "vpn_manager_private.h"

static const char *TAG = "vpn_manager";

static microlink_t *s_ml;
/* microlink keeps POINTERS into its config: these back them */
static char s_auth_key[96];
static char s_device_name[48];
static char s_control_url[64];

esp_err_t vpn_ts_up(const vpn_config_t *cfg)
{
    if (s_ml != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    strlcpy(s_auth_key, cfg->ts_auth_key, sizeof(s_auth_key));
    strlcpy(s_device_name, cfg->ts_device_name, sizeof(s_device_name));
    strlcpy(s_control_url, cfg->ts_control_url, sizeof(s_control_url));

    microlink_config_t ml_cfg =
    {
        .auth_key = s_auth_key,
        .device_name = s_device_name[0] != '\0'
            ? s_device_name : microlink_default_device_name(),
        .enable_derp = true,
        .enable_stun = true,
        .enable_disco = true,
        .max_peers = 0, /* microlink default */
        .control_url = s_control_url[0] != '\0' ? s_control_url : NULL,
        /* the client's NVS work (the device keys, the peer cache) runs on
           the flash worker's internal stack: the state task and every
           microlink task live on PSRAM stacks (2026-11-01) */
        .flash_run = flash_jobs_run,
    };

    /* the internal heap around the two bring-up calls: init = the client's
       state, queues and the device keys from NVS; start = the sockets and
       the tasks (microlink logs its own per-step line inside) */
    size_t ram_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    s_ml = microlink_init(&ml_cfg);

    if (s_ml == NULL)
    {
        ESP_LOGE(TAG, "microlink init failed");
        return ESP_FAIL;
    }

    size_t ram_init = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    esp_err_t err = microlink_start(s_ml);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "microlink start: %s", esp_err_to_name(err));
        vpn_ts_down();
        return err;
    }

    size_t ram_start = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    ESP_LOGI(TAG, "tailscale starting (%s), internal RAM: init %d, start %d B "
             "(%u free before, %u after)",
             s_control_url[0] ? s_control_url : "tailscale.com",
             (int)ram_before - (int)ram_init, (int)ram_init - (int)ram_start,
             (unsigned)ram_before, (unsigned)ram_start);
    return ESP_OK;
}

void vpn_ts_down(void)
{
    if (s_ml == NULL)
    {
        return;
    }

    (void)microlink_stop(s_ml);
    microlink_destroy(s_ml); /* frees ALL microlink memory */
    s_ml = NULL;
    memset(s_auth_key, 0, sizeof(s_auth_key)); /* key material */
    ESP_LOGI(TAG, "tailscale down");
}

bool vpn_ts_connected(void)
{
    return s_ml != NULL && microlink_is_connected(s_ml);
}

bool vpn_ts_failed(void)
{
    return s_ml != NULL && microlink_get_state(s_ml) == ML_STATE_ERROR;
}

void vpn_ts_status(char *ip, size_t ip_len, int *peers, bool *derp,
                   uint16_t *region)
{
    ip[0] = '\0';
    *peers = 0;
    *derp = false;
    *region = 0;

    if (s_ml == NULL)
    {
        return;
    }

    *derp = microlink_derp_connected(s_ml);
    *region = microlink_derp_home_region(s_ml);

    uint32_t vpn_ip = microlink_get_vpn_ip(s_ml);

    if (vpn_ip != 0 && ip_len >= 16)
    {
        microlink_ip_to_str(vpn_ip, ip);
    }

    /* peer_count is a slot bound and may contain pruned holes (removal
     * only tail-compacts): count the slots that still answer. */
    int slots = microlink_get_peer_count(s_ml);
    int live  = 0;

    for (int i = 0; i < slots; i++)
    {
        microlink_peer_info_t info;

        if (microlink_get_peer_info(s_ml, i, &info) == ESP_OK)
        {
            live++;
        }
    }

    *peers = live;
}

int vpn_ts_get_peers(vpn_ts_peer_t *out, int max)
{
    if (s_ml == NULL || out == NULL || max <= 0)
    {
        return 0;
    }

    int count = microlink_get_peer_count(s_ml);

    if (count > max)
    {
        count = max;
    }

    int written = 0;

    for (int i = 0; i < count; i++)
    {
        microlink_peer_info_t info;

        if (microlink_get_peer_info(s_ml, i, &info) != ESP_OK)
        {
            continue;
        }

        vpn_ts_peer_t *p = &out[written++];

        strlcpy(p->hostname, info.hostname, sizeof(p->hostname));
        p->ip[0] = '\0';
        if (info.vpn_ip != 0)
        {
            microlink_ip_to_str(info.vpn_ip, p->ip);
        }
        p->online      = info.online;
        p->direct_path = info.direct_path;
    }

    return written;
}

int vpn_ts_get_routes(vpn_ts_route_t *out, int max)
{
    if (s_ml == NULL || out == NULL || max <= 0)
    {
        return 0;
    }

    microlink_route_t routes[VPN_TS_MAX_ROUTES];
    int n = microlink_get_routes(s_ml, routes,
                                 max < VPN_TS_MAX_ROUTES ? max : VPN_TS_MAX_ROUTES);

    for (int i = 0; i < n; i++)
    {
        ip4_routes_entry_t e = { .network = routes[i].network,
                                 .mask = routes[i].mask };

        ip4_routes_format(&e, out[i].cidr, sizeof(out[i].cidr));
        out[i].via[0] = '\0';
        if (routes[i].via_vpn_ip != 0)
        {
            microlink_ip_to_str(routes[i].via_vpn_ip, out[i].via);
        }
    }

    return n;
}
