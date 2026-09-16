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
 * @file script_engine_doc.c
 * @brief The scripting reference the web UI shows next to the editor
 *        (2026-09-07): one entry per device binding, the globals a script
 *        can read, a Berry primer, hints for the common error messages and
 *        the engine limits. PURE (cJSON only) — host-tested.
 *
 * Adding a binding = adding its line to BIND_DOCS below AND its function
 * to the table in script_engine_bind.c; se_bindings_selfcheck() logs any
 * mismatch at boot, and the host suite checks this table's shape.
 */
#include <string.h>

#include "script_engine_doc.h"

/* ---- groups (the reference's sections, in display order) ---------------- */

static const struct
{
    const char *id;
    const char *title;
} GROUPS[] =
{
    { "basics",  "Output & timing" },
    { "obd",     "OBD-II / UDS, one request at a time" },
    { "session", "UDS conversation on a claimed bus" },
    { "dtc",     "Trouble codes" },
    { "can",     "Raw CAN" },
    { "events",  "Events & rules" },
    { "file",    "SD-card files & ECU flashing" },
};

bool se_doc_group_ok(const char *id)
{
    for (size_t i = 0; id != NULL && i < sizeof(GROUPS) / sizeof(GROUPS[0]); i++)
    {
        if (strcmp(GROUPS[i].id, id) == 0)
        {
            return true;
        }
    }

    return false;
}

/* ---- bindings ------------------------------------------------------------ */

static const se_bind_doc_t BIND_DOCS[] =
{
    { "log", "log(msg)", "basics", "nil",
      "Print a line to the run output and the device log. Numbers need str().",
      "log('rpm ' + str(rpm))" },
    { "sleep_ms", "sleep_ms(ms)", "basics", "nil",
      "Pause for up to 60 000 ms. The run budget keeps counting while asleep.",
      "sleep_ms(500)" },
    { "millis", "millis()", "basics", "int",
      "Milliseconds since the WiCAN booted.",
      "var t0 = millis()" },

    { "uds", "uds(tx, rx, hexreq)", "obd", "response hex string, or nil",
      "One request to an ECU over ISO-TP with 11-bit CAN ids: OBD-II modes ('01 0C') "
      "and UDS services ('22 F1 90') alike. Sets uds_ok and uds_nrc; a negative "
      "response comes back as its hex ('7F 22 31').",
      "var r = uds(0x7E0, 0x7E8, '01 0C')" },
    { "uds_ext", "uds_ext(tx, rx, hexreq)", "obd", "response hex string, or nil",
      "Same as uds() with 29-bit CAN ids.",
      "var r = uds_ext(0x18DA10F1, 0x18DAF110, '22 F1 90')" },
    { "uds_nrc_str", "uds_nrc_str(nrc)", "obd", "name string ('' if unknown)",
      "The ISO 14229 name of a negative response code, e.g. 0x31 -> requestOutOfRange.",
      "log(uds_nrc_str(uds_nrc))" },

    { "obd_claim", "obd_claim(tx, rx[, ext])", "session", "1 when claimed, 0 when not",
      "Hold the bus for a multi-step conversation with one ECU; the WiCAN sends "
      "tester-present so the session stays open between steps. Released by "
      "obd_release() or when the script ends.",
      "obd_claim(0x7E0, 0x7E8)" },
    { "obd_release", "obd_release()", "session", "nil",
      "End the claimed conversation (automatic when the script ends).",
      "obd_release()" },
    { "obd_request", "obd_request(hexreq[, timeout_ms])", "session", "response hex string, or nil",
      "Send a UDS request on the claimed connection and wait for the final answer "
      "(0x78 responsePending is handled). Sets uds_ok, uds_nrc and uds_pending.",
      "var r = obd_request('22 F1 90')" },
    { "obd_isotp_tx", "obd_isotp_tx(hexbytes[, timeout_ms])", "session", "1 when sent",
      "Send a raw ISO-TP payload on the claimed connection, no UDS handling.",
      "obd_isotp_tx('3E 00')" },
    { "obd_isotp_rx", "obd_isotp_rx([timeout_ms])", "session", "payload hex, or nil",
      "Receive one raw ISO-TP payload on the claimed connection (default 500 ms).",
      "var p = obd_isotp_rx(1000)" },

    { "dtc_scan", "dtc_scan()", "dtc", "report JSON string, or nil",
      "Run a trouble-code scan and wait for it (Trouble codes must be enabled). "
      "Parse the string with json.load(): valid, mil, stored, pending, permanent, freeze.",
      "var rep = json.load(dtc_scan())" },
    { "dtc_clear", "dtc_clear([codes[, mode]])", "dtc", "true when cleared",
      "Clear trouble codes (needs Allow clearing). mode: 'always', 'if_any' or "
      "'if_only'; codes: a comma list for a per-code UDS clear, '' for all.",
      "dtc_clear('', 'if_any')" },
    { "dtc_desc", "dtc_desc(code)", "dtc", "description string, or nil",
      "Look a code up in the uploaded code databases.",
      "log(str(dtc_desc('P0420')))" },

    { "can_tx", "can_tx(id, ext, hexbytes)", "can", "true when sent",
      "Put one raw CAN frame on the WiCAN's own CAN controller (up to 8 data bytes; "
      "ext = 1 for a 29-bit id). Needs the CAN bus setting on; false when it is off or "
      "nobody acknowledged. No raw receive: answers come back through uds() or obd_isotp_rx().",
      "can_tx(0x7DF, 0, '02 01 0C')" },

    { "emit", "emit(source, name, key, value)", "events", "true when queued",
      "Publish an event for the rules engine. The convention is emit('script', "
      "'done', 'value', text): rules select on script.done and read ${value}.",
      "emit('script', 'done', 'value', 'vin=' + vin)" },

    { "obd_file_size", "obd_file_size(path)", "file", "byte count, or nil",
      "Size of a file on the SD card ('/sd/...' paths only).",
      "var n = obd_file_size('/sd/fw/ecu.bin')" },
    { "obd_file_read", "obd_file_read(path, offset, len)", "file", "hex string, or nil",
      "Read up to 512 bytes of an SD-card file as hex.",
      "var h = obd_file_read('/sd/fw/ecu.bin', 0, 16)" },
    { "obd_transfer_file", "obd_transfer_file(path, offset, size, block_len[, bsc])", "file",
      "bytes sent, or nil",
      "Stream an SD-card file to the ECU as TransferData (0x36) blocks and set "
      "uds_xfer_crc / uds_xfer_blocks. Needs Allow ECU flashing.",
      "var sent = obd_transfer_file('/sd/fw/ecu.bin', 0, n, 128)" },
};

size_t se_bind_doc_count(void)
{
    return sizeof(BIND_DOCS) / sizeof(BIND_DOCS[0]);
}

const se_bind_doc_t *se_bind_doc(size_t i)
{
    return (i < se_bind_doc_count()) ? &BIND_DOCS[i] : NULL;
}

const se_bind_doc_t *se_bind_doc_find(const char *name)
{
    for (size_t i = 0; name != NULL && i < se_bind_doc_count(); i++)
    {
        if (strcmp(BIND_DOCS[i].name, name) == 0)
        {
            return &BIND_DOCS[i];
        }
    }

    return NULL;
}

/* ---- globals a script can read ------------------------------------------- */

static const struct
{
    const char *name;
    const char *doc;
} GLOBALS[] =
{
    { "uds_ok", "1 when the last uds() / uds_ext() / obd_request() got an answer "
                "(positive or negative), 0 when nothing answered." },
    { "uds_nrc", "The negative response code of that answer, or -1 when it was positive." },
    { "uds_pending", "How many 0x78 responsePending replies the last obd_request() waited through." },
    { "uds_xfer_crc", "CRC-32 of the data the last obd_transfer_file() sent." },
    { "uds_xfer_blocks", "Blocks the last obd_transfer_file() sent." },
    { "evt_source", "Source of the event that triggered this run ('' when run by hand)." },
    { "evt_name", "Name of that event, e.g. 'bit' for status.bit." },
    { "evt_<field>", "One global per field of the trigger event: evt_bit and evt_set for "
                     "status.bit, evt_param / evt_value / evt_unit for autopid.param, "
                     "evt_edge / evt_volts for battery.threshold, evt_timer for timer.tick." },
};

/* ---- a Berry primer (title, code, note) ---------------------------------- */

static const struct
{
    const char *title;
    const char *code;
    const char *note;
} PRIMER[] =
{
    { "Comments", "# to the end of the line\n#- a block\n   comment -#", "" },
    { "Variables", "var rpm = 0\nvar name = 'engine'\nrpm += 1", "Declare with var; no types." },
    { "Text", "'a' + 'b'\nstr(42)\nformat('%02X %d %.1f', 255, 7, 3.14)\nsize('abc')",
      "Only strings join with +; wrap numbers in str()." },
    { "Numbers", "0x7E0  3.5  -1\nint('0x1F')  real('2.5')\n7 / 2  ->  3   7.0 / 2  ->  3.5\n10 % 3   1 << 3   x & 0xFF",
      "Two integers divide to an integer." },
    { "Conditions", "if rpm > 3000\n  log('high')\nelif rpm > 0\n  log('running')\nelse\n  log('off')\nend\nok = a && b || !c",
      "Every block ends with end; nil is false." },
    { "Loops", "for i : 0..4  log(str(i))  end\nfor v : [1, 2, 3]  ...  end\nwhile millis() < t1  ...  end\nbreak   continue",
      "0..4 is inclusive." },
    { "Functions", "def add(a, b)\n  return a + b\nend\nvar twice = def (x) return x * 2 end", "" },
    { "Lists & maps", "var l = [1, 2]\nl.push(3)  l[0]  size(l)\nvar m = {'rpm': 800}\nm['speed'] = 0  m.find('x')  m.keys()",
      "find() returns nil when the key is missing; [] raises." },
    { "Bytes & hex", "import string\nvar parts = string.split('62 F1 90 31', ' ')\nint('0x' + parts[3])\nvar b = bytes('3157')  b[0]  b.tohex()  b.asstring()",
      "Responses are hex bytes separated by spaces." },
    { "Modules", "import string   # split, find, replace, startswith, toupper, format\nimport json     # json.load(text) -> map/list, json.dump(value)\nimport math     # floor, ceil, round, abs, min, max, pow, sqrt",
      "" },
    { "Errors", "try\n  risky()\nexcept .. as e, m\n  log(e + ': ' + m)\nend\nraise 'my_error', 'details'", "" },
};

/* ---- hints for the common error messages ---------------------------------- */

static const struct
{
    const char *match;   /* substring of the run output */
    const char *hint;
} ERRORS[] =
{
    { "obd_claim() first",
      "Call obd_claim(tx, rx) before obd_request(), obd_isotp_tx() or obd_isotp_rx()." },
    { "already claimed",
      "obd_claim() with other ids while a conversation is open: call obd_release() first." },
    { "reflash blocked",
      "Reprogramming services (0x34-0x37, obd_transfer_file) need Allow ECU flashing. "
      "Leave it off unless you are flashing an ECU." },
    { "exceeded max_runtime_ms",
      "The script ran past its budget: shorten loops and sleeps, or raise Max script "
      "runtime in the settings below." },
    { "stopped by request", "Stopped with the Stop button or the console." },
    { "syntax_error",
      "Every if / for / while / def / try block ends with end, strings are quoted, "
      "calls have parentheses. The line number is in the message." },
    { "bad hex", "Requests are hex bytes like '22 F1 90' (spaces optional, at most 8 for can_tx)." },
    { "attribute_error",
      "A name or method does not exist: check the spelling, and import string / json / "
      "math before using their functions." },
    { "type_error",
      "A value has the wrong type: numbers need str() before + with text, and text "
      "needs int() or real() before arithmetic." },
};

/* ---- JSON ---------------------------------------------------------------- */

cJSON *se_doc_reference(const se_doc_ctx_t *ctx)
{
    cJSON *o = cJSON_CreateObject();

    if (o == NULL)
    {
        return NULL;
    }

    cJSON_AddStringToObject(o, "language",
                            (ctx && ctx->language) ? ctx->language : "Berry");
    cJSON_AddBoolToObject(o, "enabled", ctx && ctx->enabled);
    cJSON_AddBoolToObject(o, "allow_reflash", ctx && ctx->allow_reflash);
    cJSON_AddBoolToObject(o, "exclusive", ctx && ctx->exclusive);

    cJSON *lim = cJSON_AddObjectToObject(o, "limits");

    cJSON_AddNumberToObject(lim, "src_max", SE_SRC_MAX);
    cJSON_AddNumberToObject(lim, "file_max", SE_SCRIPT_FILE_MAX);
    cJSON_AddNumberToObject(lim, "out_max", SE_OUT_MAX);
    cJSON_AddNumberToObject(lim, "sleep_max_ms", SE_SLEEP_MAX_MS);
    cJSON_AddNumberToObject(lim, "name_max", SE_NAME_MAX);
    cJSON_AddNumberToObject(lim, "resp_max_bytes", SE_RESP_MAX_BYTES);
    cJSON_AddNumberToObject(lim, "max_runtime_ms", ctx ? ctx->max_runtime_ms : 0);

    cJSON *groups = cJSON_AddArrayToObject(o, "groups");

    for (size_t i = 0; i < sizeof(GROUPS) / sizeof(GROUPS[0]); i++)
    {
        cJSON *g = cJSON_CreateObject();

        cJSON_AddStringToObject(g, "id", GROUPS[i].id);
        cJSON_AddStringToObject(g, "title", GROUPS[i].title);
        cJSON_AddItemToArray(groups, g);
    }

    cJSON *binds = cJSON_AddArrayToObject(o, "bindings");

    for (size_t i = 0; i < se_bind_doc_count(); i++)
    {
        const se_bind_doc_t *d = &BIND_DOCS[i];
        cJSON *b = cJSON_CreateObject();

        cJSON_AddStringToObject(b, "name", d->name);
        cJSON_AddStringToObject(b, "sig", d->sig);
        cJSON_AddStringToObject(b, "group", d->group);
        cJSON_AddStringToObject(b, "ret", d->ret);
        cJSON_AddStringToObject(b, "doc", d->doc);
        cJSON_AddStringToObject(b, "ex", d->ex);
        cJSON_AddItemToArray(binds, b);
    }

    cJSON *globals = cJSON_AddArrayToObject(o, "globals");

    for (size_t i = 0; i < sizeof(GLOBALS) / sizeof(GLOBALS[0]); i++)
    {
        cJSON *g = cJSON_CreateObject();

        cJSON_AddStringToObject(g, "name", GLOBALS[i].name);
        cJSON_AddStringToObject(g, "doc", GLOBALS[i].doc);
        cJSON_AddItemToArray(globals, g);
    }

    cJSON *primer = cJSON_AddArrayToObject(o, "primer");

    for (size_t i = 0; i < sizeof(PRIMER) / sizeof(PRIMER[0]); i++)
    {
        cJSON *p = cJSON_CreateObject();

        cJSON_AddStringToObject(p, "title", PRIMER[i].title);
        cJSON_AddStringToObject(p, "code", PRIMER[i].code);
        cJSON_AddStringToObject(p, "note", PRIMER[i].note);
        cJSON_AddItemToArray(primer, p);
    }

    cJSON *errors = cJSON_AddArrayToObject(o, "errors");

    for (size_t i = 0; i < sizeof(ERRORS) / sizeof(ERRORS[0]); i++)
    {
        cJSON *e = cJSON_CreateObject();

        cJSON_AddStringToObject(e, "match", ERRORS[i].match);
        cJSON_AddStringToObject(e, "hint", ERRORS[i].hint);
        cJSON_AddItemToArray(errors, e);
    }

    /* how a rule runs a script, and what it can chain on */
    cJSON *rules = cJSON_AddObjectToObject(o, "rules");

    cJSON_AddStringToObject(rules, "action", "script.run");
    cJSON_AddStringToObject(rules, "with", "{\"name\": \"<script>\"}");
    cJSON_AddStringToObject(rules, "event", "script.done");
    return o;
}

cJSON *se_doc_examples(void)
{
    cJSON *o = cJSON_CreateObject();

    if (o == NULL)
    {
        return NULL;
    }

    cJSON *arr = cJSON_AddArrayToObject(o, "examples");

    for (size_t i = 0; i < se_example_count(); i++)
    {
        const se_example_t *x = se_example_get(i);
        cJSON *e = cJSON_CreateObject();

        cJSON_AddStringToObject(e, "id", x->id);
        cJSON_AddStringToObject(e, "title", x->title);
        cJSON_AddStringToObject(e, "desc", x->desc);
        cJSON_AddStringToObject(e, "needs", x->needs);
        cJSON_AddNumberToObject(e, "level", x->level);
        cJSON_AddNumberToObject(e, "size", (double)strlen(x->src));
        cJSON_AddItemToArray(arr, e);
    }

    return o;
}
