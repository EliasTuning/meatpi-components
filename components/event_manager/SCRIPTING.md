# Scripting groundwork: what's ready, what's reserved, what UDS needs

> Status doc, 2026-07-07. The future scripting feature (meatpi: "I want to
> implement UDS scripting later") plugs into surfaces that ALREADY exist.
> This file is the contract: what a script engine may rely on, the
> constraints it must respect, and the exact gap list to close when the
> work starts. Design context: `TASK_event_manager.md` §8.

## 1. The scripting ABI is the event_manager registries (READY)

A script is a different rule body (`{"on":"…","script":"name"}` instead of
`{"do":"…","with":{…}}`) running against the SAME three registries rules
use. No component will ever grow a scripting dependency; scripts get no
privileged path.

| Surface | State | Where |
|---|---|---|
| Sources declare `{source, name, keys+types, description}` | READY (10 sources) | `event_manager_declare_source` |
| Actions register `{name, params JSON-schema, run(cJSON*, trigger)}` | READY (8 actions incl. `obd.request`, `autopid.group`, `http.post`, `mqtt.publish`, `logger.write`) | `event_manager_register_action` |
| Pull values `${comp.key}` resolved at fire time | READY (`${battery.voltage}`, `${time.iso}`, `${autopid.data}`, …) | `event_manager_register_value` |
| Call-by-name with cJSON params | READY, exactly how the rule engine invokes actions | `em_find_action()` (private header) |
| Event → JSON serializer | READY (feeds `/api/events/log`) | event_manager_http.c |
| Discovery for a UI/script editor | READY | `GET /api/events/sources\|actions\|values` |
| Script file storage | READY: `/data` via filesystem (streaming writes) + `/api/fs` upload/download | filesystem, api_http_fs |

## 2. Constraints a script engine MUST respect (decided by the v1 design)

1. **Not on the dispatcher task.** The event dispatcher is ONE 8 KB PSRAM
   task; actions must be quick and never touch flash. UDS flows block for
   seconds (multi-step, 0x78-pending waits): the script engine is its OWN
   component with its OWN runner task. If scripts read/write `/data`
   (script files, logs), that task follows the §2 corollary (internal
   stack, or marshal file I/O through filesystem's async writer).
2. **Event payloads are small by design** (`EM_KV_MAX`=6, `EM_STR_MAX`=48).
   A 4 KB ISO-TP response does NOT fit an event: big payloads flow through
   a DIRECT binding with a caller-owned buffer (see §3), never through the
   event queue. Events carry outcomes ("done", "value", "error"), not blobs.
3. **Engine choice stays swappable** (Berry / Lua / elk-JS: later
   decision). The binding layer (registries + obd) is engine-agnostic C.
4. **Same trust domain as settings.** Scripts can do what rules can do.
   When API auth lands (ARCHITECTURE §1b future), script CRUD sits behind
   the same gate: nothing special-cased.

## 3. UDS primitives that already exist (obd_chip)

The hard part of UDS scripting (a serialized, claim-based transport to the
vehicle) is done and bench-proven:

| Primitive | State | Notes |
|---|---|---|
| One-shot request/response | READY | `obd_chip_request(cmd, resp, len, timeout)`: claims COMMAND internally for one transaction |
| **Multi-step exclusivity** | READY | `obd_chip_claim(OBD_CHIP_CLAIM_COMMAND, timeout)` / `obd_chip_release()`: THE primitive for UDS sessions (diag session + security access + transfer must hold the bus across steps); `OBD_CHIP_CLAIM_EXCLUSIVE` exists for everything-paused flows |
| ISO-TP multi-frame, 4 KB both directions | READY (bench-proven vs MIC3624) | VT command syntax + flow-control quirks documented in `obd_chip/README.md` item 3 |
| Protocol/header/CRA control | READY | init strings (ATSP/ATSH/ATCRA…): the autopid Phase-5 lesson applies: cross-protocol inits must be self-restoring |
| Monitor mode (ATMA) claim | READY | `obd_chip` monitor claim (autopid filters use it) |
| HTTP one-shot with full init chain | READY | `POST /api/autopid/test {"cmd","init","rxheader","expression"}`: already does a complete UDS read (bench: VW MEB 29-bit SOC in one call) |
| Quiet-bus lever | READY (2026-07-07) | `POST /api/autopid/group` pauses polling groups at runtime: a UDS script claims quiet time the same way the bench does |
| Payload math | READY | expression_parser (`[Bx:By]` signed spans, bit ops) for decode; general math lives in the script language itself |

## 4. The gap list (the actual UDS-scripting work, when it starts)

1. **[DONE 2026-07-07 pm]** `script_engine` component skeleton: own runner task (queue of
   {script, trigger-event JSON}), engine embed (Berry/Lua decision),
   bindings: `event.emit`, `action.call(name, params)` (→ `em_find_action`),
   `value.get(name)`, `obd.*` (below), `sleep_ms`, KV scratch store.
2. **[DONE 2026-07-18]** `obd` script binding: shipped as
   `obd_claim(tx, rx[, ext])` / `obd_release()` (auto-release after EVERY
   run (normal end, error, kill) via the runner hook; the hold's hard
   time cap is the script runtime budget itself), `obd_request(hex[,
   timeout_ms])` returning the raw response hex (+ `uds_ok`/`uds_nrc`/
   `uds_pending` globals), `obd_isotp_tx(hex[, ms])`/`obd_isotp_rx([ms])`
   raw PDUs (new `uds_isotp_tx/rx` surface on uds_manager), and
   `uds_nrc_str(nrc)`. The 0x78 responsePending loop, NRC decode, and the
   tester-present keepalive were already uds_manager's (`uds_request` +
   `uds_session_begin/end`): the binding claims a session so TP + the
   transport hold cover the whole conversation. Core state machine =
   `script_engine_obd.c` (pure over an injected port, 12 host tests).
   LIVE-VERIFIED vs the bench ECU sim: VIN DID, NRC 0x31 decode,
   extended session surviving a 6 s sleep past the sim's 5 s S3 timeout
   (the TP proof), raw isotp 3E00→7E00, clean release.
3. **[DONE 2026-07-07 pm]** Script CRUD + run surface: `/api/scripts` (list/get/put/delete on
   `/data/scripts/*`, name-validated like cert sets) + `POST
   /api/scripts/<name>/run` for manual/UI-triggered runs + a `script` rule
   body in event_manager settings (schema bump) for event-triggered runs.
4. **[DONE 2026-07-07 pm]** Budget guards: max script runtime, max obd claim hold, instruction/
   memory quota (engine-dependent), and a kill switch (`/api/scripts/stop`).
5. **[DONE 2026-07-18]** Host tests: `script_engine/host_test` grew a
   12-test suite for the obd conversation core over a fake port
   (claim/extend/busy/release/auto-release, no-claim guards, outcome +
   NRC plumbing), plus a 7-test reflash suite (`test_reflash.c`) for the
   TransferData streamer (block framing, bsc wrap, CRC-32 check vector,
   NRC-stop, read-failure); suite total 25/25 PASS.

### 4b. Script-driven ECU reflash (DONE 2026-07-18)

A script can flash an ECU from a firmware file on the **SD card**:
- `obd_file_size("/sd/..")` → bytes; `obd_file_read("/sd/..", off, len)`
  → hex (SD-only paths, no `..`; `/data` rejected: internal-flash reads
  would fault the PSRAM runner stack, SD/SDMMC is cache-safe).
- `obd_transfer_file("/sd/..", off, size, block_len[, bsc])` → bytes sent
  (nil on fail); streams UDS `36 <bsc> <block>` C-side (no MB image
  through Berry strings), awaits each `76 <bsc>`, honors the ECU's
  advertised `maxNumberOfBlockLength`, accumulates CRC-32 into
  `uds_xfer_crc` (+ `uds_xfer_blocks`). The script owns the sequence
  (`0x27`/`0x2E`/`0x34`/`0x31`/`0x37`/`0x11`) around it.
- The 64-byte request cap on `obd_request`/`obd_isotp_tx` was lifted to
  4 KB (ISO-TP under it does 8192) so a real block fits.
- **Gate:** the `script_engine.allow_reflash` setting (default **false**,
  mirrors j2534) blocks `0x34/0x35/0x36/0x37` + `obd_transfer_file` from
  scripts unless explicitly enabled. Security access + DID writes stay
  ungated (diagnostics need them).
- Reference: `tools/testbench/reflash.be`. LIVE-VERIFIED vs the ECU sim:
  happy path (640 B / 5 blocks, CRC 0xCDD25DE2, checkMemory PASS, F195
  version bumped), gate-off blocks it, `verifyfail` → checkMemory fail
  caught, `progfail` → mid-transfer NRC caught.

### 4c. The editor + the engine's self-description (DONE 2026-09-07)

The web UI's Scripts page (Editor · Examples · Reference) is a real
editor: scripts are created, opened, edited, saved (`/api/fs/upload` to
`/data/scripts`), run, checked, downloaded and deleted in the browser;
CodeMirror 5 (highlighting, line numbers, brackets, autocomplete of the
bindings, Ctrl-S / Ctrl-Enter) is an on-demand library cached under
`cache/www` like uPlot; a plain textarea works until it is installed. The
firmware describes its own API: `GET /api/scripts/reference` (one entry
per binding: signature, group, returns, doc, example; the readable
globals; a Berry primer; hints keyed on error-message substrings; the
limits) and ships the example gallery (`GET /api/scripts/examples`, `?id=` for a
source: hello, obd_live, vin, uds_session, dtc_report, on_event,
can_frame: each run on the bench against the ECU simulator). `POST
/api/scripts/check` compiles without running. Run failures now carry the
exception type and Berry's traceback (`string:<line>:` → the editor's
jump-to-line links). The tables (`script_engine_doc.c`,
`script_engine_examples.c`) are pure, host-tested, and cross-checked
against the binding table at boot: adding a binding = one line in each.
Details: `script_engine/README.md`.

Everything in §1–§3 was built for other features and is already
bench-proven: none of it is speculative. The §4 list is deliberately the
WHOLE remaining scope: if a §4 item seems to need a change in §1–§3
surfaces, that's a design smell to raise first.

## 5. Landed 2026-07-07 pm (what closed §4 items 1/3/4)

- `script_engine`: Berry VM on a PSRAM runner task; bindings log/sleep_ms/
  millis/uds/uds_ext/can_tx/can_start/can_stop/can_rx/digitalWrite/digitalPWM/emit (+ 2026-07-08: dtc_scan/dtc_clear, the
  autopid DTC surface, gated by the dtc_enabled/dtc_allow_clear settings;
  TASK_dtc.md §9, and dtc_desc(code) → uploaded-database description,
  TASK_dtc_db.md); budget + kill. GET /api/scripts (list),
  POST /api/scripts/run {src|name}, POST /api/scripts/stop, `script run`.
- Event wiring: `script.run {name}` action + the `{"script":"name"}` rule
  sugar (parser rewrite, host-tested); trigger event exposed as evt_*
  globals; scripts emit the declared `script.done {value}` source.
- `uds.request {tx,rx,req,ext?}` action publishing `uds.response
  {ok,nrc,len,data,req}`: rules chain on UDS outcomes; payloads stay
  truncated per §2 (full payloads = a script's uds() binding).
- **HARD LESSON (extends §2.1): flash READS also assert from a PSRAM
  stack** (littlefs → esp_partition_read disables the flash cache; hit
  live via the dispatcher running script.run). script_engine marshals
  file loads through a one-shot INTERNAL-stack loader task when the
  caller's stack is external (esp_ptr_external_ram probe). Never read
  /data from the dispatcher or any PSRAM-stack task directly.
- Full chain bench-proven vs the PCAN ECU: manual run → VIN over UDS →
  emit script.done → sugar rule ran notify.be (evt_* correct) →
  uds.request 10 01 → 7F 10 11 decoded → log.note template rendered.
- Still open from §4: item 2 (obd.* script bindings: claim/release,
  isotp_tx/rx; today's uds() binding covers the isotp/obd transports
  via uds_manager) and item 5 (binding-layer host tests).
