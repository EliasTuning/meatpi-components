# autopid

Scheduled OBD PID polling: requests PIDs through `obd_chip`, parses the
ELM response into payload bytes, evaluates every parameter expression
(`expression_parser`) against that ONE response, and keeps the latest
values in a PSRAM cache that HTTP/CLI/(future) event_manager read.

Design + phased goal checklist: [TASK_autopid.md](TASK_autopid.md).
DTC check/report/clear (sub-module, bench-PASSED 2026-07-08, disabled by
default): [TASK_dtc.md](TASK_dtc.md).
Decisions log there is authoritative; the big ones:

- **The PID is the scheduling unit** (legacy fix #1): a PID with ten
  parameters is requested ONCE per period and that single response
  updates all ten. The scheduler (`autopid_sched.c`) has no parameter
  concept, so the legacy behavior is unrepresentable.
- **No transports inside**: autopid produces values/events; delivery
  (MQTT/HTTP/HA/ABRP) belongs to event_manager and future components.
- **An external ELM app owns the chip while it is active** (2026-09-08,
  legacy parity): the poller stays off the chip until a bridged client
  (Car Scanner over TCP:35000, a BLE/USB/WS terminal) has been silent
  for `AP_CLIENT_YIELD_MS` = 10 s - the old firmware's
  `DEV_AUTOPID_ELM327_APP_BIT` + 10 s timer. Reproduced on the bench
  with the two interleaved (the field report "RPM dial choppy, values
  jump"): the chip fans EVERY line out to every subscriber, so the app
  read autopid's `0105 / 41 05 82` as the answer to its own request, and
  the app's command landing mid-poll made the chip print `STOPPED` -
  80 of 200 app requests came back wrong, 0 of 200 with autopid off.
  Source of truth is `obd_chip_client_idle_ms()` via the transport's
  `client_idle_ms` hook (the bridge glue touches it on every app write);
  the pure predicate `ap_sched_client_hold()` is host-tested. Visible
  as `stats.paused_client` in `/api/autopid`, `(yielding to an OBD app)`;
  likewise `stats.paused_diag` / `(paused for a diagnostic tool)` while the
  UDS Tool or J2534 holds the bus through their Exclusive bus option
  (obd_gate's diagnostics hold, 2026-09-16: polls AND DTC scans pause, the
  chip baseline is restored on resume)
  in the `autopid` console command and INFO lines `paused: external OBD
  client active` / `resumed: external OBD client idle`. On resume the
  poller first re-sends the protocol prelude (`ap_std_prelude()`:
  `ATS1;ATH0;ATST96;ATTP<p>;ATSH..;ATCRA`) and replays every type/PID
  init (`ap_runner_restore_baseline()`) - the app leaves the chip in ITS
  state, and Car Scanner's `ATS0` alone made every resumed poll fail to
  parse (bench: 110 failed polls, 0 ok, until the reboot). Side effects
  while an app drives: no autopid values/events, the periodic DTC check
  waits, and `autopid_ecu_online()` (HA `ecu_status`) goes false 30 s
  after the last poll - the same as legacy; user-started jobs (std scan,
  DTC scan, test-a-PID) are not gated. Not a setting on purpose (the
  chip is a single-master serial device: sharing it is never right).
- **Tables in a file, knobs in settings**: `/data/autopid/config.json`
  (groups/pids/filters/parameters, validated + applied LIVE via
  `PUT /api/autopid/config`) vs the `autopid` settings component
  (enable, per-type enables, init strings, pause voltage, min event
  interval — reboot-to-apply like every component).

## API (`include/autopid.h`)

- `autopid_init()` / `_start()` / `_stop()` — settings + log descriptor; the
  poller task (PSRAM stack, notify-driven); stop latches the idle state.
- `autopid_group_set(group, enabled, period_override_ms)` — the EPHEMERAL
  runtime group switch/rate override (`POST /api/autopid/group`, the
  `autopid.group` rule action); `< 0` keeps the period, 0 = max rate.
- `autopid_group_restore(group)` (2026-09-17) — back to the configured
  enable + rate: the undo of an `autopid.group` while-rule.
- `autopid_snapshot(&json)` / `autopid_get_value(param, &v, &unit)` /
  `autopid_set_value_sink(sink)` / `autopid_publish_external(...)` — the
  live value cache in and out.
- `autopid_config_json_dup(&s)` / `autopid_reload_config()` — the PID table
  file (`PUT /api/autopid/config` applies LIVE).
- `autopid_stats(&st)` / `autopid_ecu_online()` — poll counters, pause
  reasons, ECU presence.
- `autopid_dtc_scan_start()` / `_scanning()` / `_report(&json)` /
  `_clear(codes, mode, ...)` / `_desc(code, ...)` — the DTC engine.
- `autopid_register_http()` / `autopid_register_cli()` — own routes/commands.

## Files

| File | Role |
|---|---|
| `autopid.c` | settings fields, poller task (PSRAM stack, notify-driven), battery-pause watch, core query surface |
| `autopid_runner.c` | the chip-facing poll: type/per-PID init transitions, ATCRA rxheader, request → parse → guard → eval → cache |
| `autopid_filter.c` | the ATMA filter window: MONITOR claim, header/CRA choreography, frame capture (`ap_filter_frame`, pure), retried stop |
| `autopid_std.c` | standard PIDs: vendored SAE table (obd2_standard_pids.h), PURE bit_start→expression mapping + bitmap parser, the async support scan (`/data/autopid/std_scan.json`) |
| `autopid_sched.c` | PURE scheduler: due times, group inheritance/override (`ap_sched_group_set`/`_restore`, 2026-09-17), fail backoff (×4 after 3), period-0 high-fidelity round-robin, stagger |
| `autopid_group.c` | runtime group control: `autopid_group_set`/`_restore` (name lookup under the core lock + poller wake-up over the pure scheduler calls) and the group state JSON (split out of autopid.c 2026-09-17) |
| `autopid_resp.c` | PURE ELM text → payload bytes (headers on/off, ISO-TP single/multi, lowest-responder rule, noise/error lines) + the cross-talk guard (`ap_payload_matches_cmd` — a second chip master's response can't be cached as ours) |
| `autopid_config.c` | PURE JSON parse/validate + target file load/save (atomic) |
| `autopid_cache.c` | latest-value slots (PSRAM, mutex), legacy snapshot + detail JSON, plus the **external-value table** (12 name-keyed injected samples: GPS from the ESPNetLink dongle) merged into every snapshot/get/detail |
| `autopid_dtc_codec.c` | PURE DTC codec: 2-byte code ⇄ "P0420", 43/47/4A payload parse, 41-01 MIL/count, clear-condition matrix (`always/if_any/if_only` — mode 04 is all-or-nothing, so "clear specific" = a condition on the present set) |
| `autopid_dtc.c` | DTC engine: async scan job (01-01 gate + 03/07/0A), conditional mode-04 clear (fresh 03 → condition → 04 → confirming 03), periodic due-check, RAM report + diff (events fire per NEW code) |
| `autopid_dtc_db_codec.c` | PURE DTC-database importer: sniff + parse CSV/TSV/semicolon/JSON-map/JSON-array/plain text → canonical sorted form ([TASK_dtc_db.md](TASK_dtc_db.md)) |
| `autopid_dtc_db.c` | database store (/data/autopid/dtc_db) + PSRAM cache w/ binary search: upload/list/delete/search/lookup, report `desc` enrichment — lookups never touch flash |
| `autopid_dbc_codec.c` | PURE DBC codec: BO_/SG_ parser + the signal→expression compiler (Intel/Motorola, signed via multiply-subtract, Motorola-aligned spans; host cross-checked vs a reference decoder) — [TASK_dbc.md](TASK_dbc.md) |
| `autopid_dbc.c` | DBC store (/data/autopid/dbc) + PSRAM cache: upload/list/delete, signal browse/search, and the add-to-filters merge (dry-run validate → atomic save → LIVE reload) |
| `autopid_http.c` | `/api/autopid[/data\|/config\|/dtc*]` (see `components/HTTP_API.md` §6e4/§6e4b) |
| `autopid_cli.c` | `autopid [-l] [-d] [--dtc-scan]` console command (§6b self-registration) |

## Settings (`/api/settings/autopid`, reboot-to-apply)

`enabled` (default **false**), `std_enabled`/`custom_enabled`/
`specific_enabled`, `std_init`/`custom_init`/`specific_init`
(';'-separated AT prelude per type), `std_protocol` (enum `0` chip-auto | `6` CAN 11-bit 500k | `7` CAN 29-bit 500k | `8` CAN 11-bit 250k | `9` CAN 29-bit 250k, default `6`; schema v7), `vehicle`,
`pause_below_mv` (0 = no fixed threshold, else 1–14500 mV; resumes +0.3 V with 5 s hold),
`pause_follow_sleep` (default **true** — legacy `disable_pid_requests`
parity: requests pause below the sleep threshold via the battery
watch; explicit `pause_below_mv` overrides), `pause_mode`
(`all`|`requests_only`), `min_event_interval_ms`
(10–600000), `cli`; DTC (schema v4, ALL off by default): `dtc_enabled`
(**false** — master gate), `dtc_allow_clear` (**false** — second gate
for mode 04 / UDS 0x14), `dtc_scan_period_min` (0 = on-demand only),
`dtc_pending`/`dtc_permanent` (include modes 07/0A), `dtc_init`
(';'-separated chip prep per scan), `dtc_rxheader` (ATCRA filter —
targeted-ECU scans); UDS transport (v4, TASK_dtc.md §12):
`dtc_protocol` (`obd`|`uds`|`auto` — auto = OBD first, UDS fallback),
`dtc_uds_txid`/`dtc_uds_rxid` (default 7E0/7E8), `dtc_uds_ext`
(29-bit ids), `dtc_uds_mask` (0x19 status mask, default 0x08
confirmed).

DTC events/actions/pull-values + rule recipes (scheduled scan+report,
alert-on-new-code, clear-when-detected, scheduled clear) and the Berry
`dtc_scan()`/`dtc_clear()` bindings: [TASK_dtc.md](TASK_dtc.md) §7-9.
DTC works with polling `enabled=false` (scans only need the OBD chip).

**EEPROM guard (ATSP → ATTP, ATM1 → ATM0; since 2026-09-16 the chip driver's own guard, `obd_chip_guard.h`, which also REFUSES ATPP/ATSD/ATCV/STWBR: config parse rejects a PID whose `cmd`/`init` carries one)**: every user-supplied chip
command — init strings (type inits, per-PID `init`, `dtc_init`) at
send time, PID `cmd`/`init` fields at config parse, and the test-a-PID
one-shot — is sanitized (case-insensitive, whitespace-tolerant —
`ap_init_sanitize`, host-tested): `ATSP` becomes `ATTP` (same protocol
switch, RAM only) and `ATM1` (memory on — makes the chip store every
later protocol change) becomes `ATM0`. Both write the chip's EEPROM,
and PID commands/inits replay on every poll cycle / type transition,
so either would wear it out (legacy semantics). Monitor commands
(`ATMA`/`ATMR`/`ATMT`) and `ATM0` pass through untouched. Write ATSP
in configs/profiles freely; the chip only ever sees ATTP.

## Config file shape (`PUT /api/autopid/config`)

```json
{
  "groups": [{"name": "driving", "enabled": true, "period_ms": 500}],
  "pids": [{
    "name": "engine", "type": "std|custom|specific", "cmd": "010C",
    "init": "ATSH7E0", "rxheader": "7E8", "group": "driving",
    "period_ms": 0,
    "parameters": [{"name": "rpm", "expression": "[B2:B3]/4",
                    "unit": "rpm", "class": "frequency",
                    "min": 0, "max": 16384}]
  }],
  "filters": [{"frame_id": 666, "monitor_ms": 800,
               "parameters": [{"name": "soc", "expression": "B4/2"}]}]
}
```

`period_ms 0` on a PID inherits the group; group `period_ms 0` =
high-fidelity max-rate (entries round-robin as fast as the chip
answers — **~19 req/s total / 53 ms per request measured**, shared
across all period-0 entries; numbers + guidance in
[BENCHMARKS.md](BENCHMARKS.md)). Periods of 1–49 ms are accepted but
flagged (`stats.sub_floor_pids`). A group named `default` always
exists. Expressions index the
payload INCLUDING the service/PID echo (B0 = 0x41 on mode-01) — legacy
profile expressions work unchanged. **Filter expressions index the
frame DATA (B0 = first data byte, no id echo)** — also the legacy
frame-of-reference.

Table bounds (`autopid_private.h`): 32 groups, 512 PIDs, 128 filters,
2048 parameters pooled across the table and **256 parameters per PID
or filter** (`AP_PARAMS_PER`, raised from 16 on 2026-09-16: the
published Hyundai/Kia BMS DIDs decode 20–32 values from one reply and
Xpeng's cell-voltage DID 192, so every such profile was rejected on
import). A config over any bound is refused whole, naming the entry
(`220105: more than 256 parameters`); nothing is clipped silently.
The per-entry slice the poller copies per request lives in PSRAM
(~60 KB), not on the task stack.

## Filters (ATMA windows)

A scheduled filter entry opens a bounded monitor window: `ATH1` +
`ATCRA<id>`, MONITOR claim, `ATMA`, collect until a frame with the
filter's CAN id arrives (or `monitor_ms` expires — 50..60000, default
1000), SPACE stop, restore receive-all + headers-off, PID inits
replayed. ONE captured frame updates all the filter's parameters. The
window **holds the chip** — other masters' requests fail fast while it
is open, so schedule filters sparsely (period_ms seconds apart, short
windows). Chip "<DATA ERROR"-style markers after the data bytes are
skipped (legacy semantics; bench-observed with injected frames).

**Protocol/width rule (bench-characterized 2026-07-06)**: the ATCRA
hardware filter only matches ids of the ACTIVE protocol's width — a
29-bit filter needs the chip on a 29-bit protocol (ATSP7/9) and an
11-bit filter needs 11-bit (ATSP6/8). Cross-width combinations are
accepted by the chip ("OK") but the window sees NOTHING and the filter
fails cleanly into backoff — set the protocol via the type init /
`std_protocol` to match your filters. (Monitor-all WITHOUT a CRA shows
both widths regardless of protocol; it is the CRA that is
width-bound.) On 29-bit protocols the monitor prints the id as four
byte tokens (`18 DA F1 10 …`) — handled.

**Short frames**: the captured payload is the frame's REAL length
(DLC), and expressions are bounds-checked against it — an expression
past the end (`B6` on a DLC-2 frame) is skipped for that window
(never a garbage read; `null` in the API), while in-range parameters
on the same frame evaluate normally.

## Standard PIDs: scan once, store

`POST /api/autopid/std_scan` (UI button; prompt "ignition ON" first)
walks the SAE support bitmaps against the configured `std_protocol`,
maps hits to the built-in table and stores ready-made config rows —
names, units, classes and **generated expressions** (`[B2:B3]*0.25`
etc.) — at `/data/autopid/std_scan.json`. The UI copies entries from
`GET /api/autopid/std_scan/result` (or the full catalog at
`/api/autopid/std_table`) straight into the config PUT. Never
auto-rescans. Polling pauses during the scan and resumes with its
init state replayed.

## External values (GPS, and future non-PID samples)

`autopid_publish_external(name, unit, value)` injects a named sample
that is NOT a polled OBD parameter into the same live cache. It joins
`autopid_snapshot()` (→ the HA `autopid_data` push + `${autopid.data}`),
fires the value sink (→ `data_logger`), emits `autopid.param` on change
(→ event rules, `group="external"`), and resolves through
`autopid_get_value()` (→ the dashboard) — so one call reaches everywhere
polled parameters do, with no per-consumer work. The table holds 12
distinct names; a config reload does NOT clear it (external samples are
config-independent).

Today's producer: the ESPNetLink dongle's GPS, published by **main** from
`usb_acm_cli`'s GPS sink as `gps_latitude` / `gps_longitude` /
`gps_altitude` (m) / `gps_speed` (km/h) / `gps_heading` / `gps_satellites`
— only a live fix; last-known persists. autopid itself stays
USB/GPS-agnostic (the glue lives in main, like the data_logger sink).

`autopid_snapshot()` / `GET /api/autopid/data` is a flat `{name: value}`
object — the GPS keys sit right alongside the polled PIDs:

```json
{
  "VehicleSpeed": 62,
  "rpm": 1840,
  "coolant_temp": 89,
  "SOC_BMS": 74.5,
  "gps_latitude": -37.90535,
  "gps_longitude": 145.145047,
  "gps_altitude": 88.8,
  "gps_speed": 61.6,
  "gps_heading": 270.5,
  "gps_satellites": 7
}
```

The raw GPS surface `GET /api/gps` is the contract shape (speed in m/s,
HDOP-derived accuracy in m); `{"valid": false}` when there is no live
fix:

```json
{
  "valid": true, "latitude": -37.90535, "longitude": 145.145047,
  "accuracy": 6, "altitude": 88.8, "speed": 17.1, "heading": 270.5,
  "satellites": 7, "age_ms": 1200
}
```

## Vehicle profiles (client-side import)

The UI/app fetches `vehicle_profiles.json` (GitHub), the user picks a
profile, and the client PUTs the converted table to
`/api/autopid/config` (+ `vehicle`/init strings in settings). The
firmware never fetches profiles. **Two conversion rules the importer
MUST apply** (bench-verified against the published MEB profile):

1. **Byte-index shift**: published profile expressions index the
   legacy frame WITH its PCI byte (`010C` RPM = `[B3:B4]`); the v6
   payload starts at the service echo (B0 = 0x41/0x62) — shift every
   `B<n>`/`S<n>` index DOWN BY ONE (`[B2:B3]`).
2. **Cross-type inits must be self-sufficient**: a specific PID whose
   init switches protocol/headers/CRA (`ATSP7;…;ATCRA…`) leaves the
   chip there — if standard PIDs are polled too, give `std_init` a
   restoring prelude (`ATSP6;ATCRA;ATH0`) so each type transition
   re-establishes its world.
3. **Parameter names must be unique across the whole table** (the
   value cache and every API key on them; the firmware refuses the
   table with `duplicate parameter name 'X'`). Profiles are community
   data: three published Hyundai/Kia profiles label cell 158
   `HV_C_V_168` twice (2026-09-16), so the importer renames a repeat
   (`_2`, `_3` …, against the std/custom parameters already loaded)
   and tells the user instead of failing the import.

`POST /api/autopid/test` = try a profile PID before saving (same
runner path: init chain, rxheader, expression — see HTTP_API §6e4).
Since 2026-09-16 it takes `type` (prepend the std/custom/specific init
chain, so the shot IS a poll of that PID) and `expressions[]` (decode
the one reply with every parameter, answered as `values[]`), and returns
a `transcript`: one `> cmd` / `< reply` line per exchange in the order
sent (type init, PID init, ATCRA, request, ATCRA off), capped per line.
The UI's Test modal shows it, so an init that parks the chip elsewhere
or a negative response (`7F 22 31`) is visible instead of a bare NO DATA.
`tools/testbench/obd/autopid_profile_bench.py <base>` PUTs every
published profile through this conversion (→ `AUTOPID PROFILE PASS`)
and polls a 24-parameter PID on the simulator — the regression for the
16-parameter cap and the duplicate-name typo.

## Tests

`host_test/` (run: `.\test.ps1 host autopid`): scheduler regression
(one-request-per-PID-per-period, group toggle/override, type gates,
round-robin, backoff, stagger), response-parser vectors (single frame,
SEARCHING noise, headers-on lowest responder, ISO-TP both header
modes, error lines), config parse happy/invalid, the yield-to-app window
predicate. Bench verification per TASK_autopid.md Phase 1.

**ELM app responsiveness bench (2026-09-08)** -
`tools/testbench/obd/elm_app_bench.py [dut[:port]] [--dut-ip 10.42.1.194]`
(PC-run; the request loop runs on rpi001 over the hotspot, verdict
`ELM APP PASS`): a Car Scanner style `010C 1` loop of 200 requests over
TCP:35000 while autopid is configured and polling must see zero
foreign/`STOPPED`/`NO DATA` answers, p50 <= 12 ms, p95 <= 40 ms, >= 40
req/s, `paused_client` reported and `polls_ok` frozen meanwhile; the
hint-less `010C` loop must be clean too (its RTT is the chip's own
multi-ECU wait); polling resumes within 15 s of the app's last command.
See TESTING.md for the numbers of the last run, and
`tools/testbench/obd/elm_compare.py` for the before/after table with a
reference USB adapter (OBDLink) on the same bus.

**Bench matrix (2026-09-06)** —
`tools/testbench/obd/autopid_matrix_bench.py [dut[:port]] [--sim 192.168.8.1]
[--sim-restore asis|on|off]` (PC or Pi, plain HTTP, no PCAN): standard
PIDs via the support scan, custom expressions (hinted `010C1`, a
multi-parameter PID, an unsupported `0162` kept isolated), vehicle-
specific entries with per-PID init + rxheader (multi-frame UDS DIDs
`22F190`/`22F187` on 7E0/7E8, a second ECU on 7E1/7E9), all three
groups live at different periods (every value right at once, update
rates, runtime group toggle, HTTP RTT), passive filters against the
simulator's broadcast beside polling, the per-type gates across a
reboot, and a verbatim restore. Bench ECU simulator facts (the "WiCAN
ECU Simulator" box, `http://192.168.8.1`, settings component
`ecu_sim`, 500k/11-bit): RPM 800, coolant 90 °C, speed 0, load ~25 %,
`0162` → NRC, VIN `1WCAN0FW0P0000001` (multi-frame), ECU name
`WCAN-ECU-SIM` (`22F187`), mode 01 on physical 7E0/7E8 AND 7E1/7E9;
`broadcast_enabled` (reboot-to-apply) streams ONE id at a very high
rate, and the set differs between the simulator's boots (seen: 0x0C0
`0C 80 00 00 00 00 00 00` = raw RPM 800 at ~1.7 kHz; later 0x1A0 all
zeros at ~2.6 kHz plus 0x100 at ~7 Hz) — the filter leg therefore
calibrates on whatever dominant frame the chip's `ATMA` shows before it
starts. The chip tags every monitored line `<DATA ERROR` (skipped, as
documented above). Last full run (2026-09-06): 48 checks PASS + 1 WARN.
Numbers: 4 std PIDs at 500 ms ≈ 7.4 polls/s; a period-0 hinted custom
RPM ≈ 17 updates / 5 s; HTTP p50 ≈ 60 ms under load through an ssh
tunnel; std / custom / specific = 13 / 31 / 8 updates per 10 s when all
three groups run together; every value correct at once (no
cross-attribution); group toggle and the per-type gates behave.

**OPEN FINDING — filters on a flooded bus** (the WARN): with the
simulator flooding one id, filter windows fail silently and often —
about 1 window in 3 missed at 1.7 kHz (0x0C0), about 4 in 5 missed at
2.6 kHz (0x1A0) — even though the window's own id IS the flood. Every
failed window still holds the chip for `monitor_ms` plus the stop, so
standard polling beside two 3 s filters drops to 0.4–0.6 updates/s from
~1.4/s. No W/E line is logged (the stop always found the prompt; the
"no frame" path only logs at debug, which this build compiles out).
Once, a never-matching filter (0x123) captured a bogus frame (value 130
= the coolant byte 0x82) — suspect a spliced monitor line matching via
the two-byte split-id shape. The same 2.8 k lines/s stream reaches an
`/ws/obd` client almost intact, so the suspect is the filter path's
16-chunk queue (drop-oldest → spliced lines that never parse), not the
chip. Real cars do not repeat one id at kHz rates, but a busy bus does
reach that aggregate; worth an instrumented build (count dropped
chunks + unparsed lines) before changing the window design. The bench
PCAN-USB FD is currently NOT on the DUT/simulator bus (channel 2 sees no
traffic), so the DBC / monitor-injection benches that need it cannot
run until it is re-wired.
