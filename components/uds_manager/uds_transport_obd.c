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
 * @file uds_transport_obd.c
 * @brief backend "obd_chip" — UDS over the MIC3624 via AT-hex.
 */
#include "uds_transport.h"

#include "obd_chip.h"

/* Re-send the 8-command target setup only when needed: the address or
 * p2 changed, the previous transaction failed, or anyone else wrote to
 * the chip in between (autopid's polls, an app) — detected with the
 * driver's tx_bytes counter, which only we moved otherwise (read while
 * we still hold the chip). One request = one AT line in a steady UDS
 * conversation (bench 2026-09-16: 8 round-trips -> 1).
 */
static uds_addr_t s_setup_addr;
static uint32_t s_setup_p2_ms;
static uint32_t s_setup_tx_bytes; /* the chip's tx_bytes after our setup */
static bool s_setup_valid;

static bool setup_still_valid(const uds_addr_t *addr, uint32_t p2_ms)
{
    obd_chip_stats_t st;

    if (!s_setup_valid || obd_chip_get_stats(&st) != ESP_OK)
    {
        return false;
    }

    return s_setup_addr.tx_id == addr->tx_id &&
           s_setup_addr.rx_id == addr->rx_id &&
           s_setup_addr.ext_id == addr->ext_id &&
           s_setup_p2_ms == p2_ms && st.tx_bytes == s_setup_tx_bytes;
}

static esp_err_t obd_req(const char *cmd, char *resp, size_t resp_len,
                         uint32_t timeout_ms)
{
    return obd_chip_request(cmd, resp, resp_len,
                            pdMS_TO_TICKS(timeout_ms));
}

static esp_err_t obd_open(void)
{
    return ESP_OK; /* the MIC is always present + claim-arbitrated */
}

static esp_err_t obd_transceive(const uds_addr_t *addr,
                                const uint8_t *req, size_t req_len,
                                uint8_t *resp, size_t resp_cap,
                                size_t *resp_len, uint32_t p2_ms,
                                uint32_t p2star_ms, uint8_t *pending_out)
{
    /* hold the chip for the whole AT transaction (7 setup commands + the
       request): autopid's polls wait at their claim instead of landing
       between our commands (bench 2026-09-16: with polling, 1 of 3
       requests survived; paused, 5 of 5) */
    esp_err_t err = obd_chip_txn_begin(pdMS_TO_TICKS(2000));

    if (err != ESP_OK)
    {
        return err; /* chip busy (monitor/update) or another transaction */
    }

    bool skip = setup_still_valid(addr, p2_ms);

    err = uds_at_transceive_ex(obd_req, addr, req, req_len, resp, resp_cap,
                               resp_len, p2_ms, p2star_ms, pending_out,
                               skip);

    obd_chip_stats_t st;

    if (err == ESP_OK && obd_chip_get_stats(&st) == ESP_OK)
    {
        /* still holding the chip: this tx_bytes is ours alone */
        s_setup_addr = *addr;
        s_setup_p2_ms = p2_ms;
        s_setup_tx_bytes = st.tx_bytes;
        s_setup_valid = true;
    }
    else
    {
        s_setup_valid = false; /* re-address before the next one */
    }

    obd_chip_txn_end();
    return err;
}

const uds_transport_t *uds_transport_obd(void)
{
    static const uds_transport_t T =
    {
        .name       = "obd_chip",
        .open       = obd_open,
        .transceive = obd_transceive,
    };

    return &T;
}
