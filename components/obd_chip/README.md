# obd_chip

## Summary

Feature component — the **single owner** of the external OBD chip (MIC3624,
ELM327 v2.3 dialect with `VT*` vendor commands) on UART1 (GPIO16 TX/GPIO15 RX,
2 Mbaud), plus its sleep pin (GPIO9), READY pin (GPIO7) and reset pin (GPIO41).
Nobody else touches that UART or those pins. Implements the task spec
(`../obd_chip_manager/TASK_obd_chip_manager.md`): one RX task fanning chip
output to subscriber queues, serialized TX, a claim model for the modal
protocol, a host-tested request→response engine, and the verified legacy
firmware-update flow.

## The broadcast contract (normative)

The manager does **no routing, filtering or parsing on the hot path** — every
subscriber receives ALL chip output (other components' responses, monitor
frames, unsolicited lines) and keeps or ignores what it wants. A full
subscriber queue drops that subscriber's chunk (counted, rate-limited WARN at
1/100) and never stalls the RX task or other subscribers. The command engine
itself is just another subscriber - one that subscribes for exactly the
duration of each `request()` (2026-09-08: a standing subscription filled
its 64-slot queue with every app line while idle, so an ELM app streaming
through a bridge for minutes drew a drop WARN every 100 chunks for
nothing). Do not add per-subscriber filters here.

**RX latency (2026-09-08):** the RX task blocks on the UART driver's
event queue and drains by buffered length, so a response is fanned out
as soon as the chip finished printing it (the FIFO rx-timeout, 10
symbol times = 50 us at 2 Mbaud). It used to poll `uart_read_bytes(128,
20 ms)`, which keeps waiting for "more" after the LAST byte until the
budget expires - every short response paid ~20 ms on top of the chip's
~3 ms (bench: a hinted `010C 1` over TCP cost 23-26 ms end to end; the
legacy firmware, event-driven, was in the single digits). Overflow
events (`UART_FIFO_OVF`/`UART_BUFFER_FULL`) flush the ring and log a
WARN; the idle wake (100 ms) drains too, so a lost event can never
strand bytes. Do not reintroduce a timed read on this task.

## Measured chip behaviour, large payloads and floods (2026-09-08)

Bench: `tools/testbench/obd/vt_large_bench.py` (PCAN memory ECU on
7E4/7EC with the simulator's ECU switched off; `pcan_memory_ecu.py`)
and `atma_flood_bench.py` (PCAN counter frames, `/api/can` as the on-bus
witness), both through the product path TCP:35000 → bridge → UART → chip.

- **Transmit, 4 KB**: `VTFullyRequestCk0FFF36<bsc><4064 B><CCCC>` (the
  MTPS-OBD tester's shape, `wican_pro_tester/MTPS-OBD+Log.TXT`) — the
  8 159-char line crosses socket → bridge → UART intact (bytes conserved)
  and the chip transmits the ISO-TP transfer in ~220 ms; the ECU verified
  every byte at 64/512/2048/4064 B. Its `7F 36 78` (pending) then `76 xx`
  come back as the tester sees them.
- **Receive, 4 KB**: a `23` read of 4094 B is delivered complete —
  headers ON: 586 raw frame lines (`7EC 10 FF 63 00 01 …`, `7EC 21 …`),
  11.7 KB of text, ~150 ms of frames + the `ATST` wait; headers OFF: a
  length line (`FFF`) then `0: 63 00 …` rows (7 B per row, one-hex-digit
  index that wraps), 14.6 KB with spaces. Byte-exact at the client, zero
  drops at every hop, `rx_max_chunk` 128.
- **The `1` "expected responses" hint ends a multi-frame response after
  its FIRST FRAME when headers are on** (64 B → one FF line, 2 KB → `NO
  DATA`). Hinted requests are for single-frame answers (the RPM case);
  never hint a long read. With headers off the hint counts messages.
- **Multi-frame assembly and automatic flow control exist only for the
  OBD physical range** (`ATSH 7E0–7E7`, responses 7E8–7EF). On 740/748
  the chip printed the First Frame raw and sent no FC — `ATFCSH`/`ATFCSD
  300000`/`ATFCSM1` did not change that on this chip. `ATAL` is accepted
  and harmless (the tester sends it).
- **ATMA flood**: 4000 frames/s (12 000 counter frames) printed at
  56 KB/s with every frame seen once, in order; no UART overflow, no
  fan-out drops, bytes conserved to the client. The limit that was found
  sat on the ESP side (one socket send per frame line) and is fixed by
  the raw-bridge coalescing in bridge_manager.

## Chunk format & memory choice

`obd_chunk_t { uint16 len; uint8 data[128] }` **by value** on the queues:
no pooling, no ref-counting, no lifetime bugs. Cost: 130 B per queue slot ×
depth, paid by each subscriber in PSRAM; fan-out is one memcpy per subscriber
per chunk. At 2 Mbaud worst case ≈ 1.6 k chunks/s — measured fine; revisit
zero-copy only with numbers.

## API (see include/obd_chip.h)

| Function | One-liner |
|---|---|
| `obd_chip_init/start/stop` | Lifecycle: driver+pins / **launch** the wake-reset-negotiate-ATE0-RX bring-up (async — see below) / stop task. `start` refuses unconfigured (§4.3). |
| `obd_chip_ready()` | True once bring-up finished with the chip answering (false in flight AND after give-up). |
| `obd_chip_subscribe/unsubscribe(q, name)` | RX fan-out registration (caller owns the queue, item = `obd_chunk_t`). |
| `obd_chip_dropped(q)` | Per-subscriber drop counter. |
| `obd_chip_send(data,len)` | Serialized raw TX (a 4 KB VT payload is one call); rejected during EXCLUSIVE. |
| `obd_chip_get_stats()` / `obd_chip_get_subscribers()` / `obd_chip_register_http()` | Observability (2026-09-08): wire + fan-out counters (`rx_bytes/chunks/max_chunk`, `rx_overflows`, `rx_buffered`, `tx_bytes`, claim, client idle) and per-subscriber `dropped/queued/depth`, served as `GET /api/obd_chip` (`obd_chip_http.c`, hand-formatted JSON). The first hop of any "missed frames" investigation: compare with `/api/bridges` and `/api/sockets`. |
| `obd_chip_client_touch()` / `obd_chip_client_idle_ms()` | The external-client activity clock (2026-09-08): the bridge glue's `obd` endpoint touches it on every app write (TCP/BLE/USB/WS), autopid reads the idle time and yields the chip while an app drives it (legacy `DEV_AUTOPID_ELM327_APP_BIT` parity, 10 s). NOT touched by `obd_chip_send` itself - autopid's own monitor sends go through that and would pause autopid against itself. `UINT32_MAX` = no client ever wrote. |
| `obd_chip_request(cmd,resp,len,timeout)` | One transaction: claims COMMAND, collects to the `>` prompt, strips echo+prompt. Refuses monitor-class cmds (`ESP_ERR_NOT_SUPPORTED`). |
| `obd_chip_claim/release(type,timeout)` | COMMAND / MONITOR / EXCLUSIVE arbitration. |
| `obd_chip_is_monitor_cmd(cmd)` | Table-driven monitor-class test (pure). |
| `obd_chip_monitor_stop()` | The stop byte: **SPACE, never CR** (CR = repeat-last-command → can re-enter ATMA). |
| `obd_chip_sleep(on)` / `obd_chip_status_ok()` | Sleep pin (hold survives resets; wake = release sequence) / READY pin. |
| `obd_chip_get_version(...)` | `VTVERS` → e.g. `MIC3624 V2.3.22`. |
| `obd_chip_firmware_update(fs_path, force)` | EXCLUSIVE; the verified legacy flow (below). |

## Async bring-up (2026-07-26 — Ali's boot-time ruling)

`obd_chip_start()` no longer blocks boot: the wake → conditional reset →
baud negotiation → `ATE0` → legacy provisioning → RX-task sequence
(~2 s typical, the biggest single step of the 4.1 s boot) runs on a
one-shot `obd_bringup` task (4 KB internal — same rule as the RX task —
prio 5, self-deleting, exit `stack_hw` logged per the stack-audit
convention). Consequences:

- **Wire-touching APIs gate on completion** so nothing interleaves with
  the bare-UART probes: `send()`/`monitor_stop()`/`firmware_update()`
  wait up to `OBD_BRINGUP_WAIT_MS` (20 s — past the worst hard-reset +
  baud-walk + provisioning case) then refuse `ESP_ERR_INVALID_STATE`;
  `request()` waits within the CALLER's own timeout and returns
  `ESP_ERR_TIMEOUT` without touching the wire (boot-window callers like
  the autopid poller simply retry). After the DONE flip the gate is a
  zero-cost flag test. `subscribe()`/`claim()` are pure state — ungated.
- `obd_chip_sleep(true)`/`obd_chip_hard_reset()` wait too (a sleep-entry
  race mid-bring-up would strand the sequence) but then proceed
  regardless — sleeping is the stronger intent.
- A **degraded start** (never called / refused unconfigured) leaves the
  gate OPEN: behavior is exactly the legacy pass-through (writes go out,
  requests time out naturally).
- The boot line's `obd=1` now means "bring-up launched"; the real outcome
  is the component's own `started: chip …` / `bring-up failed:` log line
  plus `obd_chip_ready()`. A bring-up failure logs `E` AFTER the boot
  health report has printed — it shows in the runtime error counters,
  not the boot-errors budget.
- If the task can't spawn, start falls back to the legacy synchronous
  path inline.

**Measured on the DUT 2026-07-26**: boot line 4.25 s → 2.27 s; bring-up
1992 ms on its task (`stack_hw` 1696 B of 4096); chip answers
`ELM327 v2.3` ready=1; boot HEALTH errors=0, faults=0; first post-reboot
TCP-35000 `ATI` returns clean `ELM327 v2.3` (no boot-window garbage).

## Bridge-endpoint ABI (TASK_obd_chip_manager_new §2/§3.6 — stable)

The `subscribe`/`unsubscribe`/`send` trio is this component's face to
`bridge_manager`: glue or `main` wraps it into a `bridge_endpoint_t` (three
one-line functions) — obd_chip itself never depends on bridge_manager.
`obd_chunk_t` is layout-identical to `bridge_chunk_t`/`socket_chunk_t` by
convention, so bridge queues carry it unmodified. **Treat the trio's shape
as a stable ABI**: every configured bridge (TCP, BLE, the USB port-B
passthrough — which is a `raw` bridge in bridge_manager, NOT code here)
depends on it. The test app's USB bridge is exactly this shape and predates
bridge_manager; production replaces it with a configured bridge.

## Claim model (task §5)

- `request()` claims COMMAND internally; concurrent requesters serialize
  (10 ms retry loop, caller timeout).
- MONITOR held ⇒ `request()`/COMMAND claims **fail fast**
  `ESP_ERR_INVALID_STATE` (**policy "manual"** — the settings key
  `monitor_policy` reserves `auto_interrupt`, pending meatpi's answer to task
  §11.6; nothing auto-sends the stop byte in v1).
- EXCLUSIVE (fw update): fan-out pauses (RX task discards), `send()` rejects.
- Raw bridges are ungoverned by nature: a user typing `ATMA` through a bridge
  without claiming makes `request()` callers see garbage-until-timeout, not
  corruption — the engine treats an unterminated window as `ESP_ERR_TIMEOUT`
  ("bus busy"). Bridges SHOULD claim MONITOR on their client's behalf.
- **Cross-requester (obd_gate, 2026-07-11)**: the claim model arbitrates
  the CHIP's users; the chip as a whole additionally holds the shared
  `obd_gate` while a command is in flight (`send()` acquires on a CR,
  the RX fan-out releases on the line-start `'>'`) so its conversations
  never overlap the ESP-side ELM engines on the same physical CAN bus.
  See `components/obd_gate/README.md`.

## Firmware update (verified legacy flow — do not improvise)

`VTVERS` (expect `MIC3624 …`) → `VTDLMIC3422` → `VTDLDT<line>` per record
(each answered `OK`; `?` = rejected) → stop at the `FFF1…` end marker →
`VTDLED` (2 s settle, 3 attempts) → hardware reset pulse (GPIO41 low 5 ms) →
rewake. Vendor images are `.txt` hex-record files (~3.5 k lines / ~490 KB).
Two sources: a `filesystem` path (`obd_chip_firmware_update`, read whole
into PSRAM) or the **packaged image** (`obd_chip_firmware_update_builtin` —
`EMBED_TXTFILES` of `obd_chip_manager/obd_fw/V2.3.22.txt`, byte-identical
to legacy `main/obd_fw`; +483 KB app size). Update responses DO end with
the `>` prompt (bench-verified): byte-level echo-skip, collect until `>`,
then classify `OK`/`?` (`obd_chip_fw.c`). Returning early on `OK\r` without
waiting for the prompt makes the next record race the chip and get dropped.

**Auto-update (legacy parity, Ali ruled 2026-07-26):** with the
`auto_update` setting on (default), the bring-up task runs the builtin
update with `force=false` after every bring-up — a chip already at the
packaged version costs one `VTVERS` and skips; a different version is
flashed (~30 s, EXCLUSIVE — consumers see timeouts meanwhile, log lines
mark start/progress/done); a chip stuck in download mode (`?`, which
also fails bring-up) is recovered by completing the download. Stricter
than legacy in ONE case: a `VTVERS` **timeout** (dead wire) aborts
instead of streaming records into 5 s timeouts. Bumping the packaged
version = new `EMBED_TXTFILES` path + `OBD_FW_BUILTIN_VERSION` in
`obd_chip_fw.c`.

## Settings (`"obd_chip"`, version 3)

The UART baud is **not a setting**: `OBD_CHIP_BAUD` (2 Mbaud) in
`obd_chip_private.h`. meatpi 2026-09-07: exposing it invited a user changing
it, breaking the chip and claiming warranty; v3 drops a stored `baud` on
migration. The same decision took **every** `obd_chip` field out of the web
UI (they only confused users) — the group stays reachable through the
settings API / CLI / backup for the future sleep_manager work.


| Key | Type | Default | Notes |
|---|---|---|---|
| `auto_sleep` | bool | `false` | reserved (the future sleep_manager arms the chip's controls) |
| `auto_update` | bool | `true` | flash the packaged chip fw (V2.3.22) on version mismatch after bring-up — legacy parity |
| `monitor_policy` | enum manual/auto_interrupt | `manual` | `auto_interrupt` reserved (§5 open question) |
| `wake_voltage_mv` | int 12000–15000 (must exceed `sleep_voltage_mv`) | `13500` | chip VL wake threshold, provisioned at boot (`STSLVLW`) |
| `sleep_voltage_mv` | int 12000–14000 | `13200` | chip VL sleep threshold, provisioned at boot (`STSLVLS`) |
| `sleep_time_min` | int 1–60 | `2` | chip VL sleep hold; programmed as `min*60+30` s (the ESP sleeps first — legacy guard) |

## Boot provisioning (task §11.5 — ANSWERED by meatpi 2026-07-04: "follow legacy init")

`start()` after `ATE0`: read `STSLCS` (pure parser `obd_chip_stslcs.c`,
grammar = the legacy `main/obd.c` sscanf patterns, fixture = a real
MIC3624 capture) and, ONLY when something differs, rewrite the chip's
stored sleep config: ensure NATIVE control mode (`ATPP 0E SV 7A`+`ON`),
program the VL thresholds/time from settings with every autonomous
control left **OFF** (`STSLVLW`/`STSLVLS`/`STSLVl off,off`/
`STSLU off, off` — byte-exact legacy strings), persist the 2 M power-on
baud (`ATPP 0F SV 95`+`ON`, only when `baud=2000000`), `STSLUIT 1200`,
then `ATZ` + re-bring-up (baud restore + `ATE0`). A matching chip skips
everything — verified live: provisioning ran once after flashing, the
next boots go straight to `started`.

**Framing lesson (found live by STSLCS):** the `>` prompt terminates a
response only at LINE START — `STSLCS` data contains mid-line `>`
(`VL_WAKE: OFF, >13.50V`), which used to truncate the accumulator and
trigger a reprovision-every-boot loop (`obd_parse_feed`, host-tested).

## Open stubs (task §11 — ask meatpi)

1. ~~**READY pin semantics**~~ **ANSWERED 2026-07-04** (legacy
   `elm327_chip_get_status`, reference copy in
   `../obd_chip_manager/elm327.c`): the pin is **ACTIVE LOW** —
   LOW = awake/ready (`ELM327_READY = 0`), HIGH = asleep. The earlier
   "reads 0 while answering" observation was correct behavior; the v6
   polarity was inverted and is fixed (`obd_pin_ready`,
   `obd_chip_status_ok`).
2. ~~Full **chip init sequence**~~ **ANSWERED 2026-07-04** (meatpi:
   "follow legacy init") — see *Boot provisioning* above. Boot now also
   follows the legacy `elm327_init` flow: RESET pin is an OPEN-DRAIN
   output; reset is CONDITIONAL (READY says awake → soft `ATZ`,
   asleep/stuck → hardware pulse); `STWBR` persists a successful `STSBR`
   switch as the chip's power-on baud; fw ≥ V2.3.22 gets the power-pin
   check (`VTPPSWS` → `VTPPSW10` + reset when wrong); a READY-wait loop
   (10 × 200 ms, then one forced reset) gates the bring-up; sleep parks
   BOTH the SLEEP and READY pins (pulldown + RTC pulldown + hold).
   UART ring buffers: legacy 18 K / 18 K, and they live in **PSRAM** in
   both firmwares — the driver allocates them with `MALLOC_CAP_DEFAULT`
   (`UART_ISR_IN_IRAM` off) and the SPIRAM malloc policy routes that to
   PSRAM. No DMA is involved (the ISR copies the 128 B hardware FIFO
   into the ring), so PSRAM placement is safe exactly as it was in
   legacy.
3. ~~`VT` 4 KB ISO-TP command syntax~~ **ANSWERED + bench-verified
   2026-07-03** (meatpi's vector, 4095 bytes reassembled byte-exact on PCAN;
   cross-checked against the WiCAN Pro tester project —
   `wican_pro_tester/protocol_notes.md` + `MTPS-OBD+Log.TXT`):
   `VTFullyRequestCk<LLLL><data hex><CCCC>[ <n>]` — LLLL = byte count (4
   hex, max 0FFF), CCCC = 16-bit additive checksum of the payload bytes,
   optional space-separated `<n>` = the standard ELM "expected number of
   responses" hint (the tester logs use the same form on plain requests:
   `22F106 1`). The chip transmits on the current header (0x7DF default;
   the tester uses `ATSH7E0` on CAN channels and J1850 3-byte headers like
   `ATSHC4D0F5` on VPW), REQUIRES a prompt Flow Control from the response
   ID on CAN (0x7E8 for 0x7DF; ignores the ECU sim's FC from request−8 =
   0x7D7; answers `FC RX TIMEOUT` otherwise), then prints EACH ECU response
   message — headers shown per the header setting, including `7F xx 78`
   response-pending interims — or `NO DATA` if none, then the `>` prompt.

   **Real-world transcript (tester log, channel 2 = J1850 VPW `AT SP2`,
   UDS `0x36` TransferData — the production use case is ECU flashing):**

   ```
   >ATSHC4D0F5                              <- J1850 prio/target/source
   OK
   >VTFullyRequestCk03FF3600...<1023 B>...1CA8 2
   C4F5D07F3678C0        <- response w/ header: 7F 36 78 = pending
   C4F5D0760022          <- final positive response 76 ...
   ```

   (The `IT CH_NO <n>` + `OK` lines that follow in the raw log are NOT
   chip output — that is the MTPS-OBD **Bluetooth tester box** being told
   to switch channels for the next test step; see
   `wican_pro_tester/protocol_notes.md`. The VT response ends with the ECU
   messages + prompt, exactly as reproduced on our bench.)

   Repeatable check: `tools/testbench/obd_vt_isotp_check.py [--size N]` —
   the PC side is a real ISO-TP stack (python-can + can-isotp `CanStack`),
   so FC, sequence numbers and reassembly are standards-checked; byte-exact
   with zero stack errors at 7/100/4095 bytes. Goes through plain
   `obd_chip_request()`/`send()` — no component changes were needed.
4. Authoritative **monitor-class command list** (table currently
   ATMA/ATMR/ATMT/STM/STMA in `obd_chip_parse.c`).
5. `monitor_policy=auto_interrupt` behavior.

## Dependencies

`esp_driver_uart`, `esp_driver_gpio`, `settings_manager`, `log_manager`,
`filesystem` (fw file reads) — all private. Init order: after the core
managers; `settings_manager_start()` before `obd_chip_start()`.

## Memory footprint (estimated — measure before release)

| Where | What | Size |
|---|---|---|
| Internal | UART driver RX 8 KB + TX 4 KB (`// internal: DMA`), RX-task stack 16 KB (`// internal: UART driver path`), TX mutex/TCB | ~29 KB |
| PSRAM `.bss` | subscriber registry, cmd-engine queue (64×130 B) + accumulator (16 KB since 2026-09-08 — a 4 KB ISO-TP response is 8–12.5 KB of hex; the 4 KB accumulator truncated it) + bare_probe accumulator (16 KB), config | ~41 KB |
| PSRAM heap | fw image buffer during update only | ~500 KB transient |

## Tests

- **Host (`host_test/`, 10 tests):** pure framing — prompt splits at every
  chunk boundary, echo strip, post-prompt bytes not consumed, noise
  interleave, **meatpi's real 36-line proprietary-PID log replayed at chunk
  sizes 1…4096**, overflow flagging, error/monitor classification, fw-file
  iterator + FFF1 marker.
- **On-target (`test_apps/`, live bench):** all green 2026-07-03 — see
  `test_apps/README.md` for markers and the bench doc pointer.

## EEPROM guard (2026-09-16)

The chip persists a few settings to EEPROM and EEPROM has a write budget:
`ATSP`/`ATSPA` (protocol), `ATM1` (memory on: every later protocol change
sticks), `ATPP xx SV/ON/OFF` (programmable parameters; 0C/0F re-baud the UART
= a bricked link), `ATSD` (store data byte), `ATCV` (voltage calibration),
STN `STWBR`/`STSAVCAL`. Init strings replay per poll transition, apps re-send
`ATSP` on every connect, terminal users type anything, so the guard lives in
the driver, on every TX path (`obd_chip_guard.h`, pure, host-tested in
`obd_chip_parse.c`):

- rewritten in place, same length: `ATSP..` -> `ATTP..`, `ATM1` -> `ATM0`;
- refused: `ATPP` (except `ATPPS`, the summary read), `ATSD`, `ATCV`, `STWBR`,
  `STSAVCAL`. `obd_chip_request()` returns ESP_ERR_NOT_SUPPORTED; the raw
  bridge path (`obd_chip_send`) drops the chunk and fans `?` + prompt back so
  the app or terminal sees the ELM rejection and the chip never sees the write.

A token counts only at a command boundary (start, CR/LF/space/tab/';'), so
`DATA`, the `ST` inside `ATSTFF` and hex payloads never match. Counters:
`GET /api/obd_chip` -> `eeprom_guard.rewrites` / `.blocked`; refused commands
log at W with the text. autopid's `ap_init_sanitize()` is a wrapper over this
guard and its config parse refuses a refused command in `cmd`/`init`; the UDS
AT transport sends `ATTP` itself. Exempt by construction: boot provisioning
(`bare_probe`: STSL*, `ATPP 0E/0F`, `STWBR`, once, behind a matching check) and
the firmware update flow, which write the UART directly.

## Transaction hold (2026-09-16)

`obd_chip_txn_begin(timeout)` / `obd_chip_txn_end()`: the calling task takes the
COMMAND claim across several `obd_chip_request()` calls (they nest; other
requesters wait at their claim timeout, autopid's poll included). Born from the
UDS AT transport: 7 setup commands + the request, and autopid's poll used to
land between them (bench: 1 of 3 UDS requests survived with polling, 5 of 5
with autopid paused). `end()` drops the hold whatever the nesting depth.
