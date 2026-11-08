# obd_chip

## Summary

Feature component: the **single owner** of the external OBD chip (MIC3624,
ELM327 v2.3 dialect with `VT*` vendor commands) on UART1 (GPIO16 TX/GPIO15 RX,
2 Mbaud), plus its sleep pin (GPIO9), READY pin (GPIO7) and reset pin (GPIO41).
Nobody else touches that UART or those pins. Implements the task spec
(`../obd_chip_manager/TASK_obd_chip_manager.md`): one RX task fanning chip
output to subscriber queues, serialized TX, a claim model for the modal
protocol, a host-tested request→response engine, and the verified legacy
firmware-update flow.

## The broadcast contract (normative)

The manager does **no routing, filtering or parsing on the hot path**: every
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
  MTPS-OBD tester's shape, `wican_pro_tester/MTPS-OBD+Log.TXT`), the
  8 159-char line crosses socket → bridge → UART intact (bytes conserved)
  and the chip transmits the ISO-TP transfer in ~220 ms; the ECU verified
  every byte at 64/512/2048/4064 B. Its `7F 36 78` (pending) then `76 xx`
  come back as the tester sees them.
- **Receive, 4 KB**: a `23` read of 4094 B is delivered complete,
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
  the chip printed the First Frame raw and sent no FC: `ATFCSH`/`ATFCSD
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
per chunk. At 2 Mbaud worst case ≈ 1.6 k chunks/s, measured fine; revisit
zero-copy only with numbers.

## API (see include/obd_chip.h)

| Function | One-liner |
|---|---|
| `obd_chip_init/start/stop` | Lifecycle: driver+pins / **launch** the wake-reset-negotiate-ATE0-RX bring-up (async, see below) / stop task. `start` refuses unconfigured (§4.3). |
| `obd_chip_ready()` | True once bring-up finished with the chip answering (false in flight AND after give-up). |
| `obd_chip_subscribe/unsubscribe(q, name)` | RX fan-out registration (caller owns the queue, item = `obd_chunk_t`). |
| `obd_chip_dropped(q)` | Per-subscriber drop counter. |
| `obd_chip_send(data,len)` | Serialized raw TX (a 4 KB VT payload is one call); rejected during EXCLUSIVE. |
| `obd_chip_get_stats()` / `obd_chip_get_subscribers()` / `obd_chip_register_http()` | Observability (2026-09-08): wire + fan-out counters (`rx_bytes/chunks/max_chunk`, `rx_overflows`, `rx_buffered`, `tx_bytes`, claim, client idle) and per-subscriber `dropped/queued/depth`, served as `GET /api/obd_chip` (`obd_chip_http.c`, hand-formatted JSON). The first hop of any "missed frames" investigation: compare with `/api/bridges` and `/api/sockets`. |
| `obd_chip_client_touch()` / `obd_chip_client_idle_ms()` | The external-client activity clock (2026-09-08): the bridge glue's `obd` endpoint touches it on every app write (TCP/BLE/USB/WS), autopid reads the idle time and yields the chip while an app drives it (legacy `DEV_AUTOPID_ELM327_APP_BIT` parity, 10 s). NOT touched by `obd_chip_send` itself - autopid's own monitor sends go through that and would pause autopid against itself. `UINT32_MAX` = no client ever wrote. |
| `obd_chip_settings_generation()` / `obd_chip_settings_changed()` | The chip's run-time settings generation (2026-10-28): a number that moves at every bring-up (boot, a reflash's fresh start, a recovery) and when a holder that changed the chip's volatile settings (echo, headers, protocol, timeouts, filters) says so at the end of its transaction (the CAN self-test's `ATD` + `ATE0`). AutoPID's baseline memory compares it before every poll and re-sends its prelude when it moved (bench 2026-10-28: the chip echoed after a self-test and AutoPID's memory did not know). Any holder that keeps chip state should do the same. |
| `obd_chip_request(cmd,resp,len,timeout)` | One transaction: claims COMMAND, collects to the `>` prompt, strips echo+prompt. Refuses monitor-class cmds (`ESP_ERR_NOT_SUPPORTED`). |
| `obd_chip_protocol_save(proto)` | (2026-10-01) ONE real `ATSP<proto>` per boot on the request path, bypassing the EEPROM guard: the chip learns its base protocol once after a vehicle detection. Caller: autopid's detection job only. `'1'..'9'`/`'A'..'C'` else `ESP_ERR_INVALID_ARG`; a second call this boot `ESP_ERR_INVALID_STATE`; no `OK` = `ESP_FAIL`. Counted in `protocol_saves`. See "Protocol save" below. |
| `obd_chip_claim/release(type,timeout)` | COMMAND / MONITOR / EXCLUSIVE arbitration. |
| `obd_chip_is_monitor_cmd(cmd)` | Table-driven monitor-class test (pure). |
| `obd_chip_monitor_stop()` | The stop byte: **SPACE, never CR** (CR = repeat-last-command → can re-enter ATMA). |
| `obd_chip_sleep(on)` / `obd_chip_status_ok()` | Sleep: the sleep word `STSLEEP0` first (2026-11-07, 4.x parity: see "The sleep word" below), then the sleep pin (hold survives resets); wake = the release sequence / READY pin. |
| `obd_chip_park()` | (2026-10-05) Leave the chip asleep from a boot that never initialised this component (the firmware's crash park). Pins only. The chip meets a boot in one of two states: **awake** (a power-on, or the run before had it up), then it is hardware-reset first (a session or a monitor left open is closed, and a chip mid-monitor ignores the sleep pin) and put to sleep with the hold, about 0.6 s; or **asleep** (the boot follows a sleep and the sleep pin's hold is still on: READY reads high), then it is left asleep and its sleep pin is never driven high. Either way the next normal boot finds it asleep and wakes it as after any sleep. |
| `obd_chip_get_version(...)` | `VTVERS` → e.g. `MIC3624 V2.3.24`. |
| `obd_chip_firmware_update(fs_path)` / `obd_chip_firmware_update_builtin(rule)` | EXCLUSIVE; the verified legacy flow (below). No forced rewrite since 2026-11-01 (Ali): a chip is written only when the image's version is above the one it reports. A file's version is its NAME (`V2.3.24.txt`; `ESP_ERR_INVALID_ARG` without one); the packaged image takes the rule, `OBD_FW_RULE_STARTUP` (the bring-up: a chip on the v4.52p accept list, V2.3.22 or V2.3.24, is left alone) or `OBD_FW_RULE_NEWER` (the user's entries). |
| `obd_chip_firmware_update_start(fs_path)` | (2026-10-28) the user's update (`OBD_FW_RULE_NEWER`) on its own task, NULL = the packaged image; `ESP_ERR_INVALID_STATE` while one runs, `ESP_ERR_INVALID_ARG` for a file whose name carries no version; a reflashed chip comes back through a fresh bring-up. The console / HTTP entry. |
| `obd_chip_update_in_progress()` / `obd_chip_fw_status()` | (2026-10-28) the update's state: `running`, `lines`/`total` records, `source`, `done_once` + `last_err`. The sleep entry waits on the first, which is true for the whole update JOB since 2026-11-08 (the stream and the fresh bring-up after it: a sleep pin pulled during that bring-up resets the chip); sleep_manager also pushes a running countdown out while it is true. |
| `obd_chip_probe(cmd, baud, timeout, rx, len, &got, &prompt)` | (2026-10-28, issue #1023) bare-wire diagnostic: send one command at a baud and return every byte that came back, prompt or not; EXCLUSIVE meanwhile, the UART goes back to its baud. The `obd_chip probe` command. |
| `obd_chip_active_baud()` | The baud the chip answered at during bring-up (0 = never). |
| `obd_chip_register_cli()` | The `obd_chip` command (standard §6b, self-registered on settings apply behind `cli`). |
| `obd_chip_probe_run(kind, baud, seconds, cmd, out)` / `obd_chip_probe_start(...)` / `obd_chip_probe_last(out)` / `obd_chip_probe_bauds(&n)` | (2026-10-28) the wire tools as one runner (`obd_chip_probe.c`): probe (ATI + VTVERS at 2 Mbaud and 115200 or one baud), scan (ATI at every standard baud), auto (a CR + ATI at the UART's baud or at `baud`, the hardware bit-time measurement, then a listen where it says), listen (send nothing). `cmd` (NULL or "" = ATI; one printable word under `OBD_PROBE_CMD`) replaces ATI in the first three: `VTDLMIC3422` asks a chip whose application is gone for its bootloader, and a probe with it skips VTVERS. The console runs it in place and prints; `POST/GET /api/obd_chip/probe` start it as a job and serve the result. |
| `obd_chip_report_run(out)` | (2026-10-28, Ali) the chip configuration report: ATI, AT@1, AT@2, VTVERS, ATRV, ATDPN, ATDP, ATCS, ATPPS (the EEPROM-backed programmable parameters), STSLCS (sleep and wake), VTPPSWS, in one transaction through the request engine. `obd_chip report`, `GET /api/obd_chip/report`, the page's copyable block. |
| `obd_chip_bringup_wire(out, max)` | (2026-10-28) what the last bring-up received at each baud it tried and got no prompt from (bytes + the first of them in hex): the banner of the page and `obd_chip` on a chip that is down. `obd_chip_fw_status_t.chip` = the chip's VTVERS as last read, for the same page. |
| `obd_chip_claim/release`, `obd_chip_txn_begin/end` | The claim model, in `obd_chip_claim.c` since 2026-10-28 (the 700-line rule; `obd_core_claim_state()` names the claim for the stats). |

## Async bring-up (2026-07-26: Ali's boot-time ruling)

`obd_chip_start()` no longer blocks boot: the wake → conditional reset →
baud negotiation → `ATE0` → legacy provisioning → `ATM0` → RX-task sequence
(~2 s typical, the biggest single step of the 4.1 s boot) runs on a
one-shot `obd_bringup` task (4 KB internal (same rule as the RX task)
prio 5, self-deleting, exit `stack_hw` logged per the stack-audit
convention). Consequences:

- **Wire-touching APIs gate on completion** so nothing interleaves with
  the bare-UART probes: `send()`/`monitor_stop()`/`firmware_update()`
  wait up to `OBD_BRINGUP_WAIT_MS` (20 s, past the worst hard-reset +
  baud-walk + provisioning case) then refuse `ESP_ERR_INVALID_STATE`;
  `request()` waits within the CALLER's own timeout and returns
  `ESP_ERR_TIMEOUT` without touching the wire (boot-window callers like
  the autopid poller simply retry). After the DONE flip the gate is a
  zero-cost flag test. `subscribe()`/`claim()` are pure state: ungated.
- `obd_chip_sleep(true)`/`obd_chip_hard_reset()` wait too (a sleep-entry
  race mid-bring-up would strand the sequence) but then proceed
  regardless: sleeping is the stronger intent.
- A **degraded start** (never called / refused unconfigured) leaves the
  gate OPEN: behavior is exactly the legacy pass-through (writes go out,
  requests time out naturally).
- The boot line's `obd=1` now means "bring-up launched"; the real outcome
  is the component's own `started: chip …` / `bring-up failed:` log line
  plus `obd_chip_ready()`. A bring-up failure logs `E` AFTER the boot
  health report has printed: it shows in the runtime error counters,
  not the boot-errors budget.
- If the task can't spawn, start falls back to the legacy synchronous
  path inline.

**Measured on the DUT 2026-07-26**: boot line 4.25 s → 2.27 s; bring-up
1992 ms on its task (`stack_hw` 1696 B of 4096); chip answers
`ELM327 v2.3` ready=1; boot HEALTH errors=0, faults=0; first post-reboot
TCP-35000 `ATI` returns clean `ELM327 v2.3` (no boot-window garbage).

### The reset is verified (2026-10-03)

The sequence lives in `obd_chip_bringup.c` (split out of `obd_chip.c`, which
keeps the task and the gate). An awake chip is reset by command, and until
this date one `ATZ` with a prompt after it counted as done. On a software
restart of the ESP the chip usually answered that first command with a
prompt and NO reset (what the last run left in its line buffer, or a glitch
of our TX pin, sat in front of it): saved console logs show bring-up in
852 ms on 15 of 30 boots and 2096 ms on the other 15, power-on boots always
2096 ms. The chip then kept everything the previous run had set: echo,
headers, filters and an established CAN session.

That last one is not cosmetic. A chip holding a session at 500 kbit/s is a
node at 500 kbit/s; on a bus at another bitrate it answers every frame with
an error flag (bench: about 450 error frames a second on a 250k bus, the
sender driven to bus-off, with the firmware's own CAN node off and autopid
disabled). `ATPC` or a real reset stops it; a freshly reset chip is quiet
until its next request, whatever its default protocol is.

`chip_soft_reset()`: one throw-away `ATI` (whatever sits in front ends with
it), then the reset word, and the answer must carry the banner (`ELM`).
Twice, then the reset pin as before. The word is `ATWS` since 2026-11-02
(what 4.x sent in place of every `ATZ`): the same complete reset without
the LED test, about 40 ms instead of 1.3 s, and without the junk bytes the
MIC3624 puts ahead of its `ATZ` banner (`00`, `00 F0 FD 00 00`, `BF 80 04`
in 5 of 16 tries on the bench, `test-reports/logs/atz_soft_reset_2026-11-02`
in the firmware repo; a NUL among them ended the C string before `ELM`, so
about every third software restart since 2026-10-19 logged "soft reset not
confirmed" and paid a reset pulse plus 1.5 s for a chip that had reset
fine). `obd_parse_extract` skips bytes outside printable ASCII at the start
of an answer and behind the echo now as well (host test
`test_extract_skips_reset_junk`). Check after a software restart: the log
reads `chip awake; warm start (ATWS)` and `bring-up done in` about 1.1 s
(1079 ms on the bench DUT, 2147 ms with `ATZ`, about 5 s on a boot that
fell to the reset pulse), `ATDPN` through the ELM port reads the stored
protocol (the warm start dropped the session); bench `test.ps1 obdbringup`
in the firmware repo (PASS 2026-11-02, 36 checks).

Related chip fact, corrected 2026-11-01 (issue #1023, the second dead chip):
the chip's memory function (`ATM1` / `ATM0`) is ON after every reset (the
vendor's pin 51 strap; no programmable parameter persists the switch), and
with it on the chip writes every protocol SWITCH that a request then answers
on into its non-volatile protocol store, RAM-only `ATTP` or not: `ATDPN`
after an `ATZ` reads the last protocol that answered (`A6` from a search,
`6` from a pinned `ATTP6`), and the value survived a power cycle on the
bench. A profile that flips protocols between rows (the VW MEB family:
`ATSP7` rows beside `ATSP6` rows) wrote the store on every flip, 2,500 to
25,000 times per driving hour on v6, against a 10,000-cycle rating; legacy's
AutoPID sent `atm0` at every start and never wrote it. The EEPROM guard's
`ATSP` -> `ATTP` rewrite does not prevent it (the trigger is the connection,
not the command), so the driver sends `ATM0` itself: "The memory function"
under the EEPROM guard below. `obd_chip_protocol_save()` (a real `ATSP`) is
the one deliberate write and saves with or without the switch. The bring-up's
`started:` line reads `memory off` since then.

## Bridge-endpoint ABI (TASK_obd_chip_manager_new §2/§3.6: stable)

The `subscribe`/`unsubscribe`/`send` trio is this component's face to
`bridge_manager`: glue or `main` wraps it into a `bridge_endpoint_t` (three
one-line functions), obd_chip itself never depends on bridge_manager.
`obd_chunk_t` is layout-identical to `bridge_chunk_t`/`socket_chunk_t` by
convention, so bridge queues carry it unmodified. **Treat the trio's shape
as a stable ABI**: every configured bridge (TCP, BLE, the USB port-B
passthrough, which is a `raw` bridge in bridge_manager, NOT code here)
depends on it. The test app's USB bridge is exactly this shape and predates
bridge_manager; production replaces it with a configured bridge.

## Claim model (task §5)

- `request()` claims COMMAND internally; concurrent requesters serialize
  (10 ms retry loop, caller timeout).
- MONITOR held ⇒ `request()`/COMMAND claims **fail fast**
  `ESP_ERR_INVALID_STATE` (**policy "manual"**: the settings key
  `monitor_policy` reserves `auto_interrupt`, pending meatpi's answer to task
  §11.6; nothing auto-sends the stop byte in v1).
- EXCLUSIVE (fw update): fan-out pauses (RX task discards), `send()` rejects.
- Raw bridges are ungoverned by nature: a user typing `ATMA` through a bridge
  without claiming makes `request()` callers see garbage-until-timeout, not
  corruption, the engine treats an unterminated window as `ESP_ERR_TIMEOUT`
  ("bus busy"). Bridges SHOULD claim MONITOR on their client's behalf.
- **Cross-requester (obd_gate, 2026-07-11)**: the claim model arbitrates
  the CHIP's users; the chip as a whole additionally holds the shared
  `obd_gate` while a command is in flight (`send()` acquires on a CR,
  the RX fan-out releases on the line-start `'>'`) so its conversations
  never overlap the ESP-side ELM engines on the same physical CAN bus.
  See `components/obd_gate/README.md`.

## Firmware update (verified legacy flow: do not improvise)

`VTVERS` (expect `MIC3624 …`) → `VTDLMIC3422` → `VTDLDT<line>` per record
(each answered `OK`; `?` = rejected) → stop at the `FFF1…` end marker →
`VTDLED` (2 s settle, 3 attempts) → hardware reset pulse (GPIO41 low 5 ms) →
rewake. Vendor images are `.txt` hex-record files (~3.5 k lines / ~490 KB).
Two sources: a `filesystem` path (`obd_chip_firmware_update`, read whole
into PSRAM) or the **packaged image** (`obd_chip_firmware_update_builtin`,
`EMBED_TXTFILES` of `obd_chip_manager/obd_fw/V2.3.24.txt` since 2026-10-28,
byte-identical to legacy `main/obd_fw`; +480 KB app size). Update responses
DO end with the `>` prompt (bench-verified): byte-level echo-skip, collect
until `>`, then classify `OK`/`?` (`obd_chip_fw.c`). Returning early on
`OK\r` without waiting for the prompt makes the next record race the chip
and get dropped. Measured 2026-10-28 on the bench: 3557 records in 13 s at
2 Mbaud, 19 s for the whole job with the finalize, the reset and the fresh
bring-up.

**Auto-update (legacy parity, Ali ruled 2026-07-26; the v4.52p policy since
2026-10-28, issue #1023):** with the `auto_update` setting on (default),
the bring-up task runs the builtin update with `OBD_FW_RULE_STARTUP` after
every bring-up. A chip at a **current** version costs one `VTVERS` and
skips: current = V2.3.22 or V2.3.24 (`obd_fw_version_is_current`, the list
legacy v4.52p keeps in main fab54ad; v6 bundled V2.3.22 alone and
DOWNGRADED a chip that v4.52p had taken to V2.3.24 on its first boot). An
older chip is flashed (EXCLUSIVE: consumers see timeouts meanwhile, log
lines mark start/progress/done, then a fresh bring-up restores echo-off,
the provisioning and the RX task); a chip stuck in download mode (`?`,
which also fails bring-up) is recovered by completing the download. The
gate (`fw_gate`) probes `VTVERS` at the baud the UART runs at and at the
other one (a chip in download mode sits at 2 Mbaud while a failed bring-up
leaves the UART at 115200), logs what came back (`VTVERS at 115200 baud: 2
byte(s) but no prompt (FC 00)`), and, like legacy, still tries
`VTDLMIC3422` (at both bauds) when bytes arrived without a prompt; only a
wire silent at both bauds aborts (`no answer on the wire ... not
flashing`). The bring-up logs the same wire note when `ATI` gets no
prompt. Bumping the packaged version = new `EMBED_TXTFILES` path +
`OBD_FW_BUILTIN_VERSION` in `obd_chip_fw.c` + the current list in
`obd_chip_fw_version.c`. A chip left on V2.3.22 is accepted (the start-up update
leaves it alone) but reported outdated (`obd_fw_version_is_outdated`,
host-tested; `fw.outdated` on `GET /api/obd_chip`; the bring-up's warning
line; the `warning:` line of `obd_chip`; the Maintenance card's banner
with its Update button): the user's update lifts it (Ali, 2026-10-28).

**No forced rewrite (Ali, 2026-11-01).** Under either rule a chip is
written only when the image's version is above the one the chip reports
(`obd_fw_version_is_outdated`: the first `V<a>.<b>.<c>` of each side,
compared as numbers field by field): never the same version, never a
downgrade, and a chip whose answer carries no version is left alone.
`force` is gone from the engine, the job, the console and the route (a
`force` key on the POST is refused with 400, `--force` on the console
prints why). A file's version is its name (`/data/V2.3.24.txt`; a name
without one is refused before the file is looked up). The two rules differ
only for a chip on the accept list below the packaged version (V2.3.22
today): the start-up leaves it alone, the user's update writes it. The
download-mode recovery is unchanged: a chip answering `?` or bytes without
a prompt has no version to compare and completing the download is its
only way out. The benches put the chip on V2.3.22 by uploading that image
as `V2.3.99.txt` (the name is the version): `test.ps1 obdfw` and
`canselftest`.

**From the console and the API (2026-10-28):** `obd_chip` (state, the
chip's `VTVERS` with a warning when it is older than the packaged image,
the packaged image, the job), `obd_chip update [--file <path>]` and
`POST /api/obd_chip/update {"path"}` run the user's update as a job
(`obd_chip_firmware_update_start`): the packaged image or the file, written
only when newer than the chip's firmware. `obd_chip probe [baud]` sends `ATI` and `VTVERS` on the bare
wire at 2000000 and 115200 and prints every byte that came back, prompt or
not; `obd_chip probe scan` tries `ATI` at every standard baud (9600 to
3000000); `obd_chip probe auto [seconds] [baud]` lets the S3's UART hardware
measure the bit time of whatever the chip sends; any other word on the line
(`obd_chip probe scan VTDLMIC3422`, `obd_chip probe auto 15 VTDLMIC3422
115200`) replaces `ATI` as the stimulus, for a chip whose application is
gone and whose bootloader answers only the download-mode entry (`obd_chip_autobaud()`
over IDF's `uart_detect_bitrate_start/stop`: the shortest low and high
pulse on RX, `clk * 2 / (low + high)`; a CR and `ATI` go out at the current
baud first so a quiet chip has something to answer, a chip talking on its
own is measured as it comes) and then listens at the nearest standard
baud; `obd_chip listen <seconds> [baud]` sends nothing and prints what the
chip sends by itself, text and hex. Together they are the field
diagnostics for a chip that will not come up: a chip in download mode
reads `?` at 115200 and nothing at 2 Mbaud; a chip that fills the RX ring
on its own (the issue's unit: about two bytes every 5 s with the ESP
silent, READY high on one boot and low on the next) is one that talks at a
baud nobody listens at, a reboot loop's banner being the likely text, and
`probe auto` names the baud. The web UI shows none of this (meatpi
2026-09-07: chip knobs stay API/CLI only).

**The sleep entry waits for a running update** (sleep_manager, up to
180 s): the chip's sleep pin pulled under a half-written image is the
one interruption the download mode does not survive (issue #1023: a unit
flashed with the engine off, under the sleep threshold).

**The sleep word (2026-11-07, Ali, after the review of the sleep path
against 4.52p: `internal/OBD_CHIP_SLEEP_V6_VS_V4.md`).** `obd_chip_sleep(true)`
sends `STSLEEP0`, the documented ST sleep command, through the request
path and pulls the PowerEn pin behind it; legacy's `elm327_sleep()` did
the same at every sleep entry and every re-sleep. Why the word: in the
chip's provisioned state (native control mode, the external, UART and
voltage sleep triggers OFF) the OBDLink and MIC documentation give the ST
sleep command as the one way to sleep it; the pin works through the
vendor's `VTPPSW` function alone, and a chip whose PowerEn function is off
or inverted never slept under v6 (bench 2026-11-01, E3a: six reset pulses
and a recovery reboot per attempt, then the whole sleep delay awake). A
command, not a stored setting: no EEPROM. The chip answers `OK` with a
prompt (bench 2026-11-07: `4F 4B 0D 0D 3E`) and sleeps on its own a second
or two later, READY following that late (still awake 1.5 s after the OK),
so the answer is the verdict, I `chip asleep by command (STSLEEP0 answered
OK)`, and the first nap's READY check the ground truth, as before the word.
A refusal (a chip mid-monitor answers `?`, a dead wire nothing, a claim a
stuck holder still has) is one W `chip did not take STSLEEP0 (...): the pin
alone` and the pin as before; a chip that is asleep already or never came
up gets no word. The nap's re-sleep (sleep_manager) pulses the reset, waits
the chip's 1.5 s boot and calls this, so the word finds the chip listening;
the crash park stays pin-only (no driver in that boot). What the bench
taught about the PowerEn switch the same day: `VT PPSW xy` is x = the
WAKE trigger, y = the SLEEP trigger (0 low level, 1 high level, 2 rising,
3 falling, F none): `10` = high wakes, low sleeps; `0F` = a LOW level wakes
and nothing sleeps, so a chip set to `0F` is woken by our own pin at every
entry and re-sleep (the chip's `STSLLT` read `SLEEP: CMD WAKE: PWR EN`);
`FF` is refused by the chip; `2F` (a rising edge wakes, nothing sleeps) is
the stand-in for a chip the pin cannot sleep; the bare `VT PPSW` disables
the pin function altogether (the vendor's sheet, in the app repo's
`internal/MIC_PPSW_POWEREN_INSTRUCTIONS.md`). A command-slept chip holds
its sleep through an ESP reboot and ignores 2 Mbaud words on its UART, but
the pin's wake level undoes it at once (`10` with the pin high). Bench:
`.\test.ps1 sleepword` (the app repo's `testing/sleepword.md`): a healthy
`sleep test` reads the I line before `components down`, and with `VTPPSW2F`
on the chip the command alone sleeps it (an `ATI` after it gets nothing),
the naps find it asleep, and the night that used to end in a recovery
reboot ends in the timed wake.

## Settings (`"obd_chip"`, version 3)

The UART baud is **not a setting**: `OBD_CHIP_BAUD` (2 Mbaud) in
`obd_chip_private.h`. meatpi 2026-09-07: exposing it invited a user changing
it, breaking the chip and claiming warranty; v3 drops a stored `baud` on
migration. The same decision took **every** `obd_chip` field out of the web
UI (they only confused users): the group stays reachable through the
settings API / CLI / backup for the future sleep_manager work.


| Key | Type | Default | Notes |
|---|---|---|---|
| `auto_sleep` | bool | `false` | reserved (the future sleep_manager arms the chip's controls) |
| `auto_update` | bool | `true` | flash the packaged chip fw (V2.3.24) after bring-up on a chip below the current list (current = V2.3.22 or V2.3.24, the v4.52p list; 2026-10-28), never the same version or a newer chip (2026-11-01): legacy parity |
| `cli` | bool | `true` | the `obd_chip` console command (standard §6b; 2026-10-28, optional key, no version bump) |
| `monitor_policy` | enum manual/auto_interrupt | `manual` | `auto_interrupt` reserved (§5 open question) |
| `wake_voltage_mv` | int 12000–15000 (must exceed `sleep_voltage_mv`) | `13500` | RESERVED since 2026-11-07: not written to the chip, not compared with it (was `STSLVLW` at boot; see "Boot provisioning") |
| `sleep_voltage_mv` | int 12000–14000 | `13200` | RESERVED since 2026-11-07 (was `STSLVLS` at boot) |
| `sleep_time_min` | int 1–60 | `2` | RESERVED since 2026-11-07 (was the `STSLVLS` hold, `min*60+30` s) |

## Boot provisioning (task §11.5: ANSWERED by meatpi 2026-07-04: "follow legacy init")

`start()` after `ATE0`: read `STSLCS` (pure parser `obd_chip_stslcs.c`,
grammar = the legacy `main/obd.c` sscanf patterns, fixture = a real
MIC3624 capture) and, ONLY when a control is on, rewrite the chip's stored
sleep config: ensure NATIVE control mode (`ATPP 0E SV 7A`+`ON`), switch
every autonomous control **OFF** (`STSLVl off,off`, `STSLU off, off`:
byte-exact legacy strings), stop the chip waking itself on bus activity
while the ESP32 sleeps (`ATPP 0F SV 95`+`ON`: the Activity Monitor options
with bit 6 off, not the baud; legacy's `elm327_disable_wake_commands`),
then `ATZ` + re-bring-up (baud restore + `ATE0`). A chip with the controls
off skips everything: provisioning ran once after flashing, the next
boots go straight to `started`.

**The stored thresholds are not written any more (2026-11-07, Ali).**
Until then the block also programmed the voltage thresholds and the UART
timeout from the settings (`STSLVLW >wake, 1`, `STSLVLS <sleep, time`,
`STSLUIT 1200`) and re-provisioned when the chip's stored values differed
from them (legacy's reprogram condition). `test.ps1 sleeptrig` (the app
repo's `testing/sleeptrig.md`) proved those values inert while the controls
are off: 12.5 V held 170 s against a stored `<13.20V FOR 150 s` and the
chip stayed awake, 14.0 V held 30 s against a stored `>13.50V FOR 1 s` on a
chip asleep by command and it stayed asleep, 25 min of UART silence against
a stored 1200 s and it stayed awake. So the compare looks at the four
control states only, the three settings fields are reserved, and whatever
the chip holds as thresholds stays as it is (the bench chip: 13.20 / 13.50 V,
150 s, 1200 s). Fewer EEPROM writes per provisioning, and a settings number
nobody sees no longer costs one.

PP 0F has its own read-back since 2026-11-02 (Ali: a chip image or a tool
could put it back to the vendor's D5, and STSLCS does not report it): the
bring-up reads `ATPPS` once (12 lines, a read) and writes the pair back
only when the value is not 95 or its enable is off (`obd_parse_pp`, pure,
host-tested), counted in `obd_chip_stats_t.pp_restores` with one W line
naming what it read; an intact chip costs the read. A chip firmware
rewrite keeps the PP area (bench 2026-11-02: .24 to .22 to .24, every
parameter, STSLCS and the PowerEn switch as found), so with the stock
images the write never happens; `test.ps1 obdbringup` injects the fault
(`ATPP 0F OFF` through the wire probe) and watches the restore.

**Framing lesson (found live by STSLCS):** the `>` prompt terminates a
response only at LINE START, `STSLCS` data contains mid-line `>`
(`VL_WAKE: OFF, >13.50V`), which used to truncate the accumulator and
trigger a reprovision-every-boot loop (`obd_parse_feed`, host-tested).

## Open stubs (task §11: ask meatpi)

1. ~~**READY pin semantics**~~ **ANSWERED 2026-07-04** (legacy
   `elm327_chip_get_status`, reference copy in
   `../obd_chip_manager/elm327.c`): the pin is **ACTIVE LOW**,
   LOW = awake/ready (`ELM327_READY = 0`), HIGH = asleep. The earlier
   "reads 0 while answering" observation was correct behavior; the v6
   polarity was inverted and is fixed (`obd_pin_ready`,
   `obd_chip_status_ok`).
2. ~~Full **chip init sequence**~~ **ANSWERED 2026-07-04** (meatpi:
   "follow legacy init"), see *Boot provisioning* above. Boot now also
   follows the legacy `elm327_init` flow: RESET pin is an OPEN-DRAIN
   output; reset is CONDITIONAL (READY says awake → soft `ATZ`,
   asleep/stuck → hardware pulse); `STWBR` persists a successful `STSBR`
   switch as the chip's power-on baud; fw ≥ V2.3.22 gets the power-pin
   check (`VTPPSWS` → `VTPPSW10` when wrong; since 2026-11-07 the write
   must answer `OK`, gets 600 ms to settle (legacy's 100 + 500 ms) and is
   read back as `10` BEFORE the reset pulse, an unreadable value is left
   alone and a refused write gets no pulse: `obd_parse_ppsw`,
   host-tested; until then the pulse came 0 ms after the OK); a READY-wait loop
   (10 × 200 ms, then one forced reset) gates the bring-up; sleep parks
   BOTH the SLEEP and READY pins (pulldown + RTC pulldown + hold).
   UART ring buffers: legacy 18 K / 18 K, and they live in **PSRAM** in
   both firmwares, the driver allocates them with `MALLOC_CAP_DEFAULT`
   (`UART_ISR_IN_IRAM` off) and the SPIRAM malloc policy routes that to
   PSRAM. No DMA is involved (the ISR copies the 128 B hardware FIFO
   into the ring), so PSRAM placement is safe exactly as it was in
   legacy.
3. ~~`VT` 4 KB ISO-TP command syntax~~ **ANSWERED + bench-verified
   2026-07-03** (meatpi's vector, 4095 bytes reassembled byte-exact on PCAN;
   cross-checked against the WiCAN Pro tester project:
   `wican_pro_tester/protocol_notes.md` + `MTPS-OBD+Log.TXT`):
   `VTFullyRequestCk<LLLL><data hex><CCCC>[ <n>]`, LLLL = byte count (4
   hex, max 0FFF), CCCC = 16-bit additive checksum of the payload bytes,
   optional space-separated `<n>` = the standard ELM "expected number of
   responses" hint (the tester logs use the same form on plain requests:
   `22F106 1`). The chip transmits on the current header (0x7DF default;
   the tester uses `ATSH7E0` on CAN channels and J1850 3-byte headers like
   `ATSHC4D0F5` on VPW), REQUIRES a prompt Flow Control from the response
   ID on CAN (0x7E8 for 0x7DF; ignores the ECU sim's FC from request−8 =
   0x7D7; answers `FC RX TIMEOUT` otherwise), then prints EACH ECU response
   message (headers shown per the header setting, including `7F xx 78`
   response-pending interims) or `NO DATA` if none, then the `>` prompt.

   **Real-world transcript (tester log, channel 2 = J1850 VPW `AT SP2`,
   UDS `0x36` TransferData: the production use case is ECU flashing):**

   ```
   >ATSHC4D0F5                              <- J1850 prio/target/source
   OK
   >VTFullyRequestCk03FF3600...<1023 B>...1CA8 2
   C4F5D07F3678C0        <- response w/ header: 7F 36 78 = pending
   C4F5D0760022          <- final positive response 76 ...
   ```

   (The `IT CH_NO <n>` + `OK` lines that follow in the raw log are NOT
   chip output: that is the MTPS-OBD **Bluetooth tester box** being told
   to switch channels for the next test step; see
   `wican_pro_tester/protocol_notes.md`. The VT response ends with the ECU
   messages + prompt, exactly as reproduced on our bench.)

   Repeatable check: `tools/testbench/obd_vt_isotp_check.py [--size N]`,
   the PC side is a real ISO-TP stack (python-can + can-isotp `CanStack`),
   so FC, sequence numbers and reassembly are standards-checked; byte-exact
   with zero stack errors at 7/100/4095 bytes. Goes through plain
   `obd_chip_request()`/`send()`: no component changes were needed.
4. Authoritative **monitor-class command list** (table currently
   ATMA/ATMR/ATMT/STM/STMA in `obd_chip_parse.c`).
5. `monitor_policy=auto_interrupt` behavior.

## Dependencies

`esp_driver_uart`, `esp_driver_gpio`, `settings_manager`, `log_manager`,
`filesystem` (fw file reads), `cmdline_manager` (the `obd_chip` command,
2026-10-28), `http_server_manager` + `esp_http_server` (the own routes),
`obd_gate`, `event_manager`: all private. Init order: after the core
managers; `settings_manager_start()` before `obd_chip_start()`.
`can_selftest` (above this component) uses the public request, send,
subscribe and transaction API for the CAN self-test.

## Memory footprint (estimated: measure before release)

| Where | What | Size |
|---|---|---|
| Internal | UART driver RX 8 KB + TX 4 KB (`// internal: DMA`), RX-task stack 16 KB (`// internal: UART driver path`), TX mutex/TCB | ~29 KB |
| PSRAM `.bss` | subscriber registry, cmd-engine queue (64×130 B) + accumulator (16 KB since 2026-09-08: a 4 KB ISO-TP response is 8–12.5 KB of hex; the 4 KB accumulator truncated it) + bare_probe accumulator (16 KB), config | ~41 KB |
| PSRAM heap | fw image buffer during update only | ~500 KB transient |
| PSRAM `.bss` (2026-10-28) | the wire tools' result (job + console copies, ~3 KB each), the HTTP copy, the report (11 x 512 B, console + HTTP copies), the bring-up's two wire notes | ~22 KB (estimated) |
| Internal heap, transient (2026-10-28) | the update job's and the wire tools' task stacks (4 KB each, UART driver path), one at a time | 4 KB while one runs |
| PSRAM heap, transient (2026-10-28) | the bodies of `GET /api/obd_chip/probe` (6 KB) and `/report` (6 KB) | per request |

## Tests

- **Bench (2026-10-28):** `.\test.ps1 obdfw` (the firmware update paths:
  `OBD FW PASS` 25 checks) and `.\test.ps1 canselftest` (the wire tools,
  the report and the CAN self-test routes on the real chip and the bench
  bus: `CAN SELFTEST PASS`), rows in the app repo's TESTING.md; since
  2026-11-02 `.\test.ps1 obdbringup` (the warm start, the PP 0F guard) and
  since 2026-11-07 `.\test.ps1 sleepword` (the sleep word at the entry and
  in a re-sleep, the PPSW repair's settle and read-back) and `.\test.ps1
  sleeptrig` (the chip's own sleep triggers off: the stored thresholds and
  UART timeout inert; `-ProvisionLeg`: a hidden settings change provisions
  nothing, a control switched on provisions the controls off and leaves
  the thresholds alone).
- **Host (`host_test/`, 23 tests):** pure framing, prompt splits at every
  chunk boundary, echo strip, post-prompt bytes not consumed, noise
  interleave, **meatpi's real 36-line proprietary-PID log replayed at chunk
  sizes 1…4096**, overflow flagging, error/monitor classification, fw-file
  iterator + FFF1 marker, the EEPROM guard (rewrites, pass-throughs,
  refusals, raw-chunk semantics, and since 2026-10-01 a mixed-protocol
  profile chain `ATSP6;...;ATSP7;...` whose BOTH protocol switches come out
  as `ATTP`), the STSLCS parser + provisioning policy, the PP parser, the
  `OK` acknowledgement and the VTPPSWS value (2026-11-07).
- **On-target (`test_apps/`, live bench):** all green 2026-07-03, see
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
`GET /api/obd_chip` -> `eeprom_guard.rewrites` / `.blocked` /
`.protocol_saves`; refused commands log at W with the text. autopid's
`ap_init_sanitize()` is a wrapper over this guard and its config parse refuses
a refused command in `cmd`/`init`; the UDS AT transport sends `ATTP` itself.
Exempt by construction: boot provisioning (`bare_probe`: STSL*, `ATPP 0E/0F`,
`STWBR`, once, behind a matching check) and the firmware update flow, which
write the UART directly.

### The memory function (issue #1023, 2026-11-01)

The guard above stops the EEPROM-writing COMMANDS. The chip has one more
writer the commands do not show: with its memory function on, a protocol
switch that a request then answers on is memorised in the non-volatile
protocol store, `ATTP` or not (bench 2026-11-01, the firmware repo's
`internal/TASK_obd_chip_protocol_store.md`: the legs, the datasheet, the
design). The function is ON after every reset and `ATM0` is RAM-only, so the
driver re-sends it at the three places that between them cover every reset:

- **the bring-up**, after the provisioning (which may `ATZ`), before the
  identity `ATI`: every reset this sequence performs (a boot, a wake, a
  nap's pulse, the post-update fresh start). The `started:` line reads
  `memory off`; a chip that did not answer `OK` gets one W line, and the
  next protocol switch covers it;
- **the request engine**, ahead of every protocol switch:
  `obd_chip_request()` runs an `ATM0` transaction first when
  `obd_parse_sets_protocol()` names the word (`ATSPh` / `ATTPh` with the
  `A` and `00` forms, STN `STPhh`, vendor `VTPhhh`; not the reads `STPR`,
  `STPBR`, `ATDPN`), the pair inside one transaction hold when the caller
  holds none. That covers AutoPID's prelude, its restore and every row
  init, the standard scan's `ATTP0` and its protocol walk, the profile
  probe, the UDS tool, J2534's `ATTP2` and the CAN self-test, whatever
  reset came before (an app's `ATZ`, the self-test's `ATD`, the chip's own
  brown-out). One word and one `OK`, about 1 ms at 2 Mbaud; the MEB profile
  pays seven per poll cycle.

- **the bridge path** (`obd_chip_send`), behind an app's reset (Ali,
  2026-11-01): a chunk that is exactly `ATZ`, `ATWS` or `ATD`
  (`obd_parse_reset_word()`, pure) goes out as a warm start (`ATZ` as
  `ATWS`: the same `ELM327 v2.3` banner, about 40 ms on the wire instead
  of 1.2 s; what 4.x did with every `ATZ`), and when the answer's prompt
  passes through the fan-out the RX task writes `ATM0` at once and hides
  its echo and `OK` from every subscriber up to the next prompt, the
  obd_gate held across both. The app sees exactly the reply it asked for,
  once, and its session runs with the memory function off: its own
  protocol choice after the reset no longer writes the store (bench: the
  `attpstore` canary, which wrote on the build before). The `ATM0` cannot
  go in the same chunk as the reset: the chip's reset eats what follows
  it (bench: `ATWS\rATM0\r` back to back answered the banner alone), so
  a chunk with a second command behind the reset is passed as it is.
  Failsafes: a reset the chip never answered disarms after 3 s; an
  `ATM0` whose prompt never came lets the bytes through again after 1 s
  with one W line. The guard's own `?` reply to a refused word goes to
  the subscribers directly, outside that window. The registry, the
  fan-out and `obd_chip_send` live in `obd_chip_fanout.c` since this
  change (the 700-line rule); `obd_chip.c` keeps the lifecycle, the
  bring-up task and the stats.

`obd_chip_protocol_save()` saves with or without the switch. Counted in
`obd_chip_stats_t.memory_off` (the acknowledged `ATM0` words, all three
places), `.app_resets` (reset words intercepted), `.hidden_bytes` and
`.pp_restores` (the PP 0F read-back above), on `GET /api/obd_chip` under
`eeprom_guard`, printed by the `obd_chip` console command. Host tests: `test_protocol_switch_classification`,
`test_reset_word_classification`; bench: `test.ps1 attpstore` in the
firmware repo (the engine's switch and the bring-up's `ATM0` read back
through an `ATZ`, the app's reset answered once and fast with nothing
shifted, the app's own switch after its reset no longer writing).

### Protocol save (2026-10-01, TASK_quick_setup.md "Protocol policy")

With the guard alone the chip would power up on whatever its EEPROM held
from the factory or an old app, and search (`SEARCHING...`) at every boot.
`obd_chip_protocol_save(proto)` is the ONE deliberate exception: a real
`ATSP<proto>` (set AND save) through the same serialized transaction path
as `obd_chip_request()` (COMMAND claim, obd_gate, the `>` prompt), with the
guard switched off for that single command. Rules enforced in the driver,
whatever the caller does: `proto` is `1`..`9` or `A`..`C` (never `ATSP0`,
`ESP_ERR_INVALID_ARG`), at most ONE save per boot (`ESP_ERR_INVALID_STATE`
afterwards, so a misbehaving caller cannot wear the EEPROM), the chip must
answer `OK` (`ESP_FAIL` otherwise, `ESP_ERR_TIMEOUT` on silence). The only
caller is autopid's vehicle detection job, after a successful detection,
when the car record's `chip_protocol` differs from the detected base
protocol; it then records `chip_protocol`. Everything else on the wire
stays `ATTP`: autopid's baseline re-asserts the base RAM-only after an
ELM app had the chip, and a mixed-protocol profile's per-PID `ATSP7` keeps
being rewritten to `ATTP7` by the guard (host test: the MEB-style chain).
Counted in `obd_chip_stats_t.protocol_saves` and `GET /api/obd_chip`
`eeprom_guard.protocol_saves` (0 or 1 per boot); logged at I on success.

## Transaction hold (2026-09-16)

`obd_chip_txn_begin(timeout)` / `obd_chip_txn_end()`: the calling task takes the
COMMAND claim across several `obd_chip_request()` calls (they nest; other
requesters wait at their claim timeout, autopid's poll included). Born from the
UDS AT transport: 7 setup commands + the request, and autopid's poll used to
land between them (bench: 1 of 3 UDS requests survived with polling, 5 of 5
with autopid paused). `end()` drops the hold whatever the nesting depth.
