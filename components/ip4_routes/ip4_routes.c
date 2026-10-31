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
 * @file ip4_routes.c
 * @brief The table, its lock, and the link-time wrap of IDF's IPv4 route
 *        hook (see include/ip4_routes.h for the model).
 */
#include "ip4_routes.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"

#include "log_manager.h"

#include "ip4_routes_private.h"

static const char *TAG = "ip4_routes";

/* internal: the routing hot path reads it for every outbound packet
   (16 x 12 bytes) */
/* PSRAM: read by the tcpip task, never from an ISR */
static ip4_routes_slot_t s_table[IP4_ROUTES_MAX] EXT_RAM_BSS_ATTR;
static size_t s_count;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

esp_err_t ip4_routes_init(void)
{
    static const log_descriptor_t LOG = { "ip4_routes", ESP_LOG_INFO };

    (void)log_manager_register(&LOG);
    return ESP_OK;
}

static void netif_name(const void *netif, char *out, size_t len)
{
    const struct netif *nif = netif;

    snprintf(out, len, "%c%c%u", nif->name[0], nif->name[1],
             (unsigned)nif->num);
}

esp_err_t ip4_routes_set(void *netif, const ip4_routes_entry_t *entries,
                         size_t n)
{
    if (netif == NULL || (n > 0 && entries == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_mux);

    int rc = ip4_routes_core_set(s_table, s_count, IP4_ROUTES_MAX, netif,
                                 entries, n);

    if (rc >= 0)
    {
        s_count = (size_t)rc;
    }

    portEXIT_CRITICAL(&s_mux);

    char name[8];

    netif_name(netif, name, sizeof(name));

    if (rc < 0)
    {
        /* a bounded registry overflows loudly (standard section 12) */
        ESP_LOGE(TAG, "route table full (%u of %u): %u route(s) for %s "
                 "refused", (unsigned)s_count, (unsigned)IP4_ROUTES_MAX,
                 (unsigned)n, name);
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < n; i++)
    {
        char cidr[24];

        ip4_routes_format(&entries[i], cidr, sizeof(cidr));
        ESP_LOGI(TAG, "%s via %s", cidr, name);
    }

    return ESP_OK;
}

void ip4_routes_clear(void *netif)
{
    if (netif == NULL)
    {
        return;
    }

    portENTER_CRITICAL(&s_mux);
    size_t before = s_count;

    s_count = ip4_routes_core_clear(s_table, s_count, netif);

    size_t dropped = before - s_count;

    portEXIT_CRITICAL(&s_mux);

    if (dropped > 0)
    {
        char name[8];

        netif_name(netif, name, sizeof(name));
        ESP_LOGI(TAG, "%u route(s) via %s dropped", (unsigned)dropped, name);
    }
}

void ip4_routes_capacity(size_t *used, size_t *cap)
{
    if (used != NULL)
    {
        *used = s_count;
    }

    if (cap != NULL)
    {
        *cap = IP4_ROUTES_MAX;
    }
}

/* ---- the hook ----------------------------------------------------------------
 * lwIP (ip4.c) calls LWIP_HOOK_IP4_ROUTE_SRC(src, dest) = ip4_route_src_hook
 * before its netif walk, from ip4_route() with src NULL and from
 * ip4_route_src() with the socket's bound address, and takes a non-NULL
 * answer as the netif. IDF's hook only matches the source address; the
 * link wrap puts the destination table in front of it. Runs in the tcpip
 * task for every outbound packet: no logging here. */
struct netif *__real_ip4_route_src_hook(const ip4_addr_t *src,
                                        const ip4_addr_t *dest);

/* A directly connected network wins over the table (2026-11-01): a
 * destination inside the own network of another netif that is up stays
 * on that netif, the way every IP stack routes (lwIP's own walk would have
 * chosen it) and the way Tailscale keeps a LAN local while a subnet
 * router advertises that very LAN. Without it a WiCAN at home, on the
 * network its home subnet router advertises, would send its own LAN's
 * traffic into the tunnel. */
static bool connected_elsewhere(const ip4_addr_t *dest, const struct netif *route_nif)
{
    struct netif *n;

    NETIF_FOREACH(n)
    {
        if (n == route_nif || !netif_is_up(n) || !netif_is_link_up(n) ||
            ip4_addr_isany_val(*netif_ip4_addr(n)))
        {
            continue;
        }

        if (ip4_addr_net_eq(dest, netif_ip4_addr(n), netif_ip4_netmask(n)))
        {
            return true;
        }
    }

    return false;
}

struct netif *__wrap_ip4_route_src_hook(const ip4_addr_t *src,
                                        const ip4_addr_t *dest)
{
    if (dest != NULL && s_count > 0)
    {
        uint32_t d = lwip_ntohl(ip4_addr_get_u32(dest));

        portENTER_CRITICAL(&s_mux);

        struct netif *nif = ip4_routes_core_match(s_table, s_count, d);

        portEXIT_CRITICAL(&s_mux);

        if (nif != NULL && netif_is_up(nif) && netif_is_link_up(nif) &&
            !connected_elsewhere(dest, nif))
        {
            return nif;
        }
    }

    return __real_ip4_route_src_hook(src, dest);
}
