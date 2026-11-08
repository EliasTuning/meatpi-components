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
 * @file wifi_manager_status.c
 * @brief Status queries, DNS helpers, callback registration and the
 *        scan-to-JSON API.
 */
#include "wifi_manager.h"

#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "cJSON.h"

#include "wifi_manager_bridge.h"
#include "wifi_manager_private.h"

bool wifi_manager_is_enabled(void)
{
    return wm_status_flag(0);
}

bool wifi_manager_is_sta_connected(void)
{
    return wm_status_flag(1);
}

bool wifi_manager_is_ap_started(void)
{
    return wm_status_flag(2);
}

esp_err_t wifi_manager_sta_reconnect(void)
{
    if (!wifi_manager_is_sta_connected())
    {
        return ESP_ERR_INVALID_STATE;
    }

    /* a self-initiated disconnect (reason ASSOC_LEAVE) is not auth
     * related: no ban, the reconnect task re-selects on its next lap */
    ESP_LOGI("wifi_manager", "STA re-join requested by a consumer");
    return esp_wifi_disconnect();
}

esp_err_t wifi_manager_get_sta_ip(char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    return wm_status_copy_ip(buf, buf_len);
}

bool wifi_manager_sta_on_network(const char *list)
{
    if (list == NULL || list[0] == '\0')
    {
        return true; /* no restriction */
    }

    char ssid[WM_SSID_LEN];

    if (wifi_manager_get_sta_ssid(ssid, sizeof(ssid)) != ESP_OK ||
        ssid[0] == '\0')
    {
        return false; /* no station session: on no WiFi network at all */
    }

    return wm_ssid_list_has(list, ssid);
}

uint16_t wifi_manager_get_ap_station_count(void)
{
    return wm_status_ap_clients();
}

esp_err_t wifi_manager_get_ap_ip(char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_netif_t *netif = wm_ap_netif();
    esp_netif_ip_info_t ip;

    if (netif == NULL || esp_netif_get_ip_info(netif, &ip) != ESP_OK ||
        ip.ip.addr == 0)
    {
        return ESP_ERR_INVALID_STATE;
    }

    snprintf(buf, buf_len, IPSTR, IP2STR(&ip.ip));
    return ESP_OK;
}

EventGroupHandle_t wifi_manager_get_event_group(void)
{
    return wm_event_group();
}

esp_err_t wifi_manager_set_callbacks(const wifi_manager_callbacks_t *cbs)
{
    wm_set_callbacks(cbs);
    return ESP_OK;
}

static void dns_to_str(esp_netif_t *netif, esp_netif_dns_type_t type,
                       char *out, size_t out_len)
{
    esp_netif_dns_info_t dns = { 0 };

    if (netif != NULL &&
        esp_netif_get_dns_info(netif, type, &dns) == ESP_OK &&
        dns.ip.type == ESP_IPADDR_TYPE_V4 && dns.ip.u_addr.ip4.addr != 0)
    {
        snprintf(out, out_len, IPSTR, IP2STR(&dns.ip.u_addr.ip4));
    }
    else
    {
        strlcpy(out, "N/A", out_len);
    }
}

esp_err_t wifi_manager_get_sta_dns(char *dns_main, size_t dns_main_len,
                                   char *dns_backup, size_t dns_backup_len)
{
    esp_netif_t *netif = wm_sta_netif();

    if (dns_main != NULL && dns_main_len > 0)
    {
        dns_to_str(netif, ESP_NETIF_DNS_MAIN, dns_main, dns_main_len);
    }

    if (dns_backup != NULL && dns_backup_len > 0)
    {
        dns_to_str(netif, ESP_NETIF_DNS_BACKUP, dns_backup, dns_backup_len);
    }

    return (netif != NULL) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static const char *auth_mode_str(wifi_auth_mode_t mode)
{
    switch (mode)
    {
        case WIFI_AUTH_OPEN:          return "OPEN";
        case WIFI_AUTH_WEP:           return "WEP";
        case WIFI_AUTH_WPA_PSK:       return "WPA_PSK";
        case WIFI_AUTH_WPA2_PSK:      return "WPA2_PSK";
        case WIFI_AUTH_WPA_WPA2_PSK:  return "WPA_WPA2_PSK";
        case WIFI_AUTH_WPA3_PSK:      return "WPA3_PSK";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2_WPA3_PSK";
        default:                      return "UNKNOWN";
    }
}

char *wifi_manager_scan_networks(void)
{
    wifi_ap_record_t *records = NULL;
    uint16_t          count = 0;

    if (wm_scan_locked(&records, &count) != ESP_OK)
    {
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *networks = cJSON_AddArrayToObject(root, "networks");

    for (uint16_t i = 0; i < count; i++)
    {
        cJSON *ap = cJSON_CreateObject();
        char   bssid[18];

        cJSON_AddStringToObject(ap, "ssid", (const char *)records[i].ssid);
        cJSON_AddNumberToObject(ap, "rssi", records[i].rssi);
        cJSON_AddNumberToObject(ap, "channel", records[i].primary);
        cJSON_AddStringToObject(ap, "auth_mode",
                                auth_mode_str(records[i].authmode));
        snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
                 records[i].bssid[0], records[i].bssid[1], records[i].bssid[2],
                 records[i].bssid[3], records[i].bssid[4], records[i].bssid[5]);
        cJSON_AddStringToObject(ap, "bssid", bssid);

        cJSON_AddItemToArray(networks, ap);
    }

    wm_scan_unlock();

    char *json = cJSON_PrintUnformatted(root);

    cJSON_Delete(root);
    return json;
}
