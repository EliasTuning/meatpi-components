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
 * @file vpn_manager_http.c
 * @brief The optional /api/vpn routes (§9.1): status GET + device-side
 *        keygen POST (returns ONLY the public key; the private key
 *        lands in pending settings). Config itself is the plain
 *        /api/settings/vpn_manager surface.
 */
#include "esp_attr.h"
#include <stdio.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "cJSON.h"
#include "http_server_manager.h"
#include "ip4_routes.h"

#include "vpn_manager.h"
#include "vpn_manager_private.h"

static const char *TAG = "vpn_manager";

#define VPN_HTTP_MAX_PEERS 16 /* microlink's default peer-table size */

static esp_err_t vpn_get_handler(httpd_req_t *req)
{
    vpn_manager_status_t st;

    (void)vpn_manager_status(&st);

    cJSON *obj = cJSON_CreateObject();

    if (obj == NULL)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    cJSON_AddStringToObject(obj, "state", vpn_manager_state_str(st.state));
    cJSON_AddStringToObject(obj, "type",
                            st.tailscale ? "tailscale" : "wireguard");
    cJSON_AddStringToObject(obj, "endpoint", st.endpoint);
    /* issue #976: the station's network and whether it pauses the VPN */
    cJSON_AddStringToObject(obj, "network", st.network);
    cJSON_AddBoolToObject(obj, "paused", st.paused);

    if (!st.tailscale)
    {
        /* the split tunnel (issue #975): the networks routed into it */
        ip4_routes_entry_t routes[VPN_MAX_ROUTES];
        int n = ip4_routes_parse_list(vpn_settings_config()->routes, routes,
                                      VPN_MAX_ROUTES, NULL, 0);
        cJSON *arr = cJSON_AddArrayToObject(obj, "routes");

        for (int i = 0; arr != NULL && i < n; i++)
        {
            char cidr[24];

            ip4_routes_format(&routes[i], cidr, sizeof(cidr));
            cJSON_AddItemToArray(arr, cJSON_CreateString(cidr));
        }
    }
    else
    {
        /* the subnet routes learned from the tailnet (2026-11-01): the
           networks the subnet routers advertise, routed into the tunnel */
        static vpn_ts_route_t routes[VPN_TS_MAX_ROUTES] EXT_RAM_BSS_ATTR; /* httpd task only; PSRAM */
        int n = vpn_ts_get_routes(routes, VPN_TS_MAX_ROUTES);
        cJSON *arr = cJSON_AddArrayToObject(obj, "routes");

        for (int i = 0; arr != NULL && i < n; i++)
        {
            cJSON_AddItemToArray(arr, cJSON_CreateString(routes[i].cidr));
        }
    }
    cJSON_AddStringToObject(obj, "ts_ip", st.ts_ip);
    cJSON_AddNumberToObject(obj, "ts_peers", st.ts_peers);
    /* the DERP relay (2026-10-31, issue 1024): false until the relay
     * handshake is through, and while the relay is in backoff */
    cJSON_AddBoolToObject(obj, "derp", st.ts_derp);
    /* the internal-RAM floor (issue 1024): "" unless the tunnel is held */
    cJSON_AddStringToObject(obj, "reason", st.reason);
    cJSON_AddNumberToObject(obj, "connects", st.connects);
    cJSON_AddNumberToObject(obj, "failures", st.failures);
    cJSON_AddNumberToObject(obj, "uptime_s", st.uptime_s);

    if (st.tailscale)
    {
        static vpn_ts_peer_t peers[VPN_HTTP_MAX_PEERS] EXT_RAM_BSS_ATTR; /* httpd task only; PSRAM (1.3 KB) */
        int n = vpn_ts_get_peers(peers, VPN_HTTP_MAX_PEERS);
        cJSON *arr = cJSON_AddArrayToObject(obj, "peers");

        for (int i = 0; arr != NULL && i < n; i++)
        {
            cJSON *p = cJSON_CreateObject();

            if (p == NULL)
            {
                break;
            }
            cJSON_AddStringToObject(p, "hostname", peers[i].hostname);
            cJSON_AddStringToObject(p, "ip", peers[i].ip);
            cJSON_AddBoolToObject(p, "online", peers[i].online);
            cJSON_AddStringToObject(p, "path",
                                    peers[i].direct_path ? "direct"
                                                         : "relay");
            cJSON_AddItemToArray(arr, p);
        }
    }

    char *body = cJSON_PrintUnformatted(obj);

    cJSON_Delete(obj);
    if (body == NULL)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);

    cJSON_free(body);
    return err;
}

static esp_err_t vpn_keygen_handler(httpd_req_t *req)
{
    char pub[64];
    char body[128];

    if (vpn_manager_keygen(pub, sizeof(pub)) != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "keygen failed");
        return ESP_FAIL;
    }

    snprintf(body, sizeof(body),
             "{\"public_key\":\"%s\",\"pending_reboot\":true}", pub);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

esp_err_t vpn_manager_register_http(void)
{
    static const httpd_uri_t URIS[] =
    {
        { .uri = "/api/vpn", .method = HTTP_GET,
          .handler = vpn_get_handler },
        { .uri = "/api/vpn/keygen", .method = HTTP_POST,
          .handler = vpn_keygen_handler },
    };

    esp_err_t err = http_server_manager_register_handlers(
        URIS, sizeof(URIS) / sizeof(URIS[0]));

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "/api/vpn registered");
    }

    return err;
}
