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
  `ATS1;ATH1;ATST96;ATTP<p>;ATSH..;ATCRA` on CAN, headers on since
  2026-10-24) and replays every type/PID
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
  interval: reboot-to-apply like every component).

## API (`include/autopid.h`)

- `autopid_init()` / `_start()` / `_stop()`: settings + log descriptor; the
  poller task (PSRAM stack, notify-driven); stop latches the idle state.
- `autopid_group_set(group, enabled, period_override_ms)`: the EPHEMERAL
  runtime group switch/rate override (`POST /api/autopid/group`, the
  `autopid.group` rule action); `< 0` keeps the period, 0 = max rate.
- `autopid_group_restore(group)` (2026-09-17): back to the configured
  enable + rate: the undo of an `autopid.group` while-rule.
- `autopid_snapshot(&json)` / `autopid_get_value(param, &v, &unit)` /
  `autopid_set_value_sink(sink)` / `autopid_publish_external(...)`: the
  live value cache in and out.
- `autopid_config_json_dup(&s)` / `autopid_reload_config()`: the PID table
  file (`PUT /api/autopid/config` applies LIVE).
- `autopid_stats(&st)` / `autopid_ecu_online()`: poll counters, pause
  reasons, ECU presence.
- `autopid_dtc_scan_start()` / `_scanning()` / `_report(&json)` /
  `_clear(codes, mode, ...)` / `_desc(code, ...)`: the DTC engine.
- `autopid_register_http()` / `autopid_register_cli()`: own routes/commands.

## Files

| File | Role |
|---|---|
| `autopid.c` | lifecycle, the config tables and their lock, battery-pause watch, core query surface |
| `autopid_poller.c` / `autopid_poller.h` | the poller task (PSRAM stack, notify-driven): pauses, DTC due-check, scheduler -> runner -> bookkeeping (split out of autopid.c 2026-10-02) |
| `autopid_bus_guard.c` | PURE bus guard: the bitrate a chip protocol transmits at, and the verdict (`allow` / `search` / `park`) for what the native controller heard on the bus, with its one-sentence reason (see "Bus guard" below) |
| `autopid_guard.c` | the guard's glue: asks `can_manager` what is on the bus before the chip talks (a held listen-only watch until the bitrate is proven), parks the poller, refuses the one-shot jobs, `bus_guard{}` in `GET /api/autopid` |
| `autopid_runner.c` | the chip-facing poll and the test-a-PID one-shot: type/per-PID init transitions, ATCRA rxheader, request, parse, cross-talk guard; the bus guard's word on the protocols the tables set (`row_pins_ok()`); a one-shot (the test route, the detection, the DTC jobs) parks the poller with `ap_core_scan_pause(true)`, which since 2026-10-28 waits up to 3 s for the poller's busy window (a poll or a passive filter's monitor window, `AUTOPID_IDLE` set again) before the job's first command: the profile e2e bench's test route answered `chip busy (monitor/update)` twice when it landed in a filter window |
| `autopid_baseline.c` / `autopid_runner_private.h` | the chip baseline (out of `autopid_runner.c` 2026-10-20, 700-line rule): the prelude, the chain sender with the prelude behind a reset inside a chain (`after_reset()`), the memory the poll and the test share (`ap_base_mem()`), the borrowed-header rule (a standard row that set its own `ATSH` gives the functional header back before a row that sets none) and **the baseline back before the next row type** when a chain of the tables moved the chip (`ap_base_restore()`, issue #964, see "The baseline back" below). Since 2026-10-26 the sender tells the firmware's own chains (the prelude, the restore: the EEPROM guard alone, never an empty element) from the tables' chains (`ap_base_send_init`: the guard and the print strip of issue #1014): the strip had blanked the `ATH0` of the K-line / J1850 preludes and of the no-car prelude into an empty command; the no-car prelude is headers-on since the same day (`ap_veh_prelude_for`, the default case); since 2026-10-28 the prelude also goes out again when obd_chip's settings generation moved (`ap_base_chip_gen()` over the transport's `settings_generation`: a fresh bring-up, the CAN self-test's `ATD`), the chip is then at its defaults while the memory said otherwise |
| `autopid_chain.c` | PURE (2026-10-20, issue #964): what a `;`-separated chain of the tables sets on the chip beyond the baseline (`ap_chain_sets()`: header, receive filter, extended addressing, flow control, protocol, print format, adaptive timing, ...; read as the chip reads it) and the chain that puts the baseline back (`ap_chain_restore()`: the prelude plus one undo command per thing the prelude does not cover, `ATCEA`, `ATFCSM0`, `ATAT1`, ...) |
| `autopid_publish.c` | `ap_runner_publish()` = the publish half of a poll (mux, expression, the bounds, cache, events), shared by the chip runner and the J1939 runner (out of `autopid_runner.c` 2026-10-05, 700-line rule) |
| `autopid_bounds.c` | PURE (2026-10-20, issue #1004): a parameter's `min` / `max` applied to one decoded value (`ap_bounds_apply()`): float noise never drops a reading (a value beyond a bound by at most 1e-6 of it is clamped to the bound), beyond that a standard OBD row clamps to the table's range (its display range, legacy parity) and a custom, specific or J1939 row drops the reading (its bounds are a filter), see "The bounds" below |
| `autopid_transcript.c` | pure: the test-a-PID transcript, one line per exchange (`ap_tr_add()`; out of `autopid_runner.c` 2026-10-05, 700-line rule) |
| `autopid_runner_j1939.c` | the passive runner (2026-10-03, see "J1939 rows"): a `PGN:` row read from the J1939 listener's store, published when the store holds a message the row has not published yet, stamped with the message's own time; the test-a-PID path for a PGN command |
| `autopid_j1939_core.c` / `autopid_j1939.h` | PURE J1939 rows: the `PGN:<hex>[@<source>][?]` grammar, the scheduling class of a row (`chip` / `passive` / `bus_tx`), the little-endian expression of one SPN of the j1939 component's built-in table (`(B3+B4*256)*0.125`) |
| `autopid_j1939_std.c` | the J1939 standard set: the built-in SPN table as config rows (one row per SPN), the listener's view of the vehicle for the detection job and first contact (sources as the responder set, the VIN when broadcast, the rows of the groups heard), and the same from a one second listen-only bus sample when the listener is off (`can_manager_sample_ids`) |
| `autopid_dtc_j1939.c` | the J1939 codes in the DTC report: every controller's DM1 from the listener's store (`SPN110-0`, source address, occurrence count, the four lamps), nothing asked |
| `autopid_http_test.c` | `POST /api/autopid/test` (split out of autopid_http.c 2026-10-03): one shot through the runner, or the listener's store for a `PGN:` command |
| `autopid_contact.c` | first contact, once per boot (moved out of the runner 2026-10-03): the identity check after the first answered poll, the stored-protocol fallback, and the contact probes that look for a car the tables do not reach (see "OBD dialects") |
| `autopid_identify.c` | who is this car, shared by first contact and the detection job: the responder set (one headers-on bitmap request) and the VIN, per dialect |
| `autopid_dialect.c` / `autopid_dialect.h` | PURE OBD dialects (`obd2`, `uds`, `j1939`): the requests of a dialect, the data shift of its expressions, the bitmap + responder parser for every print shape of the chip (11-bit, 29-bit as four byte tokens, headers off), the per-ECU support table and the owner of a PID, the physical address of a responder, the CAN protocols a contact probe may use on a given bus |
| `autopid_filter.c` | the ATMA filter window: MONITOR claim, header/CRA choreography, frame capture (`ap_filter_frame`, pure), retried stop |
| `autopid_std.c` | standard PIDs: vendored SAE table (obd2_standard_pids.h, included ONLY here), PURE bit_start to expression mapping (with the row's range: a percent row's full scale as the exact ratio `B2*100/255`, a range below zero as a signed span `[S2:S3]*0.25`, issue #1004) + bitmap parser, freeze-frame decode, the table JSON view |
| `autopid_names.c` | PURE (2026-10-06): parameter names made unique, `ap_names_dedupe()` over a `pids` array (config rows or the scan's `supported` entries, the later repeat gets `_2`, `_3`, ...) and `ap_config_repair_names()` for a stored file; see "No parameter name twice" below |
| `autopid_std_scan.c` | the vehicle DETECTION job (the async support scan, split out 2026-10-01): phases `protocol` (which protocol and which dialect answer: a walk over the pinned CAN protocols the bus guard allows, `0100` then `22F400` on each), `vin` (`autopid_identify.c`), `pids` (`autopid_scan_walk.c`); hands the result to the vehicle store (`autopid_vehicle_detected`), then writes `/data/autopid/std_scan.json`; started by the poller itself on an unknown car; owns `ap_std_prelude()` |
| `autopid_scan_walk.c` | the job's `pids` phase: the support bitmap walk of the car's dialect (per ECU with headers on for `uds`) and one config row per supported PID that decodes something |
| `autopid_scan_rows.c` | PURE (2026-10-09): the detection's rows as a new car's tables document (`ap_scan_rows_config()`: type `std`, group `default`, a uds row with its ECU), every row stored `"enabled": false`: nothing is read until the user ticks the ones they want; see "A new car" below |
| `autopid_vehicle_core.c` | PURE vehicle identity (TASK_quick_setup.md): ATDPN parse, VIN from 0902 / 22F190 replies, responder fingerprint (FNV-1a over sorted ECU id + 0100 bitmap pairs), effective-protocol + prelude selection, the first-pass vehicle.json reader (import); `autopid_vehicle.h` holds every vehicle contract |
| `autopid_vehicle_index.c` | PURE store index (second pass): key derivation (`VIN` or `fp:<hash>`), the route key's percent-decode (`ap_vidx_key_decode`, 2026-10-26), find by key / VIN / fingerprint, the fingerprint SUBSET rule, `ap_vidx_match`, the LRU eviction pick, add/remove, the once-a-day `last_seen` touch |
| `autopid_vehicle_codec.c` | PURE store codec: the responder set as text, one entry as JSON (API + file shape), `vehicles.json` round trip with bounds and sanitizing |
| `autopid_vehicle.c` | the store: RAM index under one lock, load + the first-pass import, queries, edits (`update` / `activate` / `delete`), the `config.json` mirror, the current car's protocol and dialect getters, the one-shot internal-stack worker `apid_veh` that does the poller's file work |
| `autopid_vehicle_switch.c` | the store's files: per-car tables copy/snapshot, the switch (snapshot, copy, live reload, SPECIFIC init, event), the new-car path with LRU eviction, first contact (`autopid_vehicle_seen`), the detection result (`autopid_vehicle_detected`) and the one-time `obd_chip_protocol_save()` |
| `autopid_sched.c` | PURE scheduler: due times, group inheritance/override (`ap_sched_group_set`/`_restore`, 2026-09-17), fail backoff (×4 after 3), period-0 high-fidelity round-robin, stagger; `ap_sched_next_of()` picks among the entries of given classes (the poller asks for the chip's rows only while it may use the chip), a passive row at period 0 is looked at every 20 ms |
| `autopid_group.c` | runtime group control: `autopid_group_set`/`_restore` (name lookup under the core lock + poller wake-up over the pure scheduler calls) and the group state JSON (split out of autopid.c 2026-09-17) |
| `autopid_resp.c` | PURE ELM text → bytes: the framing-free payload for the parsers and the guard (`ap_resp_to_payload`: headers on/off, ISO-TP single/multi, lowest-responder rule, noise/error lines, an extended address byte before the PCI, the chip's 29-bit header print `18 DA F1 xx` and a row's own 29-bit id by hint, `7F xx 78` response-pending lines dropped) and the reply as the chip prints it with headers on for the expressions (`ap_resp_frames`, 2026-10-24, issue #1014; headers-off replies rebuilt into that shape, rows in arrival order) + the cross-talk guard (`ap_payload_matches_cmd`: a second chip master's response can't be cached as ours; service 22 is compared on both identifier bytes) |
| `autopid_resp_filter.c` | PURE ATMA monitor lines → frame data bytes: `ap_filter_frame` (one line carrying the wanted id) and the incremental stream collector `ap_flt_stream_*` the filter window feeds; split out of `autopid_resp.c` 2026-10-02 (700-line rule) |
| `autopid_config.c` | PURE JSON parse/validate + target file load/save (atomic) |
| `autopid_cache.c` | latest-value slots (PSRAM, mutex), legacy snapshot + detail JSON, plus the **external-value table** (12 name-keyed injected samples: GPS from the ESPNetLink dongle) merged into every snapshot/get/detail |
| `autopid_dtc_codec.c` | PURE DTC codec: 2-byte code ⇄ "P0420", 43/47/4A payload parse, 41-01 MIL/count, clear-condition matrix (`always/if_any/if_only`, mode 04 is all-or-nothing, so "clear specific" = a condition on the present set), and the report fillers (`ap_dtc_report_add`: the merged category arrays plus one item per ECU; `ap_dtc_report_src`: the lamp word of every responder) |
| `autopid_dtc.c` | DTC engine: the settings-applied knobs (`ap_dtc_cfg()`), the scan itself (01-01 gate, the OBD path, the UDS fallback, the diff: events fire per NEW code), the conditional clear (fresh read → condition → clear → confirming read) and the RAM report. Split 2026-10-02 (700-line rule) into this file plus the four below; `autopid_dtc_engine.h` is what they share |
| `autopid_dtc_job.c` | the one-shot job task (`apid_dtc`, PSRAM stack) and its triggers: scan start, the rule-queued clear, the periodic due-check the poller calls |
| `autopid_dtc_obd.c` | the OBD path: chip choreography (`dtc_init`, headers on, ATCRA), the 03/07/0A request with its multi-ECU merge, the mode-02 freeze frame |
| `autopid_dtc_uds.c` | the UDS path: `19 02` / `14` to one physical address pair through `uds_request()` |
| `autopid_dtc_wwh.c` | the WWH-OBD path (ISO 27145-3 / SAE J1979-2), the legislated codes of a `uds`-dialect car: `22F401` lamps, `19 42 33` confirmed and pending, `19 55 33` permanent, `14 FF FF 33` clear; functional with headers on, every ECU parsed apart (the codec is uds_manager's `uds_dtc.h`) |
| `autopid_dtc_report.c` | the report as JSON (`GET /api/autopid/dtc`), read in place under the engine's lock (it is ~5 KB: no copy on a task stack), and the public `autopid_dtc_*` wrappers (script engine) |
| `autopid_dtc_db_codec.c` | PURE DTC-database importer: sniff + parse CSV/TSV/semicolon/JSON-map/JSON-array/plain text → canonical sorted form ([TASK_dtc_db.md](TASK_dtc_db.md)) |
| `autopid_dtc_db.c` | database store (/data/autopid/dtc_db) + PSRAM cache w/ binary search: upload/list/delete/search/lookup, report `desc` enrichment, lookups never touch flash |
| `autopid_dbc_codec.c` | PURE DBC codec: BO_/SG_ parser + the signal→expression compiler (Intel/Motorola, signed via multiply-subtract, Motorola-aligned spans; host cross-checked vs a reference decoder): [TASK_dbc.md](TASK_dbc.md) |
| `autopid_dbc.c` | DBC store (/data/autopid/dbc) + PSRAM cache: upload/list/delete, signal browse/search, and the add-to-filters merge (dry-run validate → atomic save → LIVE reload) |
| `autopid_http.c` | the route table + `/api/autopid[/data\|/config\|/std_scan*\|/test\|/group\|/std_table]` handlers and the shared response helpers (`ap_http_send_json/_error/_file`, exported through `autopid_http_private.h`); see `components/HTTP_API.md` 6e4 |
| `autopid_http_dtc.c` | the DTC handlers (`/api/autopid/dtc*`, 6e4b) and `ap_http_query_param()`; split out 2026-10-01 to keep every file under the standard's 700 lines |
| `autopid_http_dbc.c` | the DBC handlers (`/api/autopid/dbc*`, 6e4c); same split |
| `autopid_http_vehicles.c` | the vehicle store handlers (`/api/autopid/vehicles*`, 6e4): one wildcard URI per method, `detect` / `<key>` / `<key>/activate` dispatched inside |
| `autopid_cli.c` | `autopid [-l] [-d] [--dtc-scan]` console command (§6b self-registration) |

## Settings (`/api/settings/autopid`, reboot-to-apply)

`enabled` (default **false**), `std_enabled`/`custom_enabled`/
`specific_enabled`, `std_init`/`custom_init`/`specific_init`
(';'-separated AT prelude per type; `specific_init` is the fallback for a current car without its own `specific_init` in the vehicle store), `std_protocol` (enum `0` Automatic | `6` CAN 11-bit 500k | `7` CAN 29-bit 500k | `8` CAN 11-bit 250k | `9` CAN 29-bit 250k, default `0` since 2026-10-03 (it was a pinned `6`: a fresh device must not transmit at 500k onto a bus it has not heard); schema v7; `0` = follow the vehicle store: the detection job learns the protocol per car and the chip prelude pins the current car's protocol instead of `ATTP0`, see "Vehicle store" below; a pinned 6..9 always wins over the store), `vehicle`,
`pause_below_mv` (0 = no fixed threshold, else 1–14500 mV; resumes +0.3 V with 5 s hold; with `pause_follow_sleep` the threshold is the Power Saving sleep voltage and polling resumes at its wake voltage, sleep_manager v3, 2026-10-01),
`pause_follow_sleep` (default **true**: legacy `disable_pid_requests`
parity: requests pause below the sleep threshold via the battery
watch; explicit `pause_below_mv` overrides), `pause_mode`
(`all`|`requests_only`), `min_event_interval_ms`
(10–600000), `cli`; DTC (schema v4, ALL off by default): `dtc_enabled`
(**false**, master gate), `dtc_allow_clear` (**false**, second gate
for mode 04 / UDS 0x14), `dtc_scan_period_min` (0 = on-demand only),
`dtc_pending`/`dtc_permanent` (include modes 07/0A), `dtc_init`
(';'-separated chip prep per scan), `dtc_rxheader` (ATCRA filter:
targeted-ECU scans); UDS transport (v4, TASK_dtc.md §12):
`dtc_protocol` (`obd`|`uds`|`auto`: auto = OBD first, UDS fallback),
`dtc_uds_txid`/`dtc_uds_rxid` (default 7E0/7E8), `dtc_uds_ext`
(29-bit ids), `dtc_uds_mask` (0x19 status mask, default 0x08
confirmed).

DTC events/actions/pull-values + rule recipes (scheduled scan+report,
alert-on-new-code, clear-when-detected, scheduled clear) and the Berry
`dtc_scan()`/`dtc_clear()` bindings: [TASK_dtc.md](TASK_dtc.md) §7-9.
DTC works with polling `enabled=false` (scans only need the OBD chip).

**EEPROM guard (ATSP → ATTP, ATM1 → ATM0; since 2026-09-16 the chip driver's own guard, `obd_chip_guard.h`, which also REFUSES ATPP/ATSD/ATCV/STWBR: config parse rejects a PID whose `cmd`/`init` carries one)**: every user-supplied chip
command, init strings (type inits, per-PID `init`, `dtc_init`) at
send time, PID `cmd`/`init` fields at config parse, and the test-a-PID
one-shot, is sanitized (case-insensitive, whitespace-tolerant:
`ap_init_sanitize`, host-tested): `ATSP` becomes `ATTP` (same protocol
switch, RAM only) and `ATM1` (memory on, makes the chip store every
later protocol change) becomes `ATM0`. Both write the chip's EEPROM,
and PID commands/inits replay on every poll cycle / type transition,
so either would wear it out (legacy semantics). Monitor commands
(`ATMA`/`ATMR`/`ATMT`) and `ATM0` pass through untouched. Write ATSP
in configs/profiles freely; the chip only ever sees ATTP.

Since 2026-11-01 (issue #1023) the chip driver also sends `ATM0` ahead
of every `ATTP` (and `STP` / `VTP`) it is asked for, inside the same
transaction: the chip's memory function is ON after every reset and
wrote every protocol flip that a request answered on into its
EEPROM-backed protocol store, `ATTP` or not (the VW MEB profile: seven
writes per poll cycle, the store rated for 10k). The prelude, the
restore and every row init go through `obd_chip_request()`, so nothing
in this component changed; the transcript of test-a-PID shows the
switches as before and `GET /api/obd_chip` `eeprom_guard.memory_off`
counts the `ATM0` words sent ahead of them.

## Bus guard (2026-10-03)

The OBD chip transmits at the bitrate of the protocol it is told to use. A
request at the wrong bitrate is not a harmless miss: nobody can read it,
every node answers with an error flag, and the chip repeats it (measured:
a pinned 500k request on a live 250k bus = a 65 ms burst, 257 to 1255 error
frames, and the poller would repeat it every period). So before the chip
talks, autopid asks the native CAN controller what is on the bus
(`can_manager_probe()`: listen-only, no TX pin, works while can_manager is
disabled) and `ap_guard_decide()` rules:

| the bus | the protocol in effect | verdict |
|---|---|---|
| silent, or unknown | any | `allow` (a gatewayed OBD port answers only when asked) |
| live at the protocol's bitrate | any | `allow` |
| live at the other bitrate | from the vehicle store (`std_protocol` = `0`) | `search`: the chip's own protocol search is used (it matches the bus frequency before it sends) |
| live at the other bitrate | pinned by the setting (`6`..`9`) | `park`: nothing is written to the chip |
| traffic neither 500k nor 250k reads | a CAN protocol, or the search | `park` |

Protocols 1..5 (K-line, J1850: other pins) and the user CAN definitions B
and C are not judged: `allow`.

- The poller asks before EVERY transmission until the answer is final: the
  bus named its bitrate (frames were read) or an ECU answered. A silent bus
  is not final: a vehicle asleep at boot wakes up later at its own bitrate,
  so a listen-only watch stays on the bus meanwhile (it costs nothing while
  the bus is silent).
- **A verdict is about ONE protocol** (2026-10-05). It is final for the
  protocol in effect it was taken for: when another one is in effect (another
  car made current, `POST .../vehicles/<key>/activate`; a search given up)
  the bus is looked at again. And whoever sends a prelude that pins a
  protocol (the runner's baseline, its restore after an app or a job, the
  identity check) reads the protocol ONCE, asks `ap_guard_pin_ok(proto)` and
  sends the prelude of that reading or nothing: the row fails this pass, the
  next pass asks the guard (`ap_guard_pin_allowed()`, pure, host-tested).
  Before that the first verdict stood for the boot and the poller asked at
  the top of its pass: a 500 kbit/s car activated from the UI while plugged
  into a 250 kbit/s truck changed the protocol in effect under a pass that
  already had its "yes", the baseline went out as `ATTP6`, and the truck's
  adapter was bus-off 40 ms after its first error (about every other
  activation on the bench; `eu_truck_e2e_bench.py` leg e4 does it on
  demand and wants zero bus errors at the truck).
- **The tables' own protocols** (2026-10-05). The verdict is about the
  protocol autopid pins itself. A chain of the tables can set one too
  (`ATSP6` in a row's `init`, in a type's init, in the vehicle's own init, in
  `dtc_init`, in a row tested from the UI: 13 of the 17 vehicle profiles in
  the firmware repo carry `ATSP6` or `ATSP7`), and such a chain goes to the chip as
  written. So whoever sends one asks `ap_guard_chain_ok()` first: EVERY
  protocol the chain sets gets the ruling of a pinned setting on the bus as
  the guard last saw it (`ap_guard_chain_allowed()`, pure, host-tested: no
  search to give way to; a silent or unknown bus changes nothing; the same
  bit rate is allowed, so a profile that mixes 11 and 29 bit keeps working).
  Refused = NOTHING of the row is sent: not its protocol, not its header on
  the protocol in effect (an 11-bit header on a truck's 29-bit protocol
  makes a frame the network has a meaning for), not the request. The poll
  counts as failed (first contact goes on and finds the vehicle that is
  there), test-a-PID and the DTC scan / clear answer 409 with the sentence
  (`the init of row Soc sets protocol 6 (500 kbit/s) and the vehicle bus
  runs at 250 kbit/s: not sent`), and `GET /api/autopid` counts and explains
  it (`bus_guard.refused`, `refused_reason`, `refused_ms`: the dashboard
  shows a banner while it happens). The runner also remembers the protocol
  a chain left the chip on and asks again before every request that goes out
  on it (a bus asleep when it was sent may have woken up at another bit
  rate); refused then, the baseline prelude takes the chip off it. Bench
  2026-10-05, before this: a row with `ATSP6` on the 250 kbit/s truck (polled
  after a boot, tested from the UI, or as `dtc_init` / `custom_init`) went
  out at 500 and the truck's adapter was bus-off 19 to 55 ms after its first
  error, the chip left on protocol 6 for every poll that followed
  (`eu_truck_e2e_bench.py --only x5`).
- **A reset inside a chain** (`ATZ`, `ATD`, `ATWS`; 2026-10-05). The chip
  comes back as `A<n>`: automatic, starting from the protocol STORED in it
  (the last vehicle detected), and its first request went out on that one
  about every third time, without the look at the bus its full search takes
  (bench: the car's stored 500 kbit/s protocol on the 250 kbit/s truck after
  `ATWS`, `ATZ` and `ATD`, the truck's adapter bus-off 17 to 32 ms after its
  first error; 3 of the 7 resets that executed). So whoever sends a reset
  out of a chain of the tables sends the baseline prelude right behind it
  (`after_reset()` in the runner, `ap_dtc_obd_prep()`; a reset as a row's
  `cmd` makes the baseline due), and the rest of the chain builds on the
  protocol in effect, as at the start of every row. A command sent while
  the chip is still searching is swallowed as the search's stop (`STOPPED`),
  resets included. No profile in the repo carries a reset. After the
  change: 24 resets on the truck's bus, 0 bus errors (leg `x5` step e keeps
  one in the gate).
- **The first chip traffic of a boot** (2026-10-05). The driver's bring-up
  resets the chip, so the same holds from boot until a prelude went out.
  The poller, the detection, the DTC jobs and first contact all start with
  one; test-a-PID did not ("leaves the chip state dirty on purpose" was
  about what it leaves behind). With Automate off, a row tested as the
  first thing after a boot went out on the chip's stored protocol: 8 of 8
  boots with the truck's protocol stored (`ATDPN` `9`) and a 500 kbit/s
  vehicle on the bus, `> 0100 < CAN ERROR`, bus errors at the vehicle every
  time. `ap_runner_test()` now sends the baseline prelude first when none
  went out since the chip was last reset (the transcript shows it); 8 of 8
  clean after (leg `x5` step f).
- After the poller's first transmission "unreadable" may be the chip's own
  unanswered requests, so only readable frames decide from then on.
- One-shot chip jobs ask too: the detection / std scan, test-a-PID, the DTC
  scan and clear answer HTTP 409 with the guard's sentence.
- Visible as `stats.paused_bus` and `bus_guard{bus,bus_kbps,verdict,parked,
  reason,refused,refused_reason,refused_ms}` in `GET /api/autopid`, one `W`
  log line per change of mind, and the banner on the Automate dashboard.
- A protocol learned by the search is stored for the car (the identity check
  asks `ATDPN` before the prelude resets the search).

Bench: `tools/testbench/can/can_autobaud_bench.py` legs c0..c3 (the PCAN
adapter counts the error frames on the wire: zero while parked, zero with the
search, and the chip's UART byte counter does not move).

## OBD dialects (2026-10-03)

The legislated data of a vehicle (the standard PIDs, the VIN, the trouble
codes) is asked in one of two ways, and which one is a property of the car,
kept as `dialect` in the vehicle store:

| | `obd2` | `uds` |
|---|---|---|
| standard | SAE J1979 / ISO 15031-5 | ISO 27145 (WWH-OBD: EU heavy duty, vans) and SAE J1979-2 (OBD on UDS: US cars from MY2027) |
| is anybody there | `0100` → `41 00` + bitmap | `22F400` → `62 F4 00` + bitmap; **no answer to `0100` at all** |
| a PID | `010C` → `41 0C A B` | `22F40C` → `62 F4 0C A B` (the same data, one byte further in) |
| VIN | `0902` | `22F802` |
| bitmap ranges | `00`..`A0` | `00`..`E0` |
| trouble codes | `03` / `07` / `0A`, clear `04` | `19 42 33 <mask> 1E`, `19 55 33`, clear `14 FF FF 33` |

`j1939` = a vehicle that only broadcasts (SAE J1939: trucks, buses,
agricultural machines): no OBD request is answered, the chip stays parked for
it, and its data comes in as J1939 rows (next section). An EU truck answers
WWH-OBD AND broadcasts J1939: its dialect is `uds` and its `j1939` flag is
set, both sets of rows apply.

## J1939 rows (2026-10-03, TASK_j1939_wwh.md phases 5 and 6)

A J1939 parameter group is an ordinary PID row whose `cmd` reads

```
PGN:<hex>[@<source>][?]     PGN:F004      engine controller 1, the pick
                            PGN:FEEE@0    engine temperature from source 0
                            PGN:FEE5?     engine hours, a group sent on request
```

`<hex>` is the group number (1 to 5 hex digits; a PDU1 group with its
destination byte zero), `@<source>` pins a source address (decimal or
`0x..`, 0..253; without it the listener's pick: the lowest address heard in
the last 5 s, else the lowest of all), `?` marks a group that is sent on
request only: in `j1939` `mode` active the row asks for it (see below), in
listen mode it publishes whatever another node asked for. Any `type`, the same groups,
periods, parameters and expressions as any row; B0 is the first data byte
and J1939 words are little-endian, which the grammar's `[Bx:By]` span is
not, so they are composed byte by byte (`B3+B4*256`). `init` and `rxheader`
mean nothing to such a row and are refused by the config parser.

**Never the chip.** The rows are served by `autopid_runner_j1939.c` from the
store of the `j1939` component (which needs `can_manager` enabled, `auto`
or the bus's bitrate, `silent`): the newest message of the group, published
when the store holds a message the row has not published yet (its `(source,
count)` moved), stamped with the message's own time so the value's age is the
data's age. A row at period 0 is looked at every `AP_PASSIVE_FLOOR_MS` (20 ms)
instead of spinning. A message from ANOTHER source that is already older
than 5 s when a row first sees it is not news (the store's pick moved
because the bus fell silent), the row keeps what it published; a new
message of the source the row follows is news however old the look finds
it. A group nobody sends fails like a silent PID and backs off the same way.

**Scheduling classes.** Every entry has a class (`ap_row_class`): `chip`
(every request through the OBD chip, filters too), `passive` (a PGN row),
`bus_tx` (a PGN row with `?`). The poller picks among the chip's rows only
while it may use the chip; the passive rows run through every pause that is
about the chip: an external ELM app on the bridge (`paused_client`), a
diagnostic tool's hold (`paused_diag`), the bus guard (`paused_bus`), a chip
job. The voltage pause stops them only under `pause_mode` = `all`
(`requests_only`, the default, keeps the rows that transmit nothing running;
the setting existed since v1 and did nothing until now). `GET /api/autopid`
counts them apart: `passive_ok` / `passive_failed` (looks: the group was
there / nobody sent it) and `passive_published`.

**`?` rows ask (phase 6).** When the `j1939` component is in `mode` active
and holds an address (`j1939_active()`), a `bus_tx` row sends a Request for
its group before the look (`j1939_request()`, to the pinned source or to
everyone), at its period floored to `AP_BUS_TX_FLOOR_MS` (1 s: J1939
etiquette, and 50 requests/s at period 0 would be a flood), and publishes
the answer at the next look, one period later. A controller that answered
the request negatively (not supported, denied, busy) fails the row
(`passive_refused`) and holds the asking for `AP_J1939_NACK_HOLD_MS` (60 s);
the scheduler's backoff does the rest. A diagnostic tool's hold
(`paused_diag`: the UDS Tool or a J2534 tester with their exclusive option)
stops the asking, not the reading: the tool's session is not ours to talk
over. `GET /api/autopid` `passive_requested`, `passive_refused`,
`j1939_active`.

**The standard set.** One row per SPN of the j1939 component's built-in
table (21 SPNs: engine speed and torque, pedal, load, gear, wheel speed,
coolant and oil temperature, oil and intake pressure, barometric and
ambient, battery, fuel rate, fuel and DEF level, distances, hours, total
fuel), `cmd` `PGN:<hex>`, `type` `std`, the SPN's unit and HA class, `min` /
`max` = the operational range so the plausibility clamp drops "not
available" (`FF..`) and error (`FE..`) raw values by itself. The detection
job stores the rows of the groups it heard; `GET /api/autopid/std_table`
appends the whole table for a vehicle with the `j1939` flag.

**Detection and first contact.** The detection job's fourth phase, `network`:
with the listener up, the listener's store says whether this is a J1939 bus
(two distinct well-known groups), who is on it (the source addresses become
the responder set behind the fingerprint; NAMEs are not used, a claim is
heard on some boots only), the VIN when one was broadcast (PGN 65260; in
active mode it is asked for once and given 1.5 s, so a truck is keyed by its
VIN at once), and the rows of the groups heard; with the native bus still off, a one second
listen-only sample of a live bus through `can_manager_sample_ids()` says the
same without the VIN. A vehicle that answered no OBD request but broadcasts
J1939 gets the dialect `j1939` (chip parked: no `ATTP0`, no idle `0100`, no
`ATSP` save) and the result carries `j1939:true`, `j1939_listening:false`,
`bus_kbps`: the Quick Setup stages `can_manager` (listen-only, that bitrate)
+ `j1939` for one restart. First contact (`autopid_contact.c`) treats the
listener's word as a sighting once the chip's probe found nobody (an EU truck
answers the chip too, whose responders are the better print), and gives the
listener a ten second head start on a stored J1939 vehicle so a truck's boot
sends no OBD request at all; the VIN broadcast (a BAM, on request or at
start-up, a few seconds after the groups) gets the same time before an
identity without it, and a VIN heard within two minutes after such an
identity is still adopted (the store matches an entry learned without its
VIN by its fingerprint). A chip sighting never takes the `j1939` flag
away; a listener sighting never replaces a chip dialect or a chip responder
set. The listener's word counts from the poll-result path too, once the
chip's probe ran and found nobody: a device moved from a car to a truck
polls the car's rows, they fail, the chip probes, and the truck becomes the
current car (6 s on the bench); until 2026-10-03 only the idle path asked
the listener, which a table of chip rows never reaches, and the car's rows
were polled for ever.

**Trouble codes.** A scan on a vehicle with the `j1939` flag folds in every
controller's DM1 from the store (`autopid_dtc_j1939.c`): `SPN110-0` codes as
`stored` (active) with `sa` and `oc` per item, the four lamps (`mil`, `rsl`,
`awl`, `pl`) per source and folded, `protocol` `j1939` when the chip was not
used. In active mode the scan first asks everyone for DM2 (the previously
active codes, a group only ever answered to a request) and waits 1.5 s; they
join the report as `pending` items with `sa` / `oc`. The clear on a
J1939-only vehicle (`ap_dtc_j1939_clear`, no chip job): the condition is
evaluated against the active codes heard now, DM11 (active codes) then DM3
(previously active) are requested of every controller heard and their
acknowledgments awaited (1.5 s each); `cleared` when at least one controller
acknowledged DM11, `after` = the active codes 1.5 s later (the next DM1
broadcast), and a fresh scan is queued so the report follows the network.
Listen mode is refused with 403 `J1939: clearing needs mode active (j1939)`,
no address yet with 409.

Bench: `tools/testbench/obd/autopid_j1939_bench.py` (third stage of
`.\test.ps1 j1939`, verdict `AUTOPID J1939 PASS`): detection from a bus
sample and from the listener, every stored row equal to the PCAN truck's
value with the chip's counters at zero, a change on the truck at the API and
on the Pi's broker, the rows publishing while an ELM app holds the chip,
`pause_mode`, the DM1 report and the refused clear, test-a-PID through the
store, a pinned custom row, and silence (nothing new, values keep their
reading and grow old). The active half is in
`tools/testbench/can/j1939_active_bench.py` (fourth stage, `J1939 ACTIVE
PASS`): the car switch by the VIN asked for, a `?` row asking at its period
and publishing, a refused `?` row held, DM2 in the report, the DM11 / DM3
clear acknowledged and the report empty after it, the asking stopped by a
J2534 tester's hold.

Both dialects on one vehicle: `tools/testbench/obd/eu_truck_e2e_bench.py`
(`.\test.ps1 eutruck`, verdict `EU TRUCK E2E PASS`), an EU truck (WWH-OBD
ECUs and a J1939 network on one bus, one VIN) met by a device that knows an
OBD-II car. The detection stores `dialect uds` with `j1939 true` and rows of
both sets (the network read from a bus sample, the native bus still off);
the one restart the Quick Setup stages brings both live (the chip asks
`22F4xx` physically, the `PGN:` rows read the listener, the broker snapshot
carries both); one DTC report holds the `19 42` codes and the DM1 code, the
legislated clear takes the first and leaves the second heard; moved back to
the car, the car's rows are polled again and the truck stays in the store.

**Rows.** The standard rows of a `uds` car are ordinary rows of type `std`
with `cmd` `22F4xx`, expressions shifted by one byte (`[B3:B4]*0.25`), the
SAE names and units unchanged (the same entities in Home Assistant), and an
`init` that addresses ONE ECU: `ATSH18DA58F1` (29-bit) or `ATSH7E0`
(11-bit). That ECU is the row's owner: the lowest responder whose support
bitmap has the PID. Why physical and not functional + `rxheader`:

- a functional request with the chip's receive filter (`ATCRA`) on one ECU
  leaves every other ECU that holds a multi-frame answer without its flow
  control. It waits out its timeout and answers the NEXT request late,
  inside another request's window (bench 2026-10-03, two ECUs: `NO DATA`,
  then a stale `62 F4 00 ..` line in front of the next answer);
- a second unit may repeat a PID in its own scaling (a Mercedes Sprinter
  VS30's `5A` answers the engine speed unscaled; `58` is the engine). Asked
  physically it never gets the question.

**The borrowed header.** A standard row that sets its own request header
gives the functional one back: before any row that does not set one (a
custom row, a specific row, a standard row added by hand), the runner sends
`ATSH7DF` / `ATSH18DB33F1` again, so such a row finds the same baseline as
on an OBD-II car. Within a type a header set by a custom or specific init
stays in effect as it always did. Test-a-PID follows the same rule, and
every DTC job starts from the whole prelude.

**The baseline back (2026-10-20, issue #964).** A chain of the tables (a
type's init, a row's init, an AT command as a row's `cmd`) can move the chip
much further than the header: a 2018 BMW 640i GT's custom odometer row
(`ATSH6F1;ATCEA60;ATCRA660;ATFCSH6F1;ATFCSD60300000;ATFCSM1`) left the chip
on the gateway behind extended addressing with the receive filter on it, so
every Mode 01 row that followed went to the gateway, which answered
`7F 01 11`, and the values froze (on the legacy firmware decoded as RPM
68.25, speed 1, coolant -39; on v6 the rows fail and the values stop
moving: nine rows frozen from the second cycle on the bench). The runner
reads every chain it sends for what it sets beyond the baseline
(`ap_chain_sets()`, pure) and before the next row TYPE puts the baseline
back: the prelude (`ATS1;ATH1;ATST96;ATTP<p>;ATSH7DF;ATCRA`) plus one undo
command per thing the prelude does not cover (`ATCEA` for extended
addressing, `ATFCSM0` for flow control, `ATAT1`, `ATCAF1`, `ATCP18`, `ATE0`,
`ATL0`, `ATR1`, `ATV0;ATD0`), then the new type's init and the row's init as
before. Within a type nothing changes: a row finds what the rows before it
set. The same happens in test-a-PID (the transcript shows the restore) and
at the first poll after a test. Cost: one restore per type transition that
follows a moving chain (eight commands for the BMW row, about 60 ms).
Bench: `tools/testbench/obd/autopid_init_leak_bench.py` (`AUTOPID INIT LEAK
PASS`, the ECU simulator as the car plus `actors/pcan_ext_addr_ecu.py` as
the extended-addressed gateway on the PCAN).

**Contact.** The chip's own protocol search (`ATTP0`) asks `0100`: it
cannot find a `uds` car, and it needs 9.3 s to say `UNABLE TO CONNECT`. So
first contact (`autopid_contact.c`) and the detection job look for the car
themselves: on each pinned ISO 15765-4 protocol the bus guard allows for
this bus, `0100`, then `22F400` (0.6 s each when nobody answers).

- Boot: the prelude pins the stored protocol; the first answered poll
  triggers the identity check in the car's dialect (the other dialect when
  that yields nothing).
- The stored protocol stays silent for three polls (or the tables are
  empty): contact probes every 10 s, doubling up to 80 s while nothing
  answers: the walk above. Whoever answers is identified; a known car
  switches the tables, an unknown one starts the detection job with what
  was found as a hint. This is what makes the device find its car again
  when it is moved between an OBD-II car and a WWH van (bench: values 12 to
  16 s after boot, both directions).
- A pinned `std_protocol` is never left: both dialects are asked on it.

**The chip's 29-bit print.** With headers on the chip prints a 29-bit id as
FOUR byte tokens (`18 DA F1 10 06 41 00 ..`). Until 2026-10-03 the parsers
took such a line for data: detection kept a 29-bit OBD-II car's responders
as one row without an id, and a DTC scan on any 29-bit car found "no ECU".
`take_id29()` takes the four tokens as an id for the legislated answers
`18 DA F1 xx` followed by a PCI byte that fits the line, and since
2026-10-24 (headers on for every poll) also for the id a row's own
receive filter names (`ATCRA17FE007B`, the MEB profile) and, on a 29-bit
protocol, for any four bytes followed by a whole frame: the runner tells
the parser both before a reply is read (`ap_runner_reply_hint()`).

**Trouble codes.** `dtc_protocol` = `obd` means "the legislated codes": on
a `uds` car the scan and the clear take the WWH-OBD path
(`autopid_dtc_wwh.c`). `GET /api/autopid/dtc` names the path a job takes
now (`path`: `obd` / `wwh` / `uds`) and the report says who reported what:
`sources[{ecu, mil, count}]` and `items[{code, kind, ecu, status,
severity}]` beside the merged `stored` / `pending` / `permanent` arrays
that events, rules and scripts keep reading. A code in the SAE J1939 format
(DTC format 02) reads `SPN3226-4`.

Bench: `tools/testbench/obd/wwh_obd_bench.py` (`.\test.ps1 wwh`, verdict
`WWH OBD PASS`); the vehicle is `tools/testbench/actors/pcan_wwh_ecu.py`,
including the Sprinter VS30 of the field report (`--sprinter`).

## Config file shape (`PUT /api/autopid/config`)

```json
{
  "groups": [{"name": "driving", "enabled": true, "period_ms": 500}],
  "pids": [{
    "name": "engine", "type": "std|custom|specific", "cmd": "010C",
    "init": "ATSH7E0", "rxheader": "7E8", "group": "driving",
    "period_ms": 0,
    "parameters": [{"name": "rpm", "expression": "[B3:B4]/4",
                    "unit": "rpm", "class": "frequency",
                    "min": 0, "max": 16384}]
  }],
  "filters": [{"frame_id": 666, "monitor_ms": 800,
               "parameters": [{"name": "soc", "expression": "B4/2"}]}]
}
```

**The bounds (`min` / `max`, 2026-10-20, issue #1004).** A parameter's
range serves the dashboard (the gauge's scale) and the poller. A VW T6.1
with a full tank answered `41 2F FF`; the standard FuelTankLevel row
decoded it as `B2*0.3921568692` = 100.000001646, a hair above its max of
100 (the table's scale is a float printed with ten digits), and v6
dropped it: the dashboard went stale, MQTT kept the old value, the Test
button showed 100.000001646. The legacy firmware clamped a standard PID
to the table's range and only dropped a custom PID outside bounds the
user set. The rule now (`autopid_bounds.c`, pure): float noise never
drops a reading, a value beyond a bound by at most 1e-6 of the bound is
clamped to it whatever the row; beyond that a STANDARD OBD row clamps
(the table's range is its display range) and a custom, specific or
J1939 row drops the reading (its bounds are a filter: a profile's SOC
max 100 against a raw 0xFF, J1939's `FF..` not available). The table's
ranges were approximate for a quarter of its rows (a fuel trim reaches
99.22 against a max of 99, a narrowband sensor 1.275 V against 1, the
first wide-range sensor 8 V against 2); 33 of them now say what a full
scale decodes to, and the generator writes a percent row's scale as the
exact ratio (`B2*100/255`: 0xFF decodes to 100, not 100.000001646) and a
range below zero as a signed span (`[S2:S3]*0.25`, the evaporative
system vapour pressure). Rows stored by an earlier alfa keep their
expression and range: they clamp instead of dropping. Bench:
`tools/testbench/obd/autopid_bounds_bench.py` (`AUTOPID BOUNDS PASS`, the
ECU simulator's tank at 100 %).

`period_ms 0` on a PID inherits the group; group `period_ms 0` =
high-fidelity max-rate (entries round-robin as fast as the chip
answers: **~19 req/s total / 53 ms per request measured**, shared
across all period-0 entries; numbers + guidance in
[BENCHMARKS.md](BENCHMARKS.md)). Periods of 1–49 ms are accepted but
flagged (`stats.sub_floor_pids`). A group named `default` always
exists. **Expressions index the reply as the chip prints it with headers
on** (2026-10-24, issue #1014): every frame's bytes after the CAN id, the
ISO-TP framing bytes counted, so the RPM reply is `04 41 0C 1A F8` and
reads `[B3:B4]/4`, a long reply `10 3E 62 01 01 .. / 21 .. / 22 ..` counts
its `21`, `22` bytes, and a BMW reply behind extended addressing starts
with its address byte (`F1 10 09 62 ..`). This is the legacy firmware's
frame of reference (its `parse_elm327_response()` read the same lines), so
the published vehicle profiles go in verbatim. The parsers (VIN, DTC,
detection) and the cross-talk guard keep reading the framing-free payload
(`ap_resp_to_payload()`, B0 = the service echo); the expressions get
`ap_resp_frames()`. A `7F xx 78` response-pending frame is dropped (the
legacy firmware counted it, which is how the i3 and Mini SOC rows came to
read `[B11:B12]` for a SOC that sits at `[B6:B7]`). A reply that arrives
without ids (a K-line protocol, whose prelude keeps headers off) is
rebuilt into the same shape. `ATH0`, `ATS0` and `ATE1` in an init chain
are dropped, as the legacy firmware dropped them. **Filter expressions
index the frame DATA (B0 = first data byte, no id echo)**, also the legacy
frame-of-reference, and a `PGN:` row's B0 is the first data byte.

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
filter's CAN id arrives (or `monitor_ms` expires, 50..60000, default
1000), SPACE stop, restore receive-all (headers stay on, the CAN
prelude's state since 2026-10-24), PID inits replayed. ONE captured frame
updates all the filter's parameters. The
window **holds the chip**: other masters' requests fail fast while it
is open, so schedule filters sparsely (period_ms seconds apart, short
windows). Chip "<DATA ERROR"-style markers after the data bytes are
skipped (legacy semantics; bench-observed with injected frames).

**Protocol/width rule (bench-characterized 2026-07-06)**: the ATCRA
hardware filter only matches ids of the ACTIVE protocol's width, a
29-bit filter needs the chip on a 29-bit protocol (ATSP7/9) and an
11-bit filter needs 11-bit (ATSP6/8). Cross-width combinations are
accepted by the chip ("OK") but the window sees NOTHING and the filter
fails cleanly into backoff: set the protocol via the type init /
`std_protocol` to match your filters. (Monitor-all WITHOUT a CRA shows
both widths regardless of protocol; it is the CRA that is
width-bound.) On 29-bit protocols the monitor prints the id as four
byte tokens (`18 DA F1 10 …`), handled.

**Short frames**: the captured payload is the frame's REAL length
(DLC), and expressions are bounds-checked against it, an expression
past the end (`B6` on a DLC-2 frame) is skipped for that window
(never a garbage read; `null` in the API), while in-range parameters
on the same frame evaluate normally.

## Standard PIDs: scan once, store

`POST /api/autopid/std_scan` (UI button; prompt "ignition ON" first)
runs one chip job in three phases (`GET /api/autopid/std_scan` reports
the current one as `phase`):

1. `protocol`: which protocol AND which dialect answer (see "OBD
   dialects"). With `std_protocol` = "0" the job walks the ISO 15765-4
   protocols the bus guard allows for this bus (`6`, `7`, `8`, `9` on a
   silent bus; the two at the bus's bitrate on a live one), pins each
   with its `ATTP<n>` + functional header + `ATCRA` prelude and asks
   `0100`, then `22F400`. The first answer decides. A pinned setting
   (6..9) is asked in both dialects on that protocol alone. What first
   contact found a moment ago is tried before any of that. (Until
   2026-10-03 this phase was the chip's own search, `ATTP0` + `0100` +
   `ATDPN`: 9.3 s to give up when nothing answers `0100`, and blind to a
   UDS-dialect vehicle.)
2. `vin`: one bitmap request collects the responding ECU ids + their
   support bitmaps (the fingerprint; the CAN prelude has headers on since
   2026-10-24, nothing toggles them). Then the VIN: `0902` on an OBD-II car (ISO-TP joined by the
   chip, the lowest responder wins), falling back to `ATSH7E0`
   (`ATSH18DA10F1` on 29-bit) + UDS `22F190`; `22F802` asked of each ECU
   in turn on a UDS-dialect car, lowest id first. The functional header
   is restored afterwards.
3. `pids`: the support bitmap walk mapped onto the built-in SAE table
   into ready-made config rows (names, units, classes, **generated
   expressions** such as `[B3:B4]*0.25`). OBD-II: `0100/0120/../01A0`
   with headers on, multi-ECU OR-merged. UDS dialect: `22F400` ..
   `22F4E0` with headers on, every ECU's bitmaps kept apart, each row
   given to the lowest responder that has the PID. A PID the table knows
   by name only (no parameter to decode: `13`, `7A`, `85` ...) gets NO
   row: until 2026-10-03 it was polled all the same and every poll of it
   counted as failed.

The result goes to the vehicle store first (`autopid_vehicle_detected`:
a known car comes back with its own tables, an unknown car becomes a new
entry whose tables are these rows, see "Vehicle store"), then the table
to `/data/autopid/std_scan.json` (atomic). `GET /api/autopid/std_scan/result`
carries the table plus `protocol_detected` (the detected char, or the
pinned setting), `dialect`, `uds_protocol_id` (the `22F810` byte, when
a UDS-dialect vehicle answers it), `vin` ("" when none), `fingerprint`
("" when none) and, since the second pass, `key` / `known` / `name` of
the store entry; a UDS-dialect row carries `init` (its ECU). The
UI copies entries (or the full catalog at `/api/autopid/std_table`)
straight into the config PUT. The poller starts the job by itself once
per boot when first contact meets an unknown car; otherwise never without
the button. Polling pauses during the scan and resumes with its init
state replayed. **A new car's stored rows never poll by themselves
(2026-10-09, Ali: "it should not start polling unless the user enables the
standard PIDs and enables the PIDs they want"):** every row is stored
`"enabled": false`, one `I` line says so (`stored off: tick the ones to
read`), and nothing standard is read until the `std_enabled` switch is on
AND the user ticked rows, in the Quick Setup's picker (its Finish turns the
chosen rows on) or under Automate > Parameters (tick, Apply Configuration).
A device that never saw the wizard therefore stores what the car answers
and reads nothing until someone chooses. A known car's own tables are never
touched by a scan.

### No parameter name twice (2026-10-06)

Parameter names key the value cache and every API, so `ap_config_parse`
refuses a name that appears twice. The vendored SAE table gave one name to
two PIDs in 21 places (`OxySensorN_Volt` under 0x14..0x1B and 0x24..0x2B,
`OxySensorN_FAER` under 0x24..0x2B and 0x34..0x3B, `EngineFuelRate` 0x5E/0x9D,
`TurbochargerTemperature` 0x75/0x76, `EngineRunTime_AECD` 0x81/0x82,
`WWH_OBD_SysInfo` 0x90/0x91, `EGT_Sensor` 0x98/0x99), and the detection
stored its rows without validating them: a car answering both oxygen-sensor
sets (Ali's, 92 PIDs found) got a `config.json` its own loader refused at the
next boot (`E config file invalid: duplicate parameter name 'OxySensor1_Volt',
empty tables`, a `boot_errors` fault, nothing polled) and the wizard's PUT of
it was refused with the same words. Fixed three ways, each enough on its own:

1. **The table**: the wide-range rows are `OxySensorN_WR_FAER` /
   `OxySensorN_WR_Volt` (0x24..0x2B) and `OxySensorN_WRC_FAER` (0x34..0x3B,
   the current PIDs keep `OxySensorN_Crnt`); `EngineFuelRate_gs` (0x9D),
   `TurbochargerB_Temperature` (0x76), `EngineRunTime_AECD_6_10` (0x82),
   `WWH_OBD_SysInfo_2` (0x91), `EGT_Sensor_Bank2` (0x99); the row names
   follow. A car scanned before keeps its stored rows under the old names;
   only a new scan uses the new ones (Home Assistant entities of such a
   re-scan change name). The host suite walks the vendored array and fails on
   any repeat (`test_std_table_has_no_parameter_name_twice`).
2. **The scan** runs `ap_names_dedupe()` over its `supported` entries before
   they are stored (the result document and the car's rows alike): a device
   can never write a file its own loader refuses, whatever the table says.
3. **The loader repairs instead of emptying**: `autopid_config_load()` runs
   `ap_config_repair_names()` first; a file with a repeat gets the later name
   suffixed, one W line (`config file: N repeated parameter names made unique
   (_2, _3, ...) and the file rewritten`), and the repaired text written back
   once (change-guarded: a clean file is never touched), so the tables load,
   the boot stays clean and `GET /api/autopid/config` returns what the PUT
   will accept. The PUT itself stays strict.

The web UI's Quick Setup de-duplicates the same way before its Finish PUT
(the belt). Bench: `tools/testbench/system/autopid_names_bench.py`
(`AUTOPID NAMES PASS`).

## Vehicle store (Quick Setup, second pass, 2026-10-01)

One record per car, under `/data/autopid/` (FILES, not settings: they
describe cars, not the device):

- `vehicles.json`, the index: `{"version":1,"current":"<key>",
  "vehicles":[{"key","vin","fingerprint","name","protocol",
  "chip_protocol","profile","specific_init","ecus","std_supported",
  "pending_profile","first_seen","last_seen","scan_ts"}]}`, at most
  **8** entries. The key is the VIN, or `fp:<8 hex>` for a car without a
  readable VIN, and never changes once assigned (a `fp:` car whose VIN
  answers later keeps its key and gains the `vin`). `ecus` is the
  responder set behind the fingerprint (`7E8:BE7FB813,7E9:80000001`),
  kept so the SUBSET rule below has something to compare.
- `vehicles/<key>.json`, that car's PID tables in the `config.json`
  shape.
- `config.json` stays what the poller runs and every existing route
  serves: it is a COPY of the current car's tables. `PUT
  /api/autopid/config` (and the DBC add-to-filters merge) also lands in
  the current car's file through `autopid_config_save()`,
  change-guarded (identical bytes are not rewritten).

A first-pass `vehicle.json` (one car) is imported as the first, current
entry on the first boot and deleted; its tables are whatever `config.json`
held.

Pure, host-tested halves: `autopid_vehicle_core.c` (reply parsers,
fingerprint, prelude), `autopid_vehicle_index.c` (keys, find, the subset
rule, LRU, touch), `autopid_vehicle_codec.c` (the JSON). Target halves:
`autopid_vehicle.c` (the RAM copy under one lock, load/import, queries,
edits, first contact, the `apid_veh` worker) and
`autopid_vehicle_switch.c` (the files: snapshot, copy, reload, new car,
detection result, chip protocol save). Every file operation runs on an
INTERNAL stack (standard §2): the httpd and scan tasks directly, the
poller through the one-shot worker (6 KB internal stack, exists only while
work is pending). Writes are event-driven only: a detection, a switch, an
edit, an eviction, and the `last_seen` touch at most ONCE A DAY per car
(`ap_vidx_touch`, nothing when the clock is unset). A steady-state boot
with the same car writes nothing (§11).

**Identity.** A VIN decides alone. Without a VIN the fingerprint (FNV-1a
over the sorted responder ids + 0100 bitmaps) must match exactly, else the
**subset rule**: the same car when the main ECU (lowest id) answers with
the same bitmap on both sides and one responder set is a subset of the
other (an EV in accessory mode shows fewer ECUs than when ready, case H
of TASK_quick_setup.md). Behind a VIN match the stored responder set is
refreshed when it changed.

**First contact per boot** (the runner, after the first successful poll;
with empty tables an idle probe sends one `0100` every 10 s until the
check ran): the prelude, `0902`, `0100` (headers on), then
`autopid_vehicle_seen()`:

- the current car: `last_seen` touched (daily), a re-detected protocol
  recorded (case I); nothing else happens;
- another stored car: **switch** (below), event `autopid.vehicle_changed
  {vin, name, known:true}`;
- an unknown car: the detection job starts right there
  (`autopid_std_scan_start()`, pauses polling once) and stores it;
- nothing identifiable answered (a sleeping ECU behind a successful poll,
  case D): no change, retried on later successes up to 3 times;
- an ELM app holding the chip: the poller is not polling, so the check
  waits for the yield to end (case M).

**The switch** (automatic from first contact, by the detection job, or by
hand through `POST /api/autopid/vehicles/<key>/activate`; one code path,
`ap_veh_switch_files()`): (1) snapshot the live `config.json` into the
PREVIOUS current car's file (change-guarded), (2) copy the new car's file
over `config.json` (a missing file = the empty tables), (3) live reload
through `autopid_reload_config()`, the same path `PUT /api/autopid/config`
uses, (4) the runner's SPECIFIC type init becomes the car's
`specific_init` (the `specific_init` setting when the car has none) and
the boot prelude is re-armed so the next poll pins the car's protocol,
(5) `current` + `vehicles.json` written, (6) the event. From the poller
the RAM `current` flips at once and steps 1 to 6 run on the worker.

**A new car** (the detection job meets an unknown car): an entry with the
detected protocol, VIN or fingerprint, a default name (`"<WMI> <last 4 of
the VIN>"` or `"Car <4 hex>"`), `pending_profile:true`; the previous car's
tables are snapshotted and the new car's tables are ONLY the standard rows
the scan found (type `std`, group `default`; `"enabled": false` on every
row, `autopid_scan_rows.c`, 2026-10-09: the user ticks the ones to read),
written to `config.json` and
its file, reloaded live; event `{known:false}`. When the store is full the
least recently seen car that is not current goes first: one `W` line, its
file deleted, event `autopid.vehicle_evicted {vin, name}` (case J).

**Protocol policy.** `std_protocol` "0" = follow the store (the prelude
pins the current car's `protocol`, `ATTP0` search when there is none or
it stayed silent for 3 polls this boot); 6..9 = a manual pin that wins. The
chip learns the BASE protocol once: after a successful detection the job
calls `obd_chip_protocol_save(<detected>)` when the entry's
`chip_protocol` differs, then records `chip_protocol`. That is a REAL
`ATSP` on the driver's save path (the only command that bypasses the
EEPROM guard; the driver allows one per boot). Everything else on the
wire stays `ATTP`: the runner's baseline re-asserts the base RAM-only
after an ELM app had the chip, and mixed-protocol profiles (per-PID
`ATSP7` in init chains, the VW MEB family) keep being rewritten to `ATTP7`
by the guard, unchanged.

**Routes** (`components/HTTP_API.md` 6e4): `GET /api/autopid/vehicles`
(the index, every entry plus `"current":bool`, top-level `"max":8`),
`POST /api/autopid/vehicles/detect` (the detection job, 202 / 409; `POST
/api/autopid/std_scan` is the same job, `GET /api/autopid/std_scan`
reports `phase`, `/std_scan/result` carries `key`, `known`, `name`),
`PUT /api/autopid/vehicles/<key>` `{"name"?,"profile"?,"specific_init"?}`
(any subset; `profile` set clears `pending_profile`, `profile:""` = keep
the car without a profile and also clears its `specific_init`; the init
applies live when the car is current) returning the entry, `POST
/api/autopid/vehicles/<key>/activate` (`{"ok":true}`), `DELETE
/api/autopid/vehicles/<key>` (204; deleting the current car keeps
`config.json` and clears `current`). The `<key>` segment is percent-decoded
before the lookup (`ap_vidx_key_decode`, 2026-10-26, discussion #1018): the
web UI sends it through `encodeURIComponent`, so a `fp:` car arrived as
`fp%3A...`, matched nothing (404 `no such route`) and could be neither
forgotten nor given a profile; a VIN key was never affected. Found with
it: deleting the CURRENT car leaves the chip on the no-car prelude (the
chip's protocol search, `ATTP0`) until the next first contact or
detection; that prelude read `ATS1;ATH0;ATST96;ATTP0` and the chain
sender's print strip (issue #1014, for the user's inits) blanked its
`ATH0` into an empty command the chip answered with `?`, as it did the
K-line / J1850 preludes' deliberate `ATH0`: `ap_base_send_chain` (the
firmware's own chains: prelude, restore) now applies the EEPROM guard
alone and sends no empty element, `ap_base_send_init` (the tables' chains)
keeps the strip, and the no-car prelude is `ATS1;ATH1;ATST96;ATTP0`
(headers on, the frame of reference of the profiles: a car that answers no
standard request, a Nissan Leaf, never gets an entry and runs its profile
on this prelude with its init pinning the protocol; headers on is what it
had in practice). **Profile first (2026-10-26, Ali, after the same
discussion):** the detect route takes an optional body naming the car's
published profile (`profile`, `name`, `protocol`, `init`, up to three
`cmds`, each with its own init when the profile addresses one ECU per
request; `autopid_scan_probe.c` holds it, armed by the httpd task and
consumed once by the job): a new entry is stored under that name with the
profile attached, and a car that answers nothing standard (no 0100, no
22F400, no VIN: a Nissan Leaf) is confirmed by the profile's own first
request that answers positively, its responders being the ECU set the car
is keyed by (`confirmed_by: "profile"`, no standard rows, phase `probe`).
Every entry carries `faces` (other ECU-set hashes known to be this car, up
to four; the match tries the fingerprint, then any face, then the subset
rule) and `confirmed_by`; `POST .../<key>/merge {"into"}`
(`autopid_vehicle_merge`, "Same car as..." in the wizard's list) folds a
car seen without its VIN into the entry it belongs to. The identification
asks 0100 three times and unites the replies (`ap_veh_ecus_union`), and
the subset rule no longer needs the lowest id on both sides: a reply the
main ECU dropped out of is still the same car. `AP_VEH_INDEX_JSON_MAX` is
8192 (eight cars at every cap with four faces each fit, host-tested). The
first-pass `/api/autopid/vehicle*` routes are gone.

**The status summary (2026-10-26).** `autopid_vehicle_summary()`
(`include/autopid.h`) hands the composition root the current car in one
line (`name`, `key`, `vin`, `pending` = `pending_profile`, `cars` = how many
the store holds), which main serves as `vehicle` on `GET /api/status`: the
web UI's notice bar and the Dashboard say when the live tables belong to a
car whose setup never finished beside a set-up one. Born from Ali's report
of the day: a device set up on the simulator (two standard PIDs chosen) and
plugged into his car met a new car, which became current with its rows
stored off, and the dashboard went empty without a word while his two rows
stayed with the simulator's entry. The identity rule is unchanged: a
VIN-less contact with another bitmap is another car until the user folds
it in ("Same car as...") or sets it up. Proof: `autopid_newcar_bench.py`
(the fork on the bench, the bar in Chrome, the fold-in that brings the two
rows back) and `tools/webui_preview/probe_newcar.mjs`.

## Memory footprint (vehicle store, estimated)

Internal RAM: the `apid_veh` worker stack 6144 B (was 4096: it now runs
the tables copy + the config reload) + its TCB, static; the scan task's
6144 B stack is unchanged (it gained the store update + reload: check its
`std scan stack_hw` line on the bench). PSRAM `.bss`: the index copy
(8 entries x ~300 B = ~2.4 KB), the 6 KB `vehicles.json` scratch, the
runner's identity reply buffer `AP_RESP_MAX` = 1 KB and its 300 B
`ap_veh_seen_t`, the scan job's 1 KB reply buffer + 300 B seen record.
PSRAM heap, transient: one tables file (up to 256 KB) during a snapshot /
copy. Flash: `autopid_vehicle_*.c` + `autopid_http_vehicles.c`, roughly
12 KB of code. Flash writes: event-driven only (detection, switch, edit,
eviction, the daily touch); a steady-state boot with the same car writes
nothing. Measure with `idf.py size-components` and the `stack_hw` log lines
(`std scan stack_hw`, `vehicle writer stack_hw`) before release.

## External values (GPS, and future non-PID samples)

`autopid_publish_external(name, unit, value)` injects a named sample
that is NOT a polled OBD parameter into the same live cache. It joins
`autopid_snapshot()` (→ the HA `autopid_data` push + `${autopid.data}`),
fires the value sink (→ `data_logger`), emits `autopid.param` on change
(→ event rules, `group="external"`), and resolves through
`autopid_get_value()` (→ the dashboard), so one call reaches everywhere
polled parameters do, with no per-consumer work. The table holds 12
distinct names; a config reload does NOT clear it (external samples are
config-independent).

Today's producer: the ESPNetLink dongle's GPS, published by **main** from
`usb_acm_cli`'s GPS sink as `gps_latitude` / `gps_longitude` /
`gps_altitude` (m) / `gps_speed` (km/h) / `gps_heading` / `gps_satellites`:
only a live fix; last-known persists. autopid itself stays
USB/GPS-agnostic (the glue lives in main, like the data_logger sink).

`autopid_snapshot()` / `GET /api/autopid/data` is a flat `{name: value}`
object, the GPS keys sit right alongside the polled PIDs:

```json
{
  "VehicleSpeed": 62,
  "rpm": 1840,
  "coolant_temp": 89,
  "SOC_BMS": 74.5,
  "gps_latitude": -25.3444,
  "gps_longitude": 131.0369,
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
  "valid": true, "latitude": -25.3444, "longitude": 131.0369,
  "accuracy": 6, "altitude": 88.8, "speed": 17.1, "heading": 270.5,
  "satellites": 7, "age_ms": 1200
}
```

## Vehicle profiles (client-side import)

The UI/app fetches `vehicle_profiles.json` (GitHub), the user picks a
profile, and the client PUTs the converted table to
`/api/autopid/config` (+ `vehicle`/init strings in settings). The
firmware never fetches profiles. **The rules the importer applies**
(bench-verified against the published profiles):

1. **The expressions go in as published (2026-10-24, issue #1014).** The
   published profiles index the legacy frame of reference: every frame's
   bytes after the CAN id with the ISO-TP framing bytes counted
   (`parse_elm327_response()` in the legacy main/autopid.c read the chip
   with headers on): a single-frame reply is `0L 41 05 xx` (`010C` RPM =
   `[B3:B4]`), a multi-frame reply `10 LL 61 01 d0..d3 / 21 d4..d10 /
   22 ..` (the Kia Niro's SOC `B10/2` sits behind the `21`, the Leaf ZE1's
   `B39` and `B41:B42` skip `B40`, the PCI of the fifth consecutive frame),
   a BMW reply behind extended addressing starts with its address byte
   (`F1 10 09 62 ..`, the M340d odometer `[B13:B15]`). Since 2026-10-24
   the firmware reads every chip reply the same way (headers on for every
   CAN protocol, `ap_resp_frames()`), so nothing is converted. Until then
   the importer shifted every `B<n>`/`S<n>` index DOWN BY ONE, right for a
   single-frame reply and WRONG for a multi-frame one (the first frame's
   indexes two too high, every consecutive frame's one more):
   `tools/testbench/obd/autopid_profile_e2e_bench.py` measured it on the
   Leaf ZE1 profile (2026-10-21) and ConorIA's Kia Niro EV report (issue
   #1014) confirmed it in a car. A device that imported a profile before
   the change keeps the shifted rows until the car is imported again
   (release notes, 6.0.0-beta.3); its standard rows are re-numbered where
   the file is loaded (`ap_config_refresh_std()`: the table's current
   expressions by `cmd` and parameter name, one W line, the file rewritten
   once), because a car the store knows keeps its rows across detections.
2. **Cross-type inits need no restoring prelude any more** (2026-10-20,
   issue #964): a specific PID whose init switches protocol/headers/CRA
   (`ATSP7;…;ATCRA…`) leaves the chip there, and the runner itself puts
   the baseline back before the next row type ("The baseline back"
   above). A `std_init` that pins the protocol (`ATSP6;ATCRA;ATH0`, the
   old advice) still works; it is sent on top of the restored baseline,
   its `ATH0` dropped (the poller reads headers on; `ATS0` and `ATE1` are
   dropped the same way, as the legacy firmware did).
3. **Parameter names must be unique across the whole table** (the
   value cache and every API key on them; the firmware refuses the
   table with `duplicate parameter name 'X'`). Profiles are community
   data: three published Hyundai/Kia profiles label cell 158
   `HV_C_V_168` twice (2026-09-16), so the importer renames a repeat
   (`_2`, `_3` …, against the std/custom parameters already loaded)
   and tells the user instead of failing the import.
4. **Passive filters (2026-10-21)**: a profile's `filters` (the Leaf ZE1
   profile is the first published one) go to the table's `filters`: the
   frame id read like JS `Number()` (`"0x284"` is 644), `monitor_ms` as
   published (default 1000), `period_ms` 0 (the group's), group
   `default`, tagged `profile: true` so that the next import replaces
   them and hand-made filters stay; their expressions are NOT shifted (a
   filter indexes the frame data, B0 = first data byte, in both frames of
   reference). Bench-proven on the Leaf ZE1 profile: SPEED, the signed
   DM_F_RPM and ODOMETER decoded exactly from frames broadcast on 0x284,
   0x1DA and 0x5C5. Two importers do it: the web UI's `profileToPids()`
   (web_ui_v2 index.html: the Vehicle Specific pane, and the Quick
   Setup's Finish in setup.js) and the benches'
   `convert()` (`tools/testbench/obd/autopid_profile_bench.py`). The web
   UI's half reached the live tree only on 2026-10-27 (the 10-21 edit had
   sat in a stale clone): firmware up to 6.0.0-beta.4 imported a profile's
   PIDs and dropped its filters when picked from the web UI.

**Replies over 16 rows were assembled in the wrong order (found
2026-10-21, gone 2026-10-24):** with headers off the chip prints ISO-TP
row indexes `0:` to `F:` and wraps, and `assemble_headerless()` in
autopid_resp.c concatenated the rows by that printed index, so the rows
16 and up of a reply interleaved with 0 and up (the Leaf ZE1's 28-row
cell-voltage DID `2102`: 54 of 54 values wrong; the Toyota bZ4X `22181A2`
and the Xpeng `221122` are the two other published replies over 16 rows).
With headers on every frame is its own line in arrival order, and the
headers-off fallback takes the rows in arrival order too, the printed
index only telling a missing row.

`POST /api/autopid/test` = try a profile PID before saving (same
runner path: init chain, rxheader, expression, see HTTP_API §6e4).
Since 2026-09-16 it takes `type` (prepend the std/custom/specific init
chain, so the shot IS a poll of that PID) and `expressions[]` (decode
the one reply with every parameter, answered as `values[]`), and returns
a `transcript`: one `> cmd` / `< reply` line per exchange in the order
sent (type init, PID init, ATCRA, request, ATCRA off), capped per line.
The UI's Test modal shows it, so an init that parks the chip elsewhere
or a negative response (`7F 22 31`) is visible instead of a bare NO DATA.
`tools/testbench/obd/autopid_profile_bench.py <base>` PUTs every
published profile through this conversion (→ `AUTOPID PROFILE PASS`)
and polls a 24-parameter PID on the simulator: the regression for the
16-parameter cap and the duplicate-name typo.
`tools/testbench/obd/autopid_profile_e2e_bench.py` (2026-10-21,
`.\test.ps1 profilee2e`) plays ONE profile's ECU on the PCAN from the
profile itself (`actors/pcan_profile_ecu.py`: every request answered with
reply bytes the bench chose, every filter frame broadcast) and checks
every value the device publishes against what the profile's expressions
mean in the published frame of reference (→ `AUTOPID PROFILE E2E PASS`);
it is what found the two defects above.

## Tests

`host_test/` (run: `.\test.ps1 host autopid`, 146 tests): the frame of reference of a reply (`test_frames.c`, 9 cases, 2026-10-24, issue #1014: a stored config's standard rows re-numbered to the table's expressions, a custom row and an unknown command left alone, a second pass a no-op; the chip's own prints from the bench, an extended-addressed BMW SME with a pending frame and padding, decoded as the published rows mean (`[B6:B7]/10` = 72.6 %, the i3's `[B11:B12]` shown to count the pending frame); a Hyundai Ioniq 2021 BMS reply of nine frames from a Car Scanner log (`B10/2` = 75 %, `[B19:B20]/10` = 341.9 V); a Leaf AZE0 reply with its padding kept; the MEB 29-bit print of issue #914 taken as an id by the row's filter or the protocol hint and left alone without; headers-off replies rebuilt into frames, the 28-row reply in arrival order, a missing row stopping the assembly; the lowest responder; ATH0/ATS0/ATE1 dropped from a chain and the receive filter a chain sets; the standard table's numbering); the bounds on a decoded value and the SAE rows inside their own range (`test_bounds.c`, 4 cases, 2026-10-20: the report's 100.000001646 clamped to 100 on a standard and on a custom row, the reporter's controlled values, the tolerance, a J1939 not-available raw dropped, the exact ratio and the signed span, every ranged table row decoded at 0x00 / 0xFF / 0x7F / 0x80 through the real parser inside its range); what a chain of the tables sets on the chip and the chain that puts the baseline back (`test_chain.c`, 4 cases, 2026-10-20: the BMW row of issue #964, the published i3 / Mini / e-Ulysse chains, the off and reset forms set nothing, every undo command fits `AP_RESTORE_LEN`); the detection's rows as a new car's tables, on or off (`test_scan_rows.c`, 3 cases, 2026-10-09); the J1939 rows
(`test_j1939_rows.c`, 10 cases: the `PGN:` grammar and its refusals, the
config parser's typed fields and the `init` / `rxheader` refusal, the
scheduler's class-masked pick and the 20 ms passive floor, the built-in
table's expressions cross-checked against `j1939_spn_decode` over every SPN
and a not-available frame), the DM1 report fillers (`test_dtc_report.c`);
scheduler regression
(one-request-per-PID-per-period, group toggle/override, type gates,
round-robin, backoff, stagger), response-parser vectors (single frame,
SEARCHING noise, headers-on lowest responder, ISO-TP both header
modes, error lines), config parse happy/invalid, the yield-to-app window
predicate, the vehicle identity core (`test_vehicle.c`, 11 cases):
ATDPN shapes (`A6`, `6`, `A8\r>`, bare `A`, `?`, garbage), VIN from
0902 (single line, headers-off ISO-TP rows, headers-on multi-frame, two
responders, `NO DATA`, non-ASCII, excluded letters, the padded 15-char
bench transcript, the ECU simulator's `1WCAN0FW0P0000001`) and from
22F190 (incl. NRC 0x31), responder table + fingerprint determinism /
order independence / duplicate merge / FNV-1a reference vector,
effective protocol + prelude selection, the first-pass vehicle.json
import, the guard rule that an `ATSP7` inside a profile init chain goes
out as `ATTP7`; and the vehicle store index (`test_vehicle_index.c`, 10
cases): key derivation + validity + default names, find by key / VIN /
fingerprint, the SUBSET rule (accessory vs ready, another main ECU,
headers-off prints), the match policy over the situations table (VIN
decides, fp-only car adopted when its VIN appears, drift via subset,
nothing = no match), LRU eviction (never the current car, tie-breaks,
`current` re-indexed, a lone current car never evicted), the once-a-day
touch guard (clock unset, same day, a day later, clock backwards), the
responder-set text form, the `vehicles.json` round trip (escaping, the
`current` key, no per-entry flag in the file), a full 8-car index fits the
6 KB bound, and the bounds/sanitizing on load (no key, duplicate key,
bad VIN/protocol, derived keys, out-of-range numbers, the 9th+ entry
dropped); the bus guard (`test_bus_guard.c`, 11 cases: the verdict table,
"a verdict covers one protocol", the protocol an AT command sets in every
form the chip takes, the ruling on a chain of the tables, the commands that
reset the chip); the OBD dialects
(`test_dialect.c`, 17 cases, on reply texts captured from the chip): the
requests and the expression shift, the bitmap parser for every print shape
(11-bit, the 29-bit four-token id, headers off, a stale answer of another
range, the Sprinter's three ECUs), the VIN from `62 F8 02` (pending lines
first, two ECUs interleaved, a count byte), the per-ECU table and the owner
of a PID, physical addresses, `ATSH` detection in an init chain, the CAN
candidates per bus, the parser on 29-bit headers-on lines (and what it must
NOT take for an id), `7F xx 78` lines, the two-byte identifier check; and
the DTC report fillers (`test_dtc_report.c`, 3 cases). 107 tests in all.
Bench verification per TASK_autopid.md Phase 1, TASK_quick_setup.md and
TASK_j1939_wwh.md.

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

**Bench matrix (2026-09-06)**:
`tools/testbench/obd/autopid_matrix_bench.py [dut[:port]] [--sim 192.168.8.1]
[--sim-restore asis|on|off] [--ack-pcan PCAN_USBBUS2]` (PC or Pi, plain
HTTP; PCAN only for the optional ACK source of leg 5): standard
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
zeros at ~2.6 kHz plus 0x100 at ~7 Hz): the filter leg therefore
calibrates on whatever dominant frame the chip's `ATMA` shows before it
starts. The chip tags every monitored line `<DATA ERROR` (skipped, as
documented above). Last full run (2026-09-06): 48 checks PASS + 1 WARN.
Numbers: 4 std PIDs at 500 ms ≈ 7.4 polls/s; a period-0 hinted custom
RPM ≈ 17 updates / 5 s; HTTP p50 ≈ 60 ms under load through an ssh
tunnel; std / custom / specific = 13 / 31 / 8 updates per 10 s when all
three groups run together; every value correct at once (no
cross-attribution); group toggle and the per-type gates behave.

**OPEN FINDING: filters on a flooded bus** (the WARN): with the
simulator flooding one id, filter windows fail silently and often,
about 1 window in 3 missed at 1.7 kHz (0x0C0), about 4 in 5 missed at
2.6 kHz (0x1A0), even though the window's own id IS the flood. Every
failed window still holds the chip for `monitor_ms` plus the stop, so
standard polling beside two 3 s filters drops to 0.4–0.6 updates/s from
~1.4/s. No W/E line is logged (the stop always found the prompt; the
"no frame" path only logs at debug, which this build compiles out).
Once, a never-matching filter (0x123) captured a bogus frame (value 130
= the coolant byte 0x82): suspect a spliced monitor line matching via
the two-byte split-id shape. The same 2.8 k lines/s stream reaches an
`/ws/obd` client almost intact, so the suspect is the filter path's
16-chunk queue (drop-oldest → spliced lines that never parse), not the
chip. Real cars do not repeat one id at kHz rates, but a busy bus does
reach that aggregate; worth an instrumented build (count dropped
chunks + unparsed lines) before changing the window design. The bench
PCAN-USB FD is currently NOT on the DUT/simulator bus (channel 2 sees no
traffic), so the DBC / monitor-injection benches that need it cannot
run until it is re-wired.

**2026-10-02, what the "flood" is.** PCAN is back on the bus
(`PCAN_USBBUS2`) and measured it: the simulator does not broadcast at kHz
rates, it RETRANSMITS. While the chip monitors it is silent (no ACK), the
native controller is off, nothing else is on the bench bus, so the frame
is never acknowledged and the simulator's controller repeats it for ever
at line rate: 6868 frames of one id in 2 s with PCAN listen-only, 21 and
41 frames in 2 s (the configured 200 ms and 100 ms periods) as soon as
PCAN is a normal node that ACKs. The 0x0C0 frame also has seven data
bytes now (`0C 80 00 00 00 00 00`); the matrix bench's bus look sets
`ATCAF0` itself and accepts two bytes or more. `--ack-pcan PCAN_USBBUS2`
holds PCAN on the bus during leg 5 as the acknowledging node a car always
has. Result with it (10 frames/s on the bus): 4 filter updates and 8
standard-PID updates in 15 s, the same as on the saturated bus. So the
two halves of this finding separate: the polling starvation (8 updates
where about 30 are due) is the window design, it holds the chip for
`monitor_ms` plus the stop whatever the bus carries; the MISSED windows
belong to saturation (one saturated run: 1 filter update in 15 s). Last
full runs 2026-10-02: 50 checks PASS + 1 WARN, with and without the ACK
source; 4 std PIDs at 500 ms = 7.8 polls/s, period-0 custom RPM 17
updates / 5 s, 17 / 32 / 9 in the combination leg, HTTP p50 65 ms.
