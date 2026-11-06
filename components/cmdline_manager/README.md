# cmdline_manager

The WiCAN command line: ONE esp_console-backed command **registry** and
dispatcher, reachable over every transport **without any transport code
in this component**, and owning **no commands except `help`**
(rewrite of the legacy `cmdline`; registry shape follows IDF's
`examples/system/console` per-domain `register_*()` convention).

## Who owns which command

Every component registers its own commands with
`cmdline_manager_register()` from a `<comp>_cli.c` /
`<comp>_register_cli()` that the component itself calls on its settings
boot apply, gated by its own **`cli` setting** (bool, default true,
reboot-to-apply): main wires nothing (meatpi 2026-07-05; the
`*_register_http()` ownership rule taken one step further). Components
without other settings (dev_status_manager, filesystem,
external_storage, restart_tracker) carry a minimal `{cli}` descriptor
registered via `<comp>_register_settings()` right after
settings_manager_init (they init before it: the
log_manager_register_settings pattern). Turning `cli` off removes that
component's commands from every surface (UART, WS, BLE) at the next
boot:

Interface rule: **legacy argtable3 options behave legacy-exact**
(outputs byte-alike, `OK` trailers); bare invocation gives a v6 status
summary (legacy bare was an error: nothing depended on it); v6
additions are options too.

| Command | Owner |
|---|---|
| `help [cmd]` | cmdline_manager (the registry's own; legacy format) |
| `version`, `status` | dev_status_manager |
| `wifi -s/-i/--scan`, `wifi --try <ssid> [--password <pw>]` | wifi_manager (`-s`/`-i` legacy-exact; `--scan` = v6 JSON scan; `--try` = the connection trial of 2026-10-06, joins without saving and prints `connected: ip, rssi dBm, channel N, in N ms` or `password|not_found|refused|no_ip|timeout (reason N) after N ms`, up to 20 s) |
| `rtc -s/-r/-i/--forget` | rtc_manager (sync/read legacy-exact; `-i` = responding probe: the RX8130 has no ID register; bare = the summary incl. the clock-step line since 2026-10-13; `--forget` writes 2000-01-01 into the chip so the next power-on boot starts without a clock: the bench's drained-caps unit, issue #984) |
| `logger`, `logger test <rows> [<source> <name> [<age_s>|@<epoch_s> [<step_ms>]]]`, `logger frametest <n>` | data_logger (new in v6: both streams' status incl. the clock line and, since 2026-11-06, the export line (what `/api/logger/export` served); synthetic rows / frames for the bench; `test` takes a source and name and a start in the past (an age, or `@<epoch_s>` absolute) since 2026-11-06, the store-and-forward bench's known series; the row was missing from this table until 2026-10-13) |
| `imu -i/-r/-x` | imu_manager (`-i` legacy-exact; `-r` + bare = v6 activity/accel/temp; `-x` = the detector registers as the chip holds them, the MREG1 thresholds read plain and with MCLK forced, 2026-11-06) |
| `led -i/-c <r g b>/-b/-x` | led_manager (legacy set-color drives an ALERT-priority indication: the arbiter owns the LED; `-x/--clear` releases it) |
| `battery` | battery_monitor (new in v6) |
| `sleep`, `sleep hold <min>`, `sleep test <secs>` | sleep_manager (new in v6: state, the battery against the sleep / wake pair, and since 2026-10-06 a `sleeps in N s (...)` line while the sleep delay or the critical floor counts down and a `held awake on request` line while a hold runs; `hold` keeps the device awake 1..30 min more, three per boot, the web UI's Keep awake button; `test` forces a timed bench sleep). The row was missing from this table until that day |
| `can_selftest`, `can_selftest run` | can_selftest (self-registered on settings apply behind `cli`, 2026-10-28, issue #1023: the OBD chip asks `010C` to 7DF and the ESP32's CAN controller answers as engine ECU 7EF over the CAN pair both share, one exchange proving both transceivers, both directions and the wiring; bare = the last result, `run` = the test in the console's task, about 2 s, then the verdict, the three steps with their ms and the exchange line by line) |
| `obd_chip`, `obd_chip report`, `obd_chip update [--file <path>]`, `obd_chip probe [baud] [scan\|auto [seconds]] [command]`, `obd_chip listen <seconds> [baud]` | obd_chip (self-registered on settings apply behind `cli`, 2026-10-28, issue #1023: bare = ready / claim / baud, the chip's `VTVERS` answer with a `warning:` line when it is older than the packaged image, the bring-up's wire verdicts when the chip is down, the packaged image (V2.3.24) with the current-version list, the update job's state; `report` = the chip configuration block for a bug report: ATI, AT@1, AT@2, VTVERS, ATRV, ATDPN, ATDP, ATCS, ATPPS (the programmable parameters, the EEPROM-backed settings), STSLCS (sleep and wake), VTPPSWS (the power pin switch), read in one transaction; `update` runs the chip firmware update on its own task from the packaged image or a vendor `.txt` on the filesystem whose name carries its version, and writes the chip only when that version is above the chip's own (2026-11-01, Ali: no forced rewrite; `--force` is refused with the reason, a chip in its download mode is still recovered by completing the download); `probe` sends `ATI` and `VTVERS` on the bare wire at 2000000 and 115200 (or the one baud given) and prints every byte that came back, prompt or not; `probe scan` tries `ATI` at every standard baud from 9600 to 3000000 and names the first with a prompt; `probe auto` lets the UART hardware measure the bit time of whatever the chip sends (a CR and `ATI` go out first at the current baud) and then listens at the nearest standard baud; `listen` sends nothing and prints what the chip sends on its own, text and hex: the field diagnostics for a chip that will not come up or talks at a baud nobody expects) |
| `sdcard [-i\|-t\|--format [--yes]]` | external_storage (bare: present, mounted, the state with its reason, the card and the last format; `-i` card name/type/capacity/sector/speed from its own sdmmc data, for a mounted or merely answering card; `-t` write/read-back self-test; `--format` = the FAT32 format of a card WiCAN cannot read, the warning alone without `--yes`, with it the request and its outcome waited for (2026-10-25)) |
| `fs` | filesystem (per-backend usage: /data, /sd) |
| `http [-a]` | http_server_manager (2026-10-31: the server task's stack placement and size, then the flash audit: the routes whose requests moved the SPI flash counters since boot with their request, read, write and erase counts, the catch-all row for the asset server; `-a` lists every route that served a request. Registered at the settings apply, no `cli` switch: read-only diagnostics) |
| `restart_tracker -l/-a/-p/-n/--panic[=abort\|fault\|wdt]/--report/--clear-report/--settle/--park-retry <s>` | restart_tracker (`--panic` = deliberate test panic: `abort()`, an invalid store, or the interrupt watchdog; `-l` / `-a` print a record's crash note under it, and `mode:` when the boot parked or ran safe mode; v6 flags print as hex. 2026-10-05: `--report` prints the crash report stored in NVS as the text a user sends on, the crash-loop brake in one line (`Brake: verdict=... streak=n/3 ... report_budget=n`) and how full NVS is; `--clear-report` forgets it. The bench's two knobs on the brake: `--settle` marks this run healthy now, so a crash that follows is a first crash; `--park-retry <s>` (10..3600, 0 = off) makes the NEXT park end by itself after that long, once) |
| `system -v/-r/-i/-m/-t` | **main_cli.c**: a composite (battery voltage + restart_tracker reboot + dev_status info/memory/tasks); only main knows all of them (`-t/--tasks` is the v6 addition) |
| `debug -e <0\|1>` or `debug <tag> <level>` | **main_cli.c**: log_manager sits BELOW cmdline_manager; registering there would be a dependency cycle (same reason /api/logs lives in api_http). `-e` legacy-alike but EPHEMERAL (persist via `/api/settings/log_manager`) |
| `factoryreset [-c/--confirm]` | **main_cli.c**: REAL since 2026-07-05: legacy two-step confirm flow (60 s window) -> `main_factory_reset()` (2026-10-26: the settings partition, the data partition `/data` and NVS erased, the crash report and the backup key written back, the SD card untouched; until then `settings_manager_factory_reset()`, the settings files only) -> reboot via restart_tracker `FACTORY_RESET` |
| `autopid [-l]` | autopid (self-registered on settings apply, §6b; bare = status/tables/poll counters, `-l` adds per-parameter latest values + age) |
| `j1939 [-s \| -d \| -p \| -r <pgn> [da] \| -z]` | j1939 (self-registered on settings apply, §6b; bare = state, mode, bus verdict, frame / store / transport counters, sources, and in active mode the address claim and the transmit counters; `-s` = the built-in values the bus carries, `-d` = DM1 trouble codes per controller, `-p` = every stored message, `-r <pgn hex> [da hex]` = active mode: send a Request for a group to one controller or to everyone (ERROR with the reason in listen mode or without an address), `-z` = zero the counters and forget the stored messages) |
| `destinations [-t <name>]` | data_destinations (self-registered on settings apply, §6b; bare = the destination table with its live counters/backoff/last error, `-t` = deliver one destination now and print the outcome) |
| `backup [-r]` | config_backup (self-registered on settings apply, §6b; bare = the backup key by date and state, the copies on the flash and the card with their refresh times and, since 2026-10-16, whether one holds the settings from before a factory reset (issue #987), the size of every part and how many files it leaves out (issue #989); `-r` = refresh the copies now, which lets a held copy go; no file over the console, no secret in the output) |
| `fwupdate [check\|install [<version>] [--wipe] [--dongle]\|abort\|notes [<version>]\|confirm\|forget]` | fw_update (self-registered on settings apply, §6b, 2026-10-18; bare = the summary: channel, the scheduled check, the uplink, installed + build + slot + its OTA state, the layout id and the bootloader version, the last check, the latest and the target with its verdict, the job, the last recorded job, and since 2026-10-19 the dongle line (present, the transport, its firmware and api, the verdict and the target) with its last relay job; `check` asks the task for a manifest check (both lists); `install` = the computed target or a named release, `--wipe` accepts a layout that formats, `--dongle` installs on the ESPNetLink through the relay; downloads never go over the dongle's LTE (no `--cellular` since 2026-10-19); `abort`; `notes` prints where the notes are (the fetch stays on the component's task, not the console stack); `confirm` = the verify milestone by hand (the bench); `forget` erases the NVS job record (the bench)) |
| `espnetlink`, `espnetlink pair <ssid> <password>`, `espnetlink repair` | espnetlink_link (self-registered on settings apply, §6b; bare = pairing/uplink/GPS/dongle-health status; `pair` = manual pairing into a wifi_manager fallback slot, reboot-to-apply; `repair` = VBUS cycle so the dongle re-enumerates and its key is re-read) |
| `usb`, `usb vbus <0\|1>`, `usb mux <0\|1>`, `usb suspend/resume` | usb_host_manager (bare = host/uplink status incl. VID/PID; `vbus` now goes through `usb_host_manager_set_vbus()`; the rest is bench debug) |
| `j2534` | j2534_server (self-registered on settings apply, §6b; status: enabled / tcp port / listening / attached tester + its transport `tcp`/`serial`/`ble` / device_open / exclusive + autopid paused / channels / frame counters) |
| `blehttp` | ble_http (self-registered on settings apply, §6b; the HTTP-over-BLE tunnel: enabled / registered / paired link / active request / requests, responses, last status / errors, aborts, timeouts, resync / bytes in and out) |
| `eth`, `conn`, `wusb`, `ping` | **main_cli.c** pending stubs: no v6 backer yet; only the composition root knows what's missing |

`help` output follows the legacy format (`Available commands:` +
`cmd - help` + indented `Usage:` hints + `Tip:`/`OK`; `help <cmd>` for
details).

## How each transport reaches the CLI

| Transport | Path |
|---|---|
| TCP / UDP | configure a bridge: `br_x = cli <-> obd0/udp0/...` (socket_manager server) |
| WebSocket | configure a bridge: `br_x = cli <-> ws_cli`, then `ws://<ip>/ws/cli` |
| BLE | main glues the CLI IN/OUT characteristics to `cmdline_manager_exec_line_async()`: lines QUEUE to the dispatcher task (4-deep; full = "busy" + prompt back), because they arrive on the NimBLE host task, which must never run a command inline (a long command wedged the BLE stack, fixed 2026-07-10, `ble_cli_wedge_test.py`) |
| UART0 | built-in **linenoise** console (settings `uart`, default on): line editing, arrow-key history (RAM, 32), tab completion + hints from the registry, dumb-terminal fallback via probe. Manual loop per the IDF advanced console example, NOT the REPL component, whose internal loop would bypass the dispatcher lock. |

Input bytes from the endpoint face are assembled into lines (`\r`/`\n`
terminate, ≤255 chars, oversize discarded whole), each line runs through
the one registry, and the response routes back to the invoking transport
only. One command at a time device-wide (a mutex serializes every
transport). Transport responses end with the legacy `wican> ` prompt;
on UART linenoise draws it.

Command handlers print with `cmdline_printf()` (formatted, ≤256 bytes
per call) or `cmdline_write()` (raw, any length: scan JSON, task
lists).

**What the dispatcher itself prints**, before the prompt and the same on
every transport (the command's own output comes first): nothing when
the command ran and returned 0; `command failed (N)` when the handler
returned N (argtable's option errors go to stderr, the UART, so over a
remote transport this line can be all a client sees); `Unknown command:
<line>` + `Try: help` for a name the registry does not have; `Internal
error: ESP_ERR_NO_MEM (0x101), the command did not run` when esp_console
itself failed (no memory for the argument vector, console not
initialised). Until 2026-10-11 that last case printed nothing and looked
like success (issue #980). The rule is pure (`cm_exec_verdict()`,
host-tested), and the longest unknown line keeps its newline.

## Caller contract (§2 corollary: learned the hard way)

Handlers run arbitrary component code, so **every task that calls
`cmdline_manager_exec_line()` needs an INTERNAL-RAM stack** with a few
KB of headroom: `fs` walks LittleFS on internal flash, which panics from
a PSRAM-stack task (cache-off assert, hit live). The dispatcher (6 KB)
and console (8 KB) tasks are internal for exactly this reason. Tasks
that can't satisfy the contract (or must never block: the NimBLE host
task) use `cmdline_manager_exec_line_async()` instead: the line +
sink queue to the dispatcher (a FreeRTOS QueueSet lets it serve both
the endpoint face and async lines), which runs the command on its own
internal stack.

## Settings (`cmdline_manager`, reboot-to-apply)

| Field | Default | Meaning |
|---|---|---|
| `enabled` | `true` | master switch (dispatcher + all transports) |
| `uart` | `true` | the UART0 linenoise console |

## Files

- `cmdline_manager.c`: lifecycle, registry + `help`, dispatcher, sink routing, endpoint trio
- `cmdline_manager_console.c`: the UART0 linenoise console
- `cmdline_manager_line.c`: PURE pieces, host-tested: the byte-stream→line assembler and the dispatcher's verdict line
- `host_test/`: assembler + verdict tests (13; runs on the IDF `linux` target)

## Memory (§2/§12)

Registry mirror + input-queue storage in PSRAM; both task stacks
INTERNAL (see caller contract); FreeRTOS objects internal. UART driver
(256 B RX buffer) installed only when the console is enabled.

Live verification (2026-07-04): UART0 linenoise console (echo/editing/
help format) and the WebSocket console via `br_cli = cli <-raw-> ws_cli`
from rpi001, `tools/testbench/wifi/cli_ws_test.py` → `CLI WS PASS`
(since 2026-10-11 it also checks the `command failed (1)` verdict, via
`led -c` without values; the `Internal error` verdict cannot be provoked
on a healthy device and is pinned by the host test).
BLE async path (2026-07-10): `tools/testbench/ble_cli_wedge_test.py`
(UB500 on rpi001, main firmware, WiFi+BLE both on), GATT reads stay at
52 ms median while 4 s of queued commands execute; 8-line flood all
answered (busy fallback exercised); link never drops.
