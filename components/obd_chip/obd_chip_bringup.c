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
 * @file obd_chip_bringup.c
 * @brief The chip's boot sequence on the bare UART (before the RX task
 *        exists): wake, reset, baud negotiation, the legacy provisioning
 *        (sleep control, power-on baud, the power-pin switch). Split out
 *        of obd_chip.c 2026-10-03 (700-line rule); obd_chip.c keeps the
 *        bring-up task and its gate.
 */
#include "obd_chip.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "obd_chip_private.h"

static const char *TAG = "obd_chip"; /* one TAG per component (§10) */

#define OBD_INIT_RETRIES 3

/* What the last bare probe saw, prompt or not (issue #1023, 2026-10-28: a
 * chip that is alive but not in ELM mode was reported as silent; a few
 * bytes tell a bootloader or a reboot loop from a dead wire; a chip at
 * 2 Mbaud with its echo ON answers the garbled 115200 bytes with hundreds
 * of bytes of echo read at the wrong speed and no prompt; with the echo
 * off, as the bring-up leaves it, it answers nothing). */
static size_t  s_probe_rx;
static uint8_t s_probe_head[12];
static size_t  s_probe_head_len;

/* the verdict per baud the negotiation tried and got no prompt from, kept
 * for GET /api/obd_chip ("bringup":{"wire":[...]}): the page's banner says
 * what the chip did instead of "not responding" (slot 0 = 2 Mbaud, 1 =
 * 115200; baud 0 = not tried or answered at once) */
static obd_wire_note_t s_wire[2];

size_t obd_chip_bringup_wire(obd_wire_note_t *out, size_t max)
{
    size_t n = 0;

    for (size_t i = 0; i < 2 && n < max; i++)
    {
        if (s_wire[i].baud != 0)
        {
            out[n++] = s_wire[i];
        }
    }

    return n;
}

/** One request->response on the bare UART (RX task not running yet). */
static bool bare_probe(const char *cmd, char *resp, size_t resp_len,
                       uint32_t timeout_ms)
{
    /* static: the accumulator's 4 KB response buffer was over half the
       8 KB main-task stack at boot (2026-07-22 stack audit). Safe as a
       static: bare_probe is boot-provisioning only, before the RX
       task exists, single-threaded by construction. */
    static obd_resp_acc_t acc EXT_RAM_BSS_ATTR;
    uint8_t buf[OBD_CHIP_CHUNK_SIZE];

    obd_uart_flush_input();
    obd_uart_write((const uint8_t *)cmd, strlen(cmd));
    obd_parse_reset(&acc);
    s_probe_rx = 0;
    s_probe_head_len = 0;

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (!acc.done && xTaskGetTickCount() < deadline)
    {
        int got = obd_uart_read(buf, sizeof(buf), 50);

        if (got > 0)
        {
            for (int i = 0; i < got && s_probe_head_len < sizeof(s_probe_head);
                 i++)
            {
                s_probe_head[s_probe_head_len++] = buf[i];
            }

            s_probe_rx += (size_t)got;
            obd_parse_feed(&acc, (const char *)buf, (size_t)got);
        }
    }

    if (acc.done)
    {
        obd_parse_extract(&acc, cmd, resp, resp_len);
        return true;
    }

    return false;
}

/** One W line about a probe that got no prompt: silent, or N bytes with
 *  the first of them in hex. The field log then tells a dead wire from a
 *  chip that answers at another baud or in another mode (issue #1023). */
static void wire_note(const char *what, int baud)
{
    obd_wire_note_t *slot = &s_wire[(baud == OBD_CHIP_BAUD) ? 0 : 1];

    slot->baud = baud;
    slot->bytes = (uint32_t)s_probe_rx;
    slot->prompt = false;
    slot->hex[0] = '\0';

    if (s_probe_rx == 0)
    {
        ESP_LOGW(TAG, "%s at %d baud: nothing received", what, baud);
        return;
    }

    char hex[3 * sizeof(s_probe_head) + 1];
    size_t n = 0;

    for (size_t i = 0; i < s_probe_head_len && n < sizeof(hex) - 3; i++)
    {
        n += (size_t)snprintf(hex + n, sizeof(hex) - n, "%s%02X",
                              (i > 0) ? " " : "", s_probe_head[i]);
    }

    strlcpy(slot->hex, hex, sizeof(slot->hex));
    ESP_LOGW(TAG, "%s at %d baud: %u byte(s) but no prompt (%s%s)", what,
             baud, (unsigned)s_probe_rx, hex,
             (s_probe_rx > s_probe_head_len) ? " ..." : "");
}

/**
 * Boot chip provisioning (legacy main/obd.c obd_init, meatpi 2026-07-04:
 * "follow legacy init"): read STSLCS; ensure sleep control is NATIVE
 * (ATPP 0E) and every autonomous control OFF (the ESP32 owns the sleep
 * policy); keep the chip from waking itself on bus activity while the
 * ESP32 sleeps (ATPP 0F SV 95: the Activity Monitor options with bit 6
 * off). Any change ends in ATZ + re-bring-up. Runs on the bare UART, echo
 * already off. Log-and-degrade.
 *
 * Until 2026-11-07 the block also wrote the voltage thresholds and the
 * UART timeout from the obd_chip settings (STSLVLW, STSLVLS, STSLUIT) and
 * re-provisioned when the stored values differed from them. The sleep
 * triggers bench (the app repo's testing/sleeptrig.md) proved those values
 * inert while the controls are off, so they are neither compared nor
 * written any more: fewer EEPROM writes, and a settings number nobody sees
 * (the UI never shows them) no longer costs a provisioning. Whatever the
 * chip holds there stays as it is.
 */
static void chip_provision(void)
{
    static char resp[1024] EXT_RAM_BSS_ATTR; /* STSLCS is multi-line */
    obd_stslcs_t st;
    bool reset_needed = false;
    bool pp0f_written = false;

    if (!bare_probe("STSLCS\r", resp, sizeof(resp), 1500))
    {
        ESP_LOGW(TAG, "STSLCS: no answer; skipping chip provisioning");
        return;
    }

    obd_stslcs_parse(resp, &st);

    if (strcmp(st.ctrl_mode, "ELM327") == 0)
    {
        /* legacy: sleep control must be Native (the ESP is the master) */
        ESP_LOGI(TAG, "chip sleep control ELM327 -> Native");
        bare_probe("ATPP 0E SV 7A\r", resp, sizeof(resp), 1000);
        bare_probe("ATPP 0E ON\r", resp, sizeof(resp), 1000);
        reset_needed = true;
    }

    if (obd_stslcs_needs_provision(&st))
    {
        ESP_LOGI(TAG, "provisioning sleep config: a control is on (uart "
                 "sleep %d wake %d, voltage sleep %d wake %d); all four off",
                 st.uart_sleep.en, st.uart_wake.en, st.vl_sleep.en,
                 st.vl_wake.en);
        /* byte-exact legacy strings, including the odd casing/space */
        bare_probe("STSLVl off,off\r", resp, sizeof(resp), 1000);
        bare_probe("STSLU off, off\r", resp, sizeof(resp), 1000);
        vTaskDelay(pdMS_TO_TICKS(10));
        pp0f_written = true;

        /* PP 0F = the Activity Monitor options (MIC DS PP area, default
           D5): 95 clears bit 6, "allow wake from Low Power on a shift from
           no activity to activity", so the chip cannot wake itself on bus
           traffic while the ESP32 sleeps (legacy obd_init and its
           elm327_disable_wake_commands). Not the baud: the 2 M power-on
           baud is STWBR's, at the 115200 fallback above. A chip firmware
           rewrite keeps the PP area (bench 2026-11-02). */
        bare_probe("ATPP 0F SV 95\r", resp, sizeof(resp), 1000);
        bare_probe("ATPP 0F ON\r", resp, sizeof(resp), 1000);
        vTaskDelay(pdMS_TO_TICKS(10));
        reset_needed = true;
    }

    /* PP 0F guarded by its own read-back (Ali, 2026-11-02): the compare
       above sees STSLCS, which does not report the Activity Monitor
       options, so a chip image or a tool that put PP 0F back to the
       vendor's D5 (the chip waking itself on bus activity while the ESP32
       sleeps) would stay that way. ATPPS is the one read the chip offers;
       the pair goes out only when the value or its enable differs (the
       boot-path change guard of the standard's section 11). An intact chip
       costs one read of 12 lines and no write; a chip firmware rewrite
       keeps the PP area (bench 2026-11-02), so the write is for the image
       or tool that does not. */
    if (!pp0f_written)
    {
        uint8_t pp0f = 0;
        bool pp0f_on = false;

        if (!bare_probe("ATPPS\r", resp, sizeof(resp), 1500))
        {
            ESP_LOGW(TAG, "ATPPS: no answer; PP 0F not checked");
        }
        else if (!obd_parse_pp(resp, 0x0F, &pp0f, &pp0f_on))
        {
            ESP_LOGW(TAG, "ATPPS: no PP 0F in the answer ('%.24s'); not "
                          "checked", resp);
        }
        else if (pp0f != 0x95 || !pp0f_on)
        {
            ESP_LOGW(TAG, "PP 0F reads %02X %s (expected 95 on): restoring",
                     pp0f, pp0f_on ? "on" : "off");
            bare_probe("ATPP 0F SV 95\r", resp, sizeof(resp), 1000);
            bare_probe("ATPP 0F ON\r", resp, sizeof(resp), 1000);
            vTaskDelay(pdMS_TO_TICKS(10));
            obd_core_pp_restore_note();
            reset_needed = true;
        }
    }

    if (reset_needed)
    {
        /* ATZ reboots the chip at its (possibly new) default baud with
           echo back on: re-run the tail of the bring-up */
        obd_uart_write((const uint8_t *)"ATZ\r", 4);
        vTaskDelay(pdMS_TO_TICKS(1500));
        obd_uart_set_baud(OBD_CHIP_BAUD);
        obd_uart_write((const uint8_t *)"\r", 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        obd_uart_flush_input();

        if (!bare_probe("ATE0\r", resp, sizeof(resp), 1500))
        {
            obd_uart_set_baud(115200); /* chip kept its old default */
            obd_uart_write((const uint8_t *)"\r", 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            obd_uart_flush_input();

            if (bare_probe("ATE0\r", resp, sizeof(resp), 1500))
            {
                ESP_LOGW(TAG, "chip at 115200 after provisioning ATZ");
            }
            else
            {
                ESP_LOGE(TAG, "chip unresponsive after provisioning ATZ");
            }
        }
    }
}

/**
 * Reset an awake chip by command, and make sure it happened.
 *
 * Until 2026-10-03 this was one `ATZ` whose prompt was taken as proof. On a
 * software restart of the ESP the chip usually answered that first command
 * with a prompt and no reset (what the last run left in its line buffer, or
 * a glitch of our TX pin, sat in front of it): bring-up took 852 ms instead
 * of 2096 ms on 15 of 30 logged boots, and the chip kept everything the
 * previous run had set, an open CAN session included. A chip holding a
 * session at 500 kbit/s is a node at 500 kbit/s: on a bus at another bitrate
 * it destroys the traffic (bench: 450 error frames a second on a 250k bus,
 * from a device whose firmware believed the chip was fresh).
 *
 * So: one throw-away command first (whatever is in front of it ends with
 * it, as a `?`), then the reset word, and the answer must carry the banner.
 * Twice; the caller pulses the reset pin when that fails.
 *
 * The word is `ATWS` since 2026-11-02 (what 4.x sent in place of every
 * `ATZ`): the same complete reset (the session and every RAM setting gone,
 * the baud kept) without the LED test, about 40 ms instead of 1.3 s, and
 * without the junk bytes the MIC3624 puts ahead of its `ATZ` banner (5 of
 * 16 tries on the bench, a NUL among them: the "soft reset not confirmed"
 * of every third software restart since 2026-10-19, and a reset pulse plus
 * 1.5 s on top of a chip that had reset fine). The extract behind
 * bare_probe skips such bytes now as well.
 */
static bool chip_soft_reset(char *resp, size_t resp_len)
{
    for (int attempt = 0; attempt < 2; attempt++)
    {
        (void)bare_probe("ATI\r", resp, resp_len, 500);

        if (bare_probe("ATWS\r", resp, resp_len, 2500) &&
            strstr(resp, "ELM") != NULL)
        {
            return true;
        }
    }

    return false;
}

/** The full wake/reset/negotiate/provision sequence: the pre-2026-07-26
 *  synchronous obd_chip_start() body, run by the bring-up task in
 *  obd_chip.c. @p active_baud: the baud the chip answers at (written as the
 *  sequence learns it). ESP_OK = the chip is up and the RX task runs. */
esp_err_t obd_bringup_run(int *active_baud)
{
    char resp[64];

    memset(s_wire, 0, sizeof(s_wire)); /* this round's verdicts only */
    obd_pin_wake();
    vTaskDelay(pdMS_TO_TICKS(300));

    /* legacy elm327_hardreset_chip: hardware reset ONLY when the READY
       pin says asleep/stuck (HIGH); an awake chip gets a soft ATZ */
    obd_uart_set_baud(OBD_CHIP_BAUD);

    if (obd_pin_ready())
    {
        ESP_LOGI(TAG, "chip awake; warm start (ATWS) instead of hardware reset");

        if (!chip_soft_reset(resp, sizeof(resp)))
        {
            ESP_LOGW(TAG, "soft reset not confirmed; hardware reset");
            obd_pin_reset_pulse();
            vTaskDelay(pdMS_TO_TICKS(1500));
        }
    }
    else
    {
        ESP_LOGW(TAG, "chip not ready; hardware reset");
        obd_pin_reset_pulse();
        vTaskDelay(pdMS_TO_TICKS(1500)); /* chip boot time after reset */
    }

    /* baud negotiation: the fixed baud first, then the chip's power-on default */
    const int try_bauds[2] = { OBD_CHIP_BAUD, OBD_CHIP_FALLBACK_BAUD };
    bool alive = false;

    for (size_t b = 0; b < 2 && !alive; b++)
    {
        obd_uart_set_baud(try_bauds[b]);

        for (int attempt = 0; attempt < OBD_INIT_RETRIES && !alive; attempt++)
        {
            /* lone CR clears any stale/monitor state (stop byte is space,
               but on a fresh boot CR-flush is what the legacy flow does) */
            obd_uart_write((const uint8_t *)"\r", 1);
            vTaskDelay(pdMS_TO_TICKS(100));

            alive = bare_probe("ATI\r", resp, sizeof(resp), 1500);
        }

        if (alive)
        {
            *active_baud = try_bauds[b];
        }
        else
        {
            wire_note("ATI", try_bauds[b]); /* the last try at this baud */
        }
    }

    if (!alive)
    {
        ESP_LOGE(TAG, "chip not responding at %d or %d baud", OBD_CHIP_BAUD,
                 OBD_CHIP_FALLBACK_BAUD);
        return ESP_ERR_NOT_FOUND;
    }

    /* switch the chip to the fixed baud if it woke at the fallback */
    if (*active_baud != OBD_CHIP_BAUD)
    {
        char cmd[24];

        snprintf(cmd, sizeof(cmd), "STSBR %d\r", OBD_CHIP_BAUD);
        obd_uart_write((const uint8_t *)cmd, strlen(cmd));
        vTaskDelay(pdMS_TO_TICKS(100));
        obd_uart_set_baud(OBD_CHIP_BAUD);

        if (bare_probe("ATI\r", resp, sizeof(resp), 1500))
        {
            *active_baud = OBD_CHIP_BAUD;
            /* legacy: persist the negotiated baud as the chip's
               power-on default (next boot answers at OBD_CHIP_BAUD) */
            bare_probe("STWBR\r", resp, sizeof(resp), 1500);
        }
        else
        {
            obd_uart_set_baud(OBD_CHIP_FALLBACK_BAUD); /* stay where the chip is */
            *active_baud = OBD_CHIP_FALLBACK_BAUD;
            ESP_LOGW(TAG, "STSBR to %d failed; staying at %d", OBD_CHIP_BAUD,
                     OBD_CHIP_FALLBACK_BAUD);
        }
    }

    /* legacy elm327_powerpin_commands: fw >= V2.3.22 must run with the
       power-pin switch config at 10 (low sleeps, high wakes; v4.52p:
       V2.3.22 or V2.3.24; until 2026-10-28 only a .22 chip got the check).
       The version is kept for GET /api/obd_chip on the way. A wrong value
       is the one EEPROM write of this path: the write acknowledged, left
       OBD_PPSW_SETTLE_MS to settle (legacy's 100 + 500 ms) and read back
       BEFORE the reset pulse (2026-11-07; until then the pulse came 0 ms
       after the OK). An unreadable value is left alone: no blind write. */
    bool have_ver = bare_probe("VTVERS\r", resp, sizeof(resp), 1500);

    if (have_ver)
    {
        obd_chip_version_note(resp);

        if (obd_fw_version_is_outdated(resp, obd_chip_builtin_fw_version()))
        {
            /* accepted (left alone by the start-up update) but older than
               the packaged image: the card on System > Maintenance and
               `obd_chip` say the same (Ali, 2026-10-28) */
            ESP_LOGW(TAG, "chip firmware %.*s is older than the packaged %s: "
                     "update it from System > Maintenance or with "
                     "obd_chip update",
                     (int)strcspn(resp, "\r\n>"), resp,
                     obd_chip_builtin_fw_version());
        }
    }

    if (have_ver && obd_fw_version_is_current(resp))
    {
        uint8_t ppsw = 0;

        resp[0] = '\0';

        if (!bare_probe("VTPPSWS\r", resp, sizeof(resp), 1500) ||
            !obd_parse_ppsw(resp, &ppsw))
        {
            ESP_LOGW(TAG, "PPSW unreadable ('%.12s'): left as it is", resp);
        }
        else if (ppsw != 0x10)
        {
            ESP_LOGW(TAG, "PPSW is %02X, setting to 10", ppsw);
            resp[0] = '\0';

            if (!bare_probe("VTPPSW10\r", resp, sizeof(resp), 1500) ||
                !obd_parse_is_ok(resp))
            {
                ESP_LOGW(TAG, "VTPPSW10 not acknowledged ('%.12s'): no reset, "
                         "the value left as it is", resp);
            }
            else
            {
                vTaskDelay(pdMS_TO_TICKS(OBD_PPSW_SETTLE_MS));
                resp[0] = '\0';

                if (bare_probe("VTPPSWS\r", resp, sizeof(resp), 1500) &&
                    obd_parse_ppsw(resp, &ppsw) && ppsw == 0x10)
                {
                    ESP_LOGI(TAG, "PPSW reads 10 after the write");
                }
                else
                {
                    ESP_LOGW(TAG, "PPSW reads '%.12s' after the write, not 10",
                             resp);
                }

                obd_pin_reset_pulse(); /* legacy hard-resets after the change */
                vTaskDelay(pdMS_TO_TICKS(1500));
                obd_uart_write((const uint8_t *)"\r", 1);
                vTaskDelay(pdMS_TO_TICKS(100));
                obd_uart_flush_input();
            }
        }
    }

    /* legacy: wait for the READY pin (LOW = ready) before declaring the
       chip up; one forced reset if it never settles */
    for (int i = 0; !obd_pin_ready() && i < 10; i++)
    {
        ESP_LOGW(TAG, "chip not ready...");
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (!obd_pin_ready())
    {
        ESP_LOGE(TAG, "chip not ready for too long; hardware reset");
        obd_pin_reset_pulse();
        vTaskDelay(pdMS_TO_TICKS(1500));
    }

    /* echo off first (deterministic responses), then the legacy chip
       provisioning (task §11-5 answered by meatpi: "follow legacy init") */
    bare_probe("ATE0\r", resp, sizeof(resp), 1000);
    chip_provision();

    /* the memory function off (issue #1023, bench 2026-11-01): it is ON
       after every reset (the vendor's pin strap) and then writes every
       protocol switch a request answers on into the chip's EEPROM-backed
       protocol store; legacy's AutoPID sent `atm0` at every start, v6 never
       did. Here, after the provisioning's own ATZ, for every reset this
       sequence performs; the request engine repeats it ahead of every
       protocol switch (obd_chip_cmd.c), so a chip that stays silent here
       is covered at its first switch. */
    bool mem_off = bare_probe("ATM0\r", resp, sizeof(resp), 1000) &&
                   strstr(resp, "OK") != NULL;

    if (mem_off)
    {
        obd_core_memory_off_note();
    }
    else
    {
        ESP_LOGW(TAG, "ATM0 not acknowledged ('%.16s'): the chip may store "
                      "a protocol change until the first switch", resp);
    }

    bare_probe("ATI\r", resp, sizeof(resp), 1500); /* identity for the log */

    esp_err_t err = obd_uart_rx_task_start();

    if (err != ESP_OK)
    {
        return err;
    }

    ESP_LOGI(TAG, "started: chip '%s' at %d baud (ready=%d, memory %s)", resp,
             *active_baud, obd_pin_ready(), mem_off ? "off" : "on");
    return ESP_OK;
}
