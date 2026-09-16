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
 * @file autopid_runner.c
 * @brief The chip-facing runner: one PID poll end to end — init strings
 *        on type/PID transitions, ATCRA rxheader management, request,
 *        payload parse + cross-talk guard, expression eval of EVERY
 *        enabled parameter from the ONE response (fix #1) into the cache.
 *
 * Owns the "what is the chip currently set up for" memory. Poller-task
 * context only (plus ap_runner_reset from the scan job / settings).
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "battery_monitor.h"
#include "obd_chip.h"

#include "autopid_transport.h"

#include "expression_parser.h"

#include "autopid_private.h"

static const char *TAG = "autopid";

#define AP_REQ_TIMEOUT   pdMS_TO_TICKS(1500)
#define AP_INIT_TIMEOUT  pdMS_TO_TICKS(2000)

/* chip-state memory: which type/PID the chip is currently set up for */
static char s_type_init[3][AP_INIT_LEN];
static int  s_last_type = -1;
static int  s_last_pid = -1;
static bool s_rxheader_set;

void ap_runner_set_type_init(int type, const char *init)
{
    if (type >= 0 && type < 3)
    {
        snprintf(s_type_init[type], sizeof(s_type_init[type]), "%s",
                 (init != NULL) ? init : "");
    }
}

void ap_runner_reset(void)
{
    /* someone else (std scan, settings apply) changed protocol/header
       state under the chip — replay inits on the next poll */
    s_last_type = -1;
    s_last_pid = -1;
    s_rxheader_set = false;
}

/* ---- test-a-PID transcript: one line per exchange, "> cmd" / "< reply"
   (2026-09-16: the UI shows what went out and what came back) ---------- */
typedef struct
{
    char  *buf;                 /* NULL = no transcript wanted           */
    size_t cap;
    size_t len;
} ap_tr_t;

static void tr_add(ap_tr_t *t, char dir, const char *s)
{
    if (t == NULL || t->buf == NULL || t->cap == 0)
    {
        return;
    }

    /* replies are flattened to one line (a multi-frame answer arrives as
       several lines) and capped so a long ISO-TP reply cannot eat the
       whole buffer; the full raw reply travels separately */
    char line[200];
    size_t n = 0;

    line[n++] = dir;
    line[n++] = ' ';

    const char *p = s;

    for (; *p != '\0' && n < sizeof(line) - 5; p++)
    {
        if (*p == '\r' || *p == '\n')
        {
            if (line[n - 1] != ' ')
            {
                line[n++] = ' ';
            }
        }
        else
        {
            line[n++] = *p;
        }
    }

    while (n > 2 && line[n - 1] == ' ')
    {
        n--;
    }

    if (*p != '\0')
    {
        line[n++] = '.';
        line[n++] = '.';
        line[n++] = '.';
    }

    line[n++] = '\n';
    line[n] = '\0';

    if (t->len + n < t->cap)
    {
        memcpy(t->buf + t->len, line, n + 1);
        t->len += n;
    }
}

/** One chip exchange, logged into the transcript when one is wanted. */
static esp_err_t request_tr(const char *cmd, char *resp, size_t resp_len,
                            TickType_t timeout, ap_tr_t *tr)
{
    resp[0] = '\0';
    tr_add(tr, '>', cmd);

    esp_err_t err = ap_be()->request(cmd, resp, resp_len, timeout);

    tr_add(tr, '<', (err != ESP_OK) ? "(no reply)"
                    : (resp[0] == '\0') ? "(empty)" : resp);
    return err;
}

/** Send a ';'-separated init string, one command at a time. */
static void send_init_tr(const char *init, ap_tr_t *tr)
{
    char cmd[AP_INIT_LEN];
    size_t n = 0;

    for (const char *p = init;; p++)
    {
        if (*p == ';' || *p == '\0')
        {
            if (n > 0)
            {
                cmd[n] = '\0';
                ap_init_sanitize(cmd); /* spare the chip's EEPROM */

                char resp[64];

                (void)request_tr(cmd, resp, sizeof(resp), AP_INIT_TIMEOUT,
                                 tr);
                n = 0;
            }

            if (*p == '\0')
            {
                break;
            }
        }
        else if (n < sizeof(cmd) - 1)
        {
            cmd[n++] = *p;
        }
    }
}

static void send_init(const char *init)
{
    send_init_tr(init, NULL);
}

void ap_runner_restore_baseline(void)
{
    /* the app's ATZ/ATS0/ATH1/ATSH/ATCRA are all still in effect: put
       back what the parser and the std PIDs assume (the same prelude
       the std scan uses - ATTP not ATSP, so no EEPROM write), then let
       the next poll replay the configured type/PID inits on top */
    send_init(ap_std_prelude());
    ap_runner_reset();
}

esp_err_t ap_runner_test(const char *type_init, const char *init,
                         const char *rxheader, const char *cmd, char *raw,
                         size_t raw_len, int64_t *elapsed_us,
                         char *transcript, size_t transcript_len)
{
    /* ONE-SHOT through the same chip choreography as a real poll
       (test-a-PID, §11): the type init chain (what the poller sends when
       it switches to this PID's type), the per-PID init, ATCRA, the
       request, ATCRA off. Caller must hold the poller paused
       (ap_core_scan_pause) and this leaves the chip state dirty on
       purpose — unpausing runs ap_runner_reset(). httpd-task context
       (internal stack). */
    char resp[64];
    ap_tr_t tr = { transcript, transcript_len, 0 };

    if (transcript != NULL && transcript_len > 0)
    {
        transcript[0] = '\0';
    }

    if (type_init != NULL && type_init[0] != '\0')
    {
        send_init_tr(type_init, &tr);
    }

    if (init != NULL && init[0] != '\0')
    {
        send_init_tr(init, &tr);
    }

    bool cra_set = false;

    if (rxheader != NULL && rxheader[0] != '\0')
    {
        char cra[AP_HDR_LEN + 8];

        snprintf(cra, sizeof(cra), "ATCRA%s", rxheader);
        (void)request_tr(cra, resp, sizeof(resp), AP_INIT_TIMEOUT, &tr);
        cra_set = true;
    }

    char cmd_s[AP_CMD_LEN];

    snprintf(cmd_s, sizeof(cmd_s), "%s", cmd);
    ap_init_sanitize(cmd_s); /* the tested cmd can be an AT command */

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = request_tr(cmd_s, raw, raw_len, AP_REQ_TIMEOUT, &tr);

    if (elapsed_us != NULL)
    {
        *elapsed_us = esp_timer_get_time() - t0;
    }

    if (cra_set)
    {
        (void)request_tr("ATCRA", resp, sizeof(resp), AP_INIT_TIMEOUT,
                         &tr);
    }

    return err;
}

const char *ap_runner_type_init(int type)
{
    return (type >= 0 && type < 3) ? s_type_init[type] : "";
}

bool ap_runner_run(const ap_pid_t *pid, int pid_index,
                   const ap_param_t *params)
{
    /* type init once per type transition (legacy behavior kept) */
    if (pid->type != s_last_type)
    {
        if (s_type_init[pid->type][0] != '\0')
        {
            send_init(s_type_init[pid->type]);
        }

        s_last_type = pid->type;
        s_last_pid = -1; /* force per-PID setup after a type switch */
    }

    if (pid_index != s_last_pid)
    {
        if (pid->init[0] != '\0')
        {
            send_init(pid->init);
        }

        char resp[64];

        if (pid->rxheader[0] != '\0')
        {
            char cra[AP_HDR_LEN + 8];

            snprintf(cra, sizeof(cra), "ATCRA%s", pid->rxheader);
            (void)ap_be()->request(cra, resp, sizeof(resp),
                                   AP_INIT_TIMEOUT);
            s_rxheader_set = true;
        }
        else if (s_rxheader_set)
        {
            (void)ap_be()->request("ATCRA", resp, sizeof(resp),
                                   AP_INIT_TIMEOUT);
            s_rxheader_set = false;
        }

        s_last_pid = pid_index;
    }

    static char resp[AP_RESP_MAX] EXT_RAM_BSS_ATTR; /* poller-task only */

    esp_err_t err = ap_be()->request(pid->cmd, resp, sizeof(resp),
                                     AP_REQ_TIMEOUT);

    if (err != ESP_OK)
    {
        ESP_LOGD(TAG, "%s: request failed (%s)", pid->name,
                 esp_err_to_name(err));
        return false;
    }

    uint8_t payload[AP_PAYLOAD_MAX];
    size_t payload_len = 0;

    if (ap_resp_to_payload(resp, payload, sizeof(payload), &payload_len)
            != ESP_OK)
    {
        ESP_LOGD(TAG, "%s: no payload in response", pid->name);
        return false;
    }

    /* cross-talk guard: another chip master's response (WS-OBD bridge,
       CLI) can land in our request window — never cache it (Phase 1b) */
    if (!ap_payload_matches_cmd(pid->cmd, payload, payload_len))
    {
        ESP_LOGD(TAG, "%s: response echo mismatch (cross-talk?)",
                 pid->name);
        return false;
    }

    float volts = 0;

    (void)battery_monitor_voltage(&volts);

    int64_t now = esp_timer_get_time();
    bool any = false;

    for (uint16_t i = 0; i < pid->param_count; i++)
    {
        const ap_param_t *prm = &params[i];

        if (!prm->enabled)
        {
            continue;
        }

        /* mux precondition (DBC m<N> signals): the parameter only
           applies when the switch slice equals its mux value */
        if (prm->mux_expr[0] != '\0')
        {
            double mv = 0;

            if (expression_parser_eval(prm->mux_expr, payload,
                                       payload_len, (double)volts,
                                       &mv) != ESP_OK ||
                fabs(mv - (double)prm->mux_val) > 0.5)
            {
                continue;
            }
        }

        double value = 0;

        if (expression_parser_eval(prm->expression, payload, payload_len,
                                   (double)volts, &value) != ESP_OK)
        {
            ESP_LOGD(TAG, "%s/%s: expression failed", pid->name,
                     prm->name);
            continue;
        }

        /* plausibility clamp: out-of-range readings are dropped, not
           published (mirrors legacy min/max) */
        if ((!isnan(prm->min) && value < prm->min) ||
            (!isnan(prm->max) && value > prm->max))
        {
            ESP_LOGD(TAG, "%s/%s: %f out of range", pid->name, prm->name,
                     value);
            continue;
        }

        ap_cache_put(pid->param_start + i, value, now);
        ap_events_param(prm, pid->param_start + i, pid->group, value);
        any = true;
    }

    return any;
}
