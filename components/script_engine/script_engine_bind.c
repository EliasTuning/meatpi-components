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
 * @file script_engine_bind.c
 * @brief The device bindings exposed to Berry scripts — the scripting
 *        API (SCRIPTING.md). v1 surface:
 *
 *   log(msg)                     -> print to the run output + log
 *   sleep_ms(n)                  -> delay (budget-checked)
 *   millis()                     -> uptime ms
 *   uds(tx, rx, hexreq)          -> response hex string, or nil;
 *                                   sets uds_nrc / uds_ok globals
 *   uds_ext(tx, rx, hexreq)      -> same, 29-bit addressing
 *   can_tx(id, ext, hexbytes)    -> bool (native CAN via can_manager)
 *   emit(source, name, key, val) -> fire an event (one kv, string val)
 *   dtc_scan()                   -> DTC report JSON string, or nil
 *                                   (sync; gated by dtc_enabled)
 *   dtc_clear(codes?, mode?)     -> bool cleared (gated by dtc_enabled
 *                                   + dtc_allow_clear; mode 04 = ALL)
 *
 * Bindings check the budget/kill switch at every I/O point.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "berry.h"

#include "autopid.h"
#include "can_manager.h"
#include "event_manager.h"
#include "obd_gate.h"
#include "uds_manager.h"
#include "uds_proto.h"

#include "script_engine_doc.h"
#include "script_engine_obd.h"
#include "script_engine_private.h"

/* ---- helpers --------------------------------------------------------------- */

static uint32_t id_arg(bvm *vm, int idx)
{
    if (be_isint(vm, idx))
    {
        return (uint32_t)be_toint(vm, idx);
    }

    if (be_isstring(vm, idx))
    {
        return (uint32_t)strtoul(be_tostring(vm, idx), NULL, 16);
    }

    return 0;
}

/* ---- bindings -------------------------------------------------------------- */

static int b_log(bvm *vm)
{
    se_check_budget(vm);

    if (be_top(vm) >= 1 && be_isstring(vm, 1))
    {
        const char *s = be_tostring(vm, 1);
        be_writebuffer(s, strlen(s));
        be_writebuffer("\n", 1);
    }

    be_return_nil(vm);
}

static int b_sleep_ms(bvm *vm)
{
    se_check_budget(vm);

    if (be_top(vm) >= 1 && be_isint(vm, 1))
    {
        bint ms = be_toint(vm, 1);

        if (ms > 0 && ms <= SE_SLEEP_MAX_MS)
        {
            vTaskDelay(pdMS_TO_TICKS(ms));
        }
    }

    se_check_budget(vm);
    be_return_nil(vm);
}

static int b_millis(bvm *vm)
{
    be_pushint(vm, (bint)(esp_timer_get_time() / 1000));
    be_return(vm);
}

/* ---- the exclusive-bus hold ------------------------------------------------- */
/* `exclusive` setting: a script's first ECU access asks the background
 * pollers (autopid: PID polling + DTC scans) off the bus — obd_gate's
 * diagnostics hold, refcounted against the UDS Tool's and J2534's own
 * holds — and waits for their ack (autopid loops within 500 ms) so the
 * first request lands on a quiet bus. Released by the runner after the
 * run (se_bus_hold_release). Runs are serialized, plain statics suffice. */
static const int s_bus_token;
static bool s_bus_held;

static void se_bus_hold(void)
{
    if (s_bus_held || !se_settings_exclusive())
    {
        return;
    }

    s_bus_held = true;
    obd_gate_diag_hold(&s_bus_token, true);
    (void)obd_gate_diag_wait_ack(700);
}

void se_bus_hold_release(void)
{
    if (!s_bus_held)
    {
        return;
    }

    s_bus_held = false;
    obd_gate_diag_hold(&s_bus_token, false);
}

static int uds_common(bvm *vm, bool ext)
{
    se_check_budget(vm);

    if (be_top(vm) < 3 || !be_isstring(vm, 3))
    {
        be_raise(vm, "uds_error", "uds(tx, rx, hexrequest)");
    }

    uds_addr_t addr =
    {
        .tx_id  = id_arg(vm, 1),
        .rx_id  = id_arg(vm, 2),
        .ext_id = ext,
    };

    uint8_t reqb[64];
    size_t reqn = 0;

    if (!uds_hex_to_bytes(be_tostring(vm, 3), reqb, sizeof(reqb), &reqn) ||
        reqn == 0)
    {
        be_raise(vm, "uds_error", "bad hex request");
    }

    static EXT_RAM_BSS_ATTR uint8_t respb[512]; /* serialized; PSRAM */
    size_t respn = 0;
    uds_result_t res;

    se_bus_hold(); /* exclusive setting: autopid off the bus, now */

    esp_err_t err = uds_request(&addr, reqb, reqn, respb, sizeof(respb),
                                &respn, NULL, &res);

    /* expose outcome as globals the script can read */
    be_pushint(vm, (err == ESP_OK) ? 1 : 0);
    be_setglobal(vm, "uds_ok");
    be_pop(vm, 1);
    be_pushint(vm, (err == ESP_OK && res.negative) ? res.nrc : -1);
    be_setglobal(vm, "uds_nrc");
    be_pop(vm, 1);

    if (err != ESP_OK)
    {
        be_pushnil(vm);
        be_return(vm);
    }

    char hex[3 * 128 + 1];
    uds_bytes_to_hex(respb, respn < 128 ? respn : 128, hex, sizeof(hex));
    be_pushstring(vm, hex);
    be_return(vm);
}

static int b_uds(bvm *vm)      { return uds_common(vm, false); }
static int b_uds_ext(bvm *vm)  { return uds_common(vm, true); }

static int b_can_tx(bvm *vm)
{
    se_check_budget(vm);

    if (be_top(vm) < 3 || !be_isstring(vm, 3))
    {
        be_raise(vm, "can_error", "can_tx(id, ext, hexbytes)");
    }

    uint32_t id = id_arg(vm, 1);
    bool ext = be_top(vm) >= 2 && !be_isnil(vm, 2) &&
               (be_isint(vm, 2) ? be_toint(vm, 2) != 0 : false);
    uint8_t data[8];
    size_t dlc = 0;

    if (!uds_hex_to_bytes(be_tostring(vm, 3), data, sizeof(data), &dlc))
    {
        be_raise(vm, "can_error", "bad hex (<=8 bytes)");
    }

    esp_err_t err = can_manager_send(id, ext, false, data, (uint8_t)dlc);
    be_pushbool(vm, err == ESP_OK);
    be_return(vm);
}

static int b_emit(bvm *vm)
{
    se_check_budget(vm);

    if (be_top(vm) < 4 || !be_isstring(vm, 1) || !be_isstring(vm, 2) ||
        !be_isstring(vm, 3) || !be_isstring(vm, 4))
    {
        be_raise(vm, "emit_error", "emit(source, name, key, value)");
    }

    em_event_t ev = { 0 };
    snprintf(ev.source, sizeof(ev.source), "%s", be_tostring(vm, 1));
    snprintf(ev.name, sizeof(ev.name), "%s", be_tostring(vm, 2));
    ev.ts_us = esp_timer_get_time();
    ev.n = 1;
    ev.kv[0].key = "value"; /* fixed key name for v1 */
    ev.kv[0].type = EM_VAL_STR;
    snprintf(ev.kv[0].v.str, sizeof(ev.kv[0].v.str), "%s", be_tostring(vm, 4));

    be_pushbool(vm, event_manager_publish(&ev) == ESP_OK);
    be_return(vm);
}

/* ---- DTC (autopid public surface; TASK_dtc.md §9) --------------------------- */

/** dtc_scan() -> report JSON string, or nil (disabled/busy/failed).
 *  SYNC: waits for the scan job under the run budget (default 10 s
 *  covers the ~6 s worst case). */
static int b_dtc_scan(bvm *vm)
{
    se_check_budget(vm);

    if (autopid_dtc_scan_start() != ESP_OK)
    {
        be_pushnil(vm);
        be_return(vm);
    }

    while (autopid_dtc_scanning())
    {
        se_check_budget(vm);            /* kill switch + runtime cap    */
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    cJSON *report = NULL;

    if (autopid_dtc_report(&report) != ESP_OK)
    {
        be_pushnil(vm);
        be_return(vm);
    }

    char *body = cJSON_PrintUnformatted(report);

    cJSON_Delete(report);

    if (body == NULL)
    {
        be_pushnil(vm);
        be_return(vm);
    }

    be_pushstring(vm, body);            /* Berry copies the string      */
    cJSON_free(body);
    be_return(vm);
}

/** dtc_clear(codes?, mode?) -> bool (true = condition held + cleared).
 *  Gated by dtc_enabled + dtc_allow_clear (both settings, default off). */
static int b_dtc_clear(bvm *vm)
{
    se_check_budget(vm);

    const char *codes = (be_top(vm) >= 1 && be_isstring(vm, 1))
                            ? be_tostring(vm, 1) : NULL;
    const char *mode = (be_top(vm) >= 2 && be_isstring(vm, 2))
                           ? be_tostring(vm, 2) : NULL;
    bool cleared = false;

    (void)autopid_dtc_clear(codes, mode, &cleared);
    be_pushbool(vm, cleared);
    be_return(vm);
}

/** dtc_desc(code) -> database description string, or nil. PSRAM-cache
 *  lookup only — no flash, no budget concern. */
static int b_dtc_desc(bvm *vm)
{
    if (be_top(vm) < 1 || !be_isstring(vm, 1))
    {
        be_raise(vm, "dtc_error", "dtc_desc(code)");
    }

    char desc[96];

    if (autopid_dtc_desc(be_tostring(vm, 1), desc, sizeof(desc)) !=
        ESP_OK)
    {
        be_pushnil(vm);
        be_return(vm);
    }

    be_pushstring(vm, desc);
    be_return(vm);
}

/* ---- obd.* conversation surface (SCRIPTING.md §4 item 2) ------------------- */

/* Device port: se_obd core -> uds_manager (session = tester-present +
 * transport claim; request = full 0x78/NRC pipeline; isotp = raw PDU). */
static int port_session_begin(const se_obd_addr_t *a)
{
    uds_addr_t addr = { .tx_id = a->tx_id, .rx_id = a->rx_id,
                        .ext_id = a->ext_id };

    se_bus_hold();
    return (uds_session_begin(&addr) == ESP_OK) ? 0 : -1;
}

static void port_session_end(void)
{
    (void)uds_session_end();
}

static int port_request(const se_obd_addr_t *a,
                        const uint8_t *req, size_t req_len,
                        uint32_t timeout_ms,
                        uint8_t *resp, size_t resp_cap, size_t *resp_len,
                        se_obd_outcome_t *out)
{
    uds_addr_t addr = { .tx_id = a->tx_id, .rx_id = a->rx_id,
                        .ext_id = a->ext_id };
    uds_opts_t opts = { .p2_ms = timeout_ms };

    se_bus_hold();
    uds_result_t res;

    esp_err_t err = uds_request(&addr, req, req_len, resp, resp_cap,
                                resp_len, timeout_ms ? &opts : NULL, &res);

    if (out != NULL)
    {
        out->negative      = (err == ESP_OK) && res.negative;
        out->nrc           = res.nrc;
        out->nrc_name      = res.nrc_name;
        out->pending_count = res.pending_count;
    }

    return (err == ESP_OK) ? 0 : -1;
}

static int port_isotp_tx(const se_obd_addr_t *a,
                         const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    uds_addr_t addr = { .tx_id = a->tx_id, .rx_id = a->rx_id,
                        .ext_id = a->ext_id };

    se_bus_hold();
    return (uds_isotp_tx(&addr, data, len, timeout_ms) == ESP_OK) ? 0 : -1;
}

static int port_isotp_rx(const se_obd_addr_t *a,
                         uint8_t *out, size_t cap, size_t *out_len,
                         uint32_t timeout_ms)
{
    uds_addr_t addr = { .tx_id = a->tx_id, .rx_id = a->rx_id,
                        .ext_id = a->ext_id };

    se_bus_hold();
    return (uds_isotp_rx(&addr, out, cap, out_len, timeout_ms) == ESP_OK)
               ? 0 : -1;
}

void se_obd_port_install(void)
{
    static const se_obd_port_t P =
    {
        .session_begin = port_session_begin,
        .session_end   = port_session_end,
        .request       = port_request,
        .isotp_tx      = port_isotp_tx,
        .isotp_rx      = port_isotp_rx,
    };

    se_obd_set_port(&P);
}

/* biggest single UDS request a script may frame (covers a full
 * TransferData block; the ISO-TP layer under this does 8192) */
#define OBD_REQ_MAX 4100

/* reflash services gated by the allow_reflash setting */
static bool obd_sid_is_reflash(uint8_t sid)
{
    return sid == 0x34 || sid == 0x35 || sid == 0x36 || sid == 0x37;
}

/* raise if the request is a reflash service and the gate is off */
static void obd_reflash_gate(bvm *vm, uint8_t sid)
{
    if (obd_sid_is_reflash(sid) && !se_settings_allow_reflash())
    {
        be_raise(vm, "obd_error",
                 "reflash blocked (enable script_engine.allow_reflash)");
    }
}

/* accept only /sd/... paths, no '..' — keeps script file I/O off the
 * internal-flash littlefs (PSRAM-stack cache trap) and out of traversal */
static bool obd_path_ok(const char *p)
{
    return p != NULL && strncmp(p, "/sd/", 4) == 0 && strstr(p, "..") == NULL;
}

/* injected reader for se_obd_transfer_file: pread from an open SD file */
typedef struct
{
    FILE  *f;
    size_t base;
} obd_file_rd_t;

static int obd_file_rd_cb(void *vctx, size_t offset, uint8_t *out, size_t len)
{
    obd_file_rd_t *c = vctx;

    if (fseek(c->f, (long)(c->base + offset), SEEK_SET) != 0)
    {
        return -1;
    }

    return (int)fread(out, 1, len, c->f);
}

static void obd_set_outcome_globals(bvm *vm, int ok,
                                    const se_obd_outcome_t *out)
{
    be_pushint(vm, ok);
    be_setglobal(vm, "uds_ok");
    be_pop(vm, 1);
    be_pushint(vm, (ok && out != NULL && out->negative) ? out->nrc : -1);
    be_setglobal(vm, "uds_nrc");
    be_pop(vm, 1);
    be_pushint(vm, (ok && out != NULL) ? out->pending_count : 0);
    be_setglobal(vm, "uds_pending");
    be_pop(vm, 1);
}

/* obd_claim(tx, rx[, ext]) — arms tester-present + holds the transport
 * until obd_release() or script end (auto-release). */
static int b_obd_claim(bvm *vm)
{
    se_check_budget(vm);

    if (be_top(vm) < 2)
    {
        be_raise(vm, "obd_error", "obd_claim(tx, rx[, ext])");
    }

    se_obd_addr_t addr =
    {
        .tx_id  = id_arg(vm, 1),
        .rx_id  = id_arg(vm, 2),
        .ext_id = (be_top(vm) >= 3 && be_isint(vm, 3) && be_toint(vm, 3)),
    };

    int r = se_obd_claim(&addr);

    if (r == SE_OBD_ERR_BUSY)
    {
        be_raise(vm, "obd_error", "already claimed to another ECU");
    }

    be_pushint(vm, (r == SE_OBD_OK) ? 1 : 0);
    be_return(vm);
}

static int b_obd_release(bvm *vm)
{
    (void)se_obd_release();
    be_return_nil(vm);
}

/* obd_request(hexreq[, timeout_ms]) -> response hex or nil; sets
 * uds_ok/uds_nrc/uds_pending. The 0x78 responsePending loop and NRC
 * decode run inside uds_manager. */
static int b_obd_request(bvm *vm)
{
    se_check_budget(vm);

    if (be_top(vm) < 1 || !be_isstring(vm, 1))
    {
        be_raise(vm, "obd_error", "obd_request(hexreq[, timeout_ms])");
    }

    static EXT_RAM_BSS_ATTR uint8_t reqb[OBD_REQ_MAX]; /* serialized; PSRAM */
    size_t reqn = 0;

    if (!uds_hex_to_bytes(be_tostring(vm, 1), reqb, sizeof(reqb), &reqn) ||
        reqn == 0)
    {
        be_raise(vm, "obd_error", "bad hex request");
    }

    obd_reflash_gate(vm, reqb[0]);

    uint32_t timeout = (be_top(vm) >= 2 && be_isint(vm, 2))
                           ? (uint32_t)be_toint(vm, 2) : 0;

    static EXT_RAM_BSS_ATTR uint8_t respb[512]; /* serialized; PSRAM */
    size_t respn = 0;
    se_obd_outcome_t out;

    int r = se_obd_request(reqb, reqn, timeout, respb, sizeof(respb),
                           &respn, &out);

    if (r == SE_OBD_ERR_NOCLAIM)
    {
        be_raise(vm, "obd_error", "obd_claim() first");
    }

    obd_set_outcome_globals(vm, (r == SE_OBD_OK) ? 1 : 0,
                            (r == SE_OBD_OK) ? &out : NULL);

    if (r != SE_OBD_OK)
    {
        be_pushnil(vm);
        be_return(vm);
    }

    char hex[3 * 128 + 1];
    uds_bytes_to_hex(respb, respn < 128 ? respn : 128, hex, sizeof(hex));
    be_pushstring(vm, hex);
    be_return(vm);
}

static int b_obd_isotp_tx(bvm *vm)
{
    se_check_budget(vm);

    if (be_top(vm) < 1 || !be_isstring(vm, 1))
    {
        be_raise(vm, "obd_error", "obd_isotp_tx(hexbytes[, timeout_ms])");
    }

    static EXT_RAM_BSS_ATTR uint8_t data[OBD_REQ_MAX]; /* serialized; PSRAM */
    size_t n = 0;

    if (!uds_hex_to_bytes(be_tostring(vm, 1), data, sizeof(data), &n) ||
        n == 0)
    {
        be_raise(vm, "obd_error", "bad hex");
    }

    obd_reflash_gate(vm, data[0]);

    uint32_t timeout = (be_top(vm) >= 2 && be_isint(vm, 2))
                           ? (uint32_t)be_toint(vm, 2) : 500;

    int r = se_obd_isotp_tx(data, n, timeout);

    if (r == SE_OBD_ERR_NOCLAIM)
    {
        be_raise(vm, "obd_error", "obd_claim() first");
    }

    be_pushint(vm, (r == SE_OBD_OK) ? 1 : 0);
    be_return(vm);
}

static int b_obd_isotp_rx(bvm *vm)
{
    se_check_budget(vm);

    uint32_t timeout = (be_top(vm) >= 1 && be_isint(vm, 1))
                           ? (uint32_t)be_toint(vm, 1) : 500;

    static EXT_RAM_BSS_ATTR uint8_t buf[512]; /* serialized; PSRAM */
    size_t got = 0;

    int r = se_obd_isotp_rx(buf, sizeof(buf), &got, timeout);

    if (r == SE_OBD_ERR_NOCLAIM)
    {
        be_raise(vm, "obd_error", "obd_claim() first");
    }

    if (r != SE_OBD_OK)
    {
        be_pushnil(vm);
        be_return(vm);
    }

    char hex[3 * 128 + 1];
    uds_bytes_to_hex(buf, got < 128 ? got : 128, hex, sizeof(hex));
    be_pushstring(vm, hex);
    be_return(vm);
}

/* obd_file_size(path) -> byte count, or nil. SD-only. */
static int b_obd_file_size(bvm *vm)
{
    if (be_top(vm) < 1 || !be_isstring(vm, 1) ||
        !obd_path_ok(be_tostring(vm, 1)))
    {
        be_raise(vm, "obd_error", "obd_file_size(\"/sd/...\")");
    }

    FILE *f = fopen(be_tostring(vm, 1), "rb");

    if (f == NULL)
    {
        be_pushnil(vm);
        be_return(vm);
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);

    if (sz < 0)
    {
        be_pushnil(vm);
        be_return(vm);
    }

    be_pushint(vm, (bint)sz);
    be_return(vm);
}

/* obd_file_read(path, offset, len) -> hex, or nil. SD-only; len capped. */
static int b_obd_file_read(bvm *vm)
{
    se_check_budget(vm);

    if (be_top(vm) < 3 || !be_isstring(vm, 1) || !be_isint(vm, 2) ||
        !be_isint(vm, 3) || !obd_path_ok(be_tostring(vm, 1)))
    {
        be_raise(vm, "obd_error", "obd_file_read(\"/sd/..\", off, len)");
    }

    long off = (long)be_toint(vm, 2);
    size_t len = (size_t)be_toint(vm, 3);

    if (len == 0 || len > 512)
    {
        be_raise(vm, "obd_error", "len 1..512");
    }

    FILE *f = fopen(be_tostring(vm, 1), "rb");

    if (f == NULL || fseek(f, off, SEEK_SET) != 0)
    {
        if (f) { fclose(f); }
        be_pushnil(vm);
        be_return(vm);
    }

    uint8_t buf[512];
    size_t got = fread(buf, 1, len, f);

    fclose(f);

    char hex[3 * 512 + 1];

    uds_bytes_to_hex(buf, got, hex, sizeof(hex));
    be_pushstring(vm, hex);
    be_return(vm);
}

/* obd_transfer_file(path, offset, size, block_len[, first_bsc]) -> bytes
 * sent (nil on failure). Streams UDS TransferData; sets uds_ok +
 * uds_xfer_crc/uds_xfer_blocks globals. Gated by allow_reflash. */
static int b_obd_transfer_file(bvm *vm)
{
    se_check_budget(vm);

    if (be_top(vm) < 4 || !be_isstring(vm, 1) || !be_isint(vm, 2) ||
        !be_isint(vm, 3) || !be_isint(vm, 4) ||
        !obd_path_ok(be_tostring(vm, 1)))
    {
        be_raise(vm, "obd_error",
                 "obd_transfer_file(\"/sd/..\", off, size, block_len[, bsc])");
    }

    if (!se_settings_allow_reflash())
    {
        be_raise(vm, "obd_error",
                 "reflash blocked (enable script_engine.allow_reflash)");
    }

    obd_file_rd_t rd = { .base = (size_t)be_toint(vm, 2) };
    size_t size = (size_t)be_toint(vm, 3);
    size_t block = (size_t)be_toint(vm, 4);
    uint8_t bsc = (be_top(vm) >= 5 && be_isint(vm, 5))
                      ? (uint8_t)be_toint(vm, 5) : 1;

    rd.f = fopen(be_tostring(vm, 1), "rb");

    if (rd.f == NULL)
    {
        be_raise(vm, "obd_error", "cannot open firmware file");
    }

    se_obd_xfer_result_t res;
    int r = se_obd_transfer_file(obd_file_rd_cb, &rd, size, block, bsc,
                                 5000, &res);

    fclose(rd.f);

    if (r == SE_OBD_ERR_NOCLAIM)
    {
        be_raise(vm, "obd_error", "obd_claim() first");
    }

    be_pushint(vm, (r == SE_OBD_OK) ? 1 : 0);
    be_setglobal(vm, "uds_ok");
    be_pop(vm, 1);
    be_pushint(vm, (bint)res.crc32);
    be_setglobal(vm, "uds_xfer_crc");
    be_pop(vm, 1);
    be_pushint(vm, (bint)res.blocks);
    be_setglobal(vm, "uds_xfer_blocks");
    be_pop(vm, 1);

    if (r != SE_OBD_OK)
    {
        ESP_LOGW("script_engine", "transfer_file: %s",
                 res.msg ? res.msg : "?");
        be_pushnil(vm);
        be_return(vm);
    }

    be_pushint(vm, (bint)res.sent);
    be_return(vm);
}

/* uds_nrc_str(nrc) -> ISO 14229 name ("" when unknown) */
static int b_uds_nrc_str(bvm *vm)
{
    if (be_top(vm) < 1 || !be_isint(vm, 1))
    {
        be_raise(vm, "obd_error", "uds_nrc_str(nrc)");
    }

    be_pushstring(vm, uds_nrc_name((uint8_t)be_toint(vm, 1)));
    be_return(vm);
}

/* ---- trigger-event context --------------------------------------------------- */

/* Copy of the event that triggered this run (event_manager `script.run`
 * action / `script` rule body); zeroed source = manual run. Run is
 * serialized, so one slot suffices. */
static em_event_t s_trigger;
static bool s_trigger_valid;

void se_set_trigger(const em_event_t *ev)
{
    if (ev != NULL)
    {
        s_trigger = *ev;
        s_trigger_valid = true;
    }
    else
    {
        s_trigger_valid = false;
    }
}

static void set_str_global(bvm *vm, const char *name, const char *val)
{
    be_pushstring(vm, val);
    be_setglobal(vm, name);
    be_pop(vm, 1);
}

/* evt_source/evt_name ("" on manual runs) + one global per trigger kv:
 * evt_<key> typed int/real/bool→int/string. */
static void trigger_globals_register(bvm *vm)
{
    set_str_global(vm, "evt_source", s_trigger_valid ? s_trigger.source : "");
    set_str_global(vm, "evt_name", s_trigger_valid ? s_trigger.name : "");

    if (!s_trigger_valid)
    {
        return;
    }

    for (uint8_t i = 0; i < s_trigger.n && i < EM_KV_MAX; i++)
    {
        const em_kv_t *kv = &s_trigger.kv[i];
        char gname[32];

        if (kv->key == NULL)
        {
            continue;
        }

        snprintf(gname, sizeof(gname), "evt_%s", kv->key);

        switch (kv->type)
        {
            case EM_VAL_I64:
                be_pushint(vm, (bint)kv->v.i64);
                break;
            case EM_VAL_F64:
                be_pushreal(vm, (breal)kv->v.f64);
                break;
            case EM_VAL_BOOL:
                be_pushint(vm, kv->v.b ? 1 : 0);
                break;
            default:
                be_pushstring(vm, kv->v.str);
                break;
        }

        be_setglobal(vm, gname);
        be_pop(vm, 1);
    }
}

/* ---- registration ---------------------------------------------------------- */

/* The functions behind the reference: script_engine_doc.c documents each
 * one (name, signature, group, doc, example) and se_bindings_selfcheck()
 * reports any drift between the two tables at boot. Adding a binding =
 * one line here + one line there. */
static const struct
{
    const char *name;
    bntvfunc    fn;
} BINDINGS[] =
{
    { "log",               b_log },
    { "sleep_ms",          b_sleep_ms },
    { "millis",            b_millis },
    { "uds",               b_uds },
    { "uds_ext",           b_uds_ext },
    { "can_tx",            b_can_tx },
    { "emit",              b_emit },
    { "dtc_scan",          b_dtc_scan },
    { "dtc_clear",         b_dtc_clear },
    { "dtc_desc",          b_dtc_desc },
    { "obd_claim",         b_obd_claim },
    { "obd_release",       b_obd_release },
    { "obd_request",       b_obd_request },
    { "obd_isotp_tx",      b_obd_isotp_tx },
    { "obd_isotp_rx",      b_obd_isotp_rx },
    { "obd_file_size",     b_obd_file_size },
    { "obd_file_read",     b_obd_file_read },
    { "obd_transfer_file", b_obd_transfer_file },
    { "uds_nrc_str",       b_uds_nrc_str },
};

#define N_BINDINGS (sizeof(BINDINGS) / sizeof(BINDINGS[0]))

static void reg(bvm *vm, const char *name, bntvfunc f)
{
    be_pushntvfunction(vm, f);
    be_setglobal(vm, name);
    be_pop(vm, 1);
}

bool se_bindings_selfcheck(void)
{
    bool ok = true;

    for (size_t i = 0; i < N_BINDINGS; i++)
    {
        if (se_bind_doc_find(BINDINGS[i].name) == NULL)
        {
            ESP_LOGE("script_engine", "binding '%s' has no reference entry",
                     BINDINGS[i].name);
            ok = false;
        }
    }

    for (size_t i = 0; i < se_bind_doc_count(); i++)
    {
        const char *name = se_bind_doc(i)->name;
        bool found = false;

        for (size_t j = 0; j < N_BINDINGS && !found; j++)
        {
            found = strcmp(BINDINGS[j].name, name) == 0;
        }

        if (!found)
        {
            ESP_LOGE("script_engine", "reference entry '%s' has no binding",
                     name);
            ok = false;
        }
    }

    return ok;
}

void se_bindings_register(bvm *vm)
{
    trigger_globals_register(vm);

    for (size_t i = 0; i < N_BINDINGS; i++)
    {
        reg(vm, BINDINGS[i].name, BINDINGS[i].fn);
    }

    /* Pre-declare the uds() out-globals so scripts can read them (Berry
     * flags a bare identifier that was never assigned at compile time). */
    be_pushint(vm, 0);
    be_setglobal(vm, "uds_ok");
    be_pop(vm, 1);
    be_pushint(vm, -1);
    be_setglobal(vm, "uds_nrc");
    be_pop(vm, 1);
    be_pushint(vm, 0);
    be_setglobal(vm, "uds_pending");
    be_pop(vm, 1);
    be_pushint(vm, 0);
    be_setglobal(vm, "uds_xfer_crc");
    be_pop(vm, 1);
    be_pushint(vm, 0);
    be_setglobal(vm, "uds_xfer_blocks");
    be_pop(vm, 1);
}
