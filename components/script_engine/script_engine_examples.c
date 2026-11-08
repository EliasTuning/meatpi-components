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
 * @file script_engine_examples.c
 * @brief The built-in example scripts (2026-09-07): the Scripts page's
 *        gallery. Each is a complete, commented Berry program a user can
 *        open, run and edit; they ship in the firmware (not on the file
 *        system) so they follow the bindings and never get lost. PURE:
 *        host-tested for shape (valid ids, under the inline size cap).
 *
 * Every example was run on the bench against the ECU simulator; keep them
 * runnable when bindings change.
 */
#include <string.h>

#include "script_engine_doc.h"

static const char HELLO[] =
    "# Hello, WiCAN - output, variables, loops and timing.\n"
    "# Safe to run as it is: nothing talks to the vehicle.\n"
    "log('Hello from WiCAN')\n"
    "\n"
    "var boot = millis()                 # milliseconds since the WiCAN booted\n"
    "log('up for ' + str(boot / 1000) + ' s')\n"
    "\n"
    "# text joins with +, numbers need str(); format() works like printf\n"
    "var total = 0\n"
    "for i : 1..5                        # 1..5 is inclusive\n"
    "  total += i\n"
    "  log(format('step %d, running total %d', i, total))\n"
    "  sleep_ms(200)                     # pause 0.2 s (the run budget keeps counting)\n"
    "end\n"
    "\n"
    "# a function of your own\n"
    "def hex_byte(n)\n"
    "  return format('%02X', n)\n"
    "end\n"
    "log('0x7E0 as two bytes: ' + hex_byte(0x7E0 >> 8) + ' ' + hex_byte(0x7E0 & 0xFF))\n"
    "\n"
    "# lists and maps\n"
    "var pids = ['01 0C', '01 0D', '01 05']\n"
    "var names = {'01 0C': 'engine speed', '01 0D': 'vehicle speed', '01 05': 'coolant'}\n"
    "for p : pids\n"
    "  log(p + ' is ' + names[p])\n"
    "end\n"
    "log('done in ' + str(millis() - boot) + ' ms')\n";

static const char OBD_LIVE[] =
    "# Live OBD-II values: ask the engine ECU for RPM, speed and coolant\n"
    "# temperature five times, one second apart.\n"
    "# uds(tx, rx, request) sends one request and returns the answer as hex\n"
    "# bytes ('41 0C 0C 80'), or nil when nothing answered.\n"
    "import string\n"
    "\n"
    "var TX = 0x7DF      # functional address: every OBD-II ECU listens\n"
    "var RX = 0x7E8      # the engine ECU answers here\n"
    "\n"
    "# byte i of a hex answer as a number (-1 when the answer is shorter)\n"
    "def byte_at(resp, i)\n"
    "  var parts = string.split(resp, ' ')\n"
    "  if i >= size(parts) return -1 end\n"
    "  return int('0x' + parts[i])\n"
    "end\n"
    "\n"
    "# one mode-01 PID, decoded by the function you pass in; nil when unanswered\n"
    "def pid(code, decode)\n"
    "  var r = uds(TX, RX, '01 ' + code)\n"
    "  if r == nil\n"
    "    log('PID ' + code + ': no answer (uds_ok=' + str(uds_ok) + ')')\n"
    "    return nil\n"
    "  end\n"
    "  return decode(r)\n"
    "end\n"
    "\n"
    "for n : 1..5\n"
    "  var rpm = pid('0C', def (r) return (byte_at(r, 2) * 256 + byte_at(r, 3)) / 4 end)\n"
    "  var speed = pid('0D', def (r) return byte_at(r, 2) end)\n"
    "  var coolant = pid('05', def (r) return byte_at(r, 2) - 40 end)\n"
    "  log(format('#%d  rpm=%s  speed=%s km/h  coolant=%s C', n, str(rpm), str(speed), str(coolant)))\n"
    "  sleep_ms(1000)\n"
    "end\n";

static const char VIN[] =
    "# Read the vehicle identification number (VIN).\n"
    "# OBD-II mode 09 PID 02 answers '49 02 01' followed by the 17 ASCII\n"
    "# characters; a UDS ECU also serves it as data identifier F190.\n"
    "import string\n"
    "\n"
    "# the bytes of a hex answer from position `from` as text\n"
    "def text_from(resp, from)\n"
    "  var parts = string.split(resp, ' ')\n"
    "  var last = size(parts) - 1\n"
    "  var b = bytes()\n"
    "  for i : from..last\n"
    "    b.add(int('0x' + parts[i]), 1)      # append one byte\n"
    "  end\n"
    "  return b.asstring()\n"
    "end\n"
    "\n"
    "var r = uds(0x7DF, 0x7E8, '09 02')\n"
    "if r != nil && string.startswith(r, '49 02')\n"
    "  log('VIN (mode 09): ' + text_from(r, 3))\n"
    "else\n"
    "  log('mode 09 gave nothing (uds_ok=' + str(uds_ok) + ', nrc=' + str(uds_nrc) + '), trying UDS DID F190')\n"
    "  r = uds(0x7E0, 0x7E8, '22 F1 90')\n"
    "  if r != nil && string.startswith(r, '62 F1 90')\n"
    "    log('VIN (DID F190): ' + text_from(r, 3))\n"
    "  elif uds_nrc >= 0\n"
    "    log('the ECU refused: ' + uds_nrc_str(uds_nrc))\n"
    "  else\n"
    "    log('no answer')\n"
    "  end\n"
    "end\n";

static const char UDS_SESSION[] =
    "# A UDS conversation: claim the bus, open an extended session, read a\n"
    "# few data identifiers, then release. While the bus is claimed the WiCAN\n"
    "# sends tester-present for you, so the ECU keeps the session open.\n"
    "\n"
    "# send one request on the claimed connection and report the outcome\n"
    "def show(label, req)\n"
    "  var r = obd_request(req)\n"
    "  if r != nil && uds_nrc < 0\n"
    "    log(label + ': ' + r)\n"
    "  elif uds_nrc >= 0\n"
    "    log(label + ': refused, ' + uds_nrc_str(uds_nrc) + format(' (0x%02X)', uds_nrc))\n"
    "  else\n"
    "    log(label + ': no answer')\n"
    "  end\n"
    "  return r\n"
    "end\n"
    "\n"
    "if obd_claim(0x7E0, 0x7E8) == 0\n"
    "  log('could not claim the bus - is another script or tool using it?')\n"
    "else\n"
    "  show('extended session', '10 03')\n"
    "  show('VIN, DID F190', '22 F1 90')\n"
    "  show('part number, DID F187', '22 F1 87')\n"
    "  show('software version, DID F195', '22 F1 95')\n"
    "  show('a DID the ECU does not have', '22 01 62')    # expect a negative response\n"
    "  sleep_ms(3000)          # longer than most S3 timeouts: tester-present keeps the session\n"
    "  show('still in session', '22 F1 90')\n"
    "  obd_release()\n"
    "end\n";

static const char DTC_REPORT[] =
    "# Trouble codes: run a scan, list the codes with their descriptions from\n"
    "# the uploaded code databases, and show the freeze frame if one was\n"
    "# captured. Needs Trouble codes enabled (Trouble Codes -> Settings).\n"
    "# Clearing is left commented out: it also needs Allow clearing.\n"
    "import json\n"
    "\n"
    "var raw = dtc_scan()      # waits for the scan (a few seconds)\n"
    "if raw == nil\n"
    "  log('scan not available: enable Trouble codes, and make sure nothing else is using the OBD chip')\n"
    "  return\n"
    "end\n"
    "\n"
    "var rep = json.load(raw)\n"
    "if rep != nil && rep.find('report') != nil rep = rep['report'] end\n"
    "if rep == nil || !rep['valid']\n"
    "  log('scan failed: ' + str(rep != nil ? rep.find('error') : raw))\n"
    "  return\n"
    "end\n"
    "\n"
    "log(format('%d ECU(s) answered over %s, MIL %s', rep['ecus'], str(rep['protocol']), rep['mil'] ? 'ON' : 'off'))\n"
    "for kind : ['stored', 'pending', 'permanent']\n"
    "  var codes = rep.find(kind)\n"
    "  if codes == nil || size(codes) == 0\n"
    "    log(kind + ': none')\n"
    "    continue\n"
    "  end\n"
    "  for code : codes\n"
    "    var desc = dtc_desc(code)\n"
    "    log(kind + ': ' + code + (desc != nil ? ' - ' + desc : ''))\n"
    "  end\n"
    "end\n"
    "\n"
    "var fz = rep.find('freeze')\n"
    "if fz != nil\n"
    "  log('freeze frame for ' + str(fz['dtc']) + ' from ECU ' + str(fz['ecu']) + ':')\n"
    "  for name : fz['params'].keys()\n"
    "    var p = fz['params'][name]\n"
    "    log('  ' + name + ' = ' + str(p['value']) + ' ' + str(p['unit']))\n"
    "  end\n"
    "end\n"
    "\n"
    "# dtc_clear('', 'if_any')     # uncomment to clear every code that is present\n";

static const char ON_EVENT[] =
    "# A script that a rule runs. Rules & Events -> Add Rule: pick the\n"
    "# trigger (When), set Do = script.run and With = {\"name\": \"on_event\"}.\n"
    "# The trigger arrives in the evt_* globals, and whatever the script\n"
    "# emits can drive further rules (script.done).\n"
    "import global\n"
    "\n"
    "if evt_source == ''\n"
    "  log('run by hand: no trigger event. From a rule, evt_source, evt_name and one evt_<field> per event field are set.')\n"
    "else\n"
    "  log('triggered by ' + evt_source + '.' + evt_name)\n"
    "end\n"
    "\n"
    "# the fields you get for common triggers:\n"
    "#   status.bit         -> evt_bit ('sta_connected', 'mqtt_connected', ...), evt_set (1/0)\n"
    "#   autopid.param      -> evt_param, evt_value, evt_unit, evt_group\n"
    "#   battery.threshold  -> evt_edge ('below' / 'above'), evt_volts\n"
    "#   timer.tick         -> evt_timer\n"
    "#   imu.motion         -> evt_state ('active' / 'stationary')\n"
    "# a field exists only when the trigger carries it: test before reading\n"
    "var result = 'ok at ' + str(millis()) + ' ms'\n"
    "if global.contains('evt_param')\n"
    "  result = global.member('evt_param') + '=' + str(global.member('evt_value'))\n"
    "elif global.contains('evt_bit')\n"
    "  result = global.member('evt_bit') + (global.member('evt_set') == 1 ? ' set' : ' cleared')\n"
    "end\n"
    "\n"
    "# publish an event of our own; a rule on script.done can read ${value}\n"
    "emit('script', 'done', 'value', result)\n"
    "log('emitted script.done with value=' + result)\n";

static const char CAN_FRAME[] =
    "# Send raw CAN frames. can_tx(id, ext, hexbytes) puts one frame on the\n"
    "# WiCAN's own CAN controller (up to 8 data bytes; ext = 1 for a 29-bit\n"
    "# id). It needs the CAN bus turned on (Settings -> CAN bus) and answers\n"
    "# false when the bus is off or nobody acknowledged the frame. A single\n"
    "# frame comes back with can_rx(id, timeout_ms). UDS answers still come\n"
    "# through uds(), and a claimed conversation can use obd_isotp_rx().\n"
    "var ok = can_tx(0x7DF, 0, '02 01 0C 00 00 00 00 00')   # an OBD-II RPM request as a raw frame\n"
    "log('11-bit frame sent: ' + str(ok) + (ok ? '' : '  (is the CAN bus on?)'))\n"
    "\n"
    "# the same request through the ISO-TP layer, which also collects the answer\n"
    "var r = uds(0x7DF, 0x7E8, '01 0C')\n"
    "log('answer: ' + str(r))\n"
    "\n"
    "# a 29-bit id with three data bytes\n"
    "ok = can_tx(0x18DA10F1, 1, '02 3E 00')\n"
    "log('29-bit frame sent: ' + str(ok))\n"
    "\n"
    "var f = can_rx(0x7E8, 1000)    # wait 1 s for id 0x7E8\n"
    "log('frame: ' + str(f))\n";

static const char GPIO_PULSE[] =
    "# Park the CAN controller, pulse its TX pin, then the script end\n"
    "# starts the bus again. Times are milliseconds (1.6 and 0.4). The\n"
    "# whole pulse must stay within 100 ms. CAN_RX works the same way.\n"
    "can_stop()\n"
    "digitalPWM(CAN_TX, 1.6, 0.4, 10)\n"
    "digitalWrite(CAN_TX, LOW)\n"
    "can_start()\n";

static const se_example_t EXAMPLES[] =
{
    { "hello", "Hello, WiCAN",
      "Output, variables, loops and timing. Runs without a vehicle.",
      "", 1, HELLO },
    { "obd_live", "Live OBD-II values",
      "Poll engine speed, vehicle speed and coolant temperature and decode the answers.",
      "vehicle", 1, OBD_LIVE },
    { "vin", "Read the VIN",
      "Mode 09 first, UDS data identifier F190 as the fallback, decoded to text.",
      "vehicle", 1, VIN },
    { "uds_session", "A UDS conversation",
      "Claim the bus, open an extended session, read data identifiers, handle a refusal.",
      "vehicle", 2, UDS_SESSION },
    { "dtc_report", "Trouble codes report",
      "Scan, list every code with its description, show the freeze frame.",
      "dtc", 2, DTC_REPORT },
    { "on_event", "Run from a rule",
      "Read the trigger event's fields and emit an event other rules can use.",
      "", 2, ON_EVENT },
    { "can_frame", "Raw CAN frames",
      "Send 11-bit and 29-bit frames and wait for one answer with can_rx().",
      "can", 2, CAN_FRAME },
    { "gpio_pulse", "Pulse a CAN pin",
      "Stop the bus, pulse CAN_TX for 1.6 ms on and 0.4 ms off, start the bus.",
      "can", 2, GPIO_PULSE },
};

size_t se_example_count(void)
{
    return sizeof(EXAMPLES) / sizeof(EXAMPLES[0]);
}

const se_example_t *se_example_get(size_t i)
{
    return (i < se_example_count()) ? &EXAMPLES[i] : NULL;
}

const se_example_t *se_example_find(const char *id)
{
    for (size_t i = 0; id != NULL && i < se_example_count(); i++)
    {
        if (strcmp(EXAMPLES[i].id, id) == 0)
        {
            return &EXAMPLES[i];
        }
    }

    return NULL;
}
