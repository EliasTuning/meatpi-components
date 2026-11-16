# j2534_server

> Wire protocol (all transports): [`J2534_WIRE_PROTOCOL.md`](J2534_WIRE_PROTOCOL.md).
> Windows DLL: `drivers/j2534/` in the WiCAN app repo. BLE binding:
> `ble_j2534` + `ble_manager/BLE_API.md`.

## Summary

SAE J2534 PassThru device: a tester (the Windows DLL, a phone app, a
bench script) opens a session, connects CAN, ISO15765, J1850 VPW and
TP 2.0 channels, writes messages, sets filters and periodic messages, and
receives bus traffic as unsolicited `RX_MSG` frames. One framed wire
protocol runs over three transports: the TCP listener in this component
(port 6809), USB CDC-ACM (`usb_cdc_device`) and BLE (`ble_j2534`, since
2026-09-21). Exactly one tester at a time across all of them; a second one
is answered `ERR_DEVICE_IN_USE`.

`READ_VBATT` returns the battery monitor's latest calibrated, averaged voltage,
rounded to millivolts, without a CAN channel or bus transaction. OPEN is required.
No sample or an invalid reading returns `ERR_FAILED` with no result. The Windows
DLL also needs the voltage update: older DLLs return a fixed 12,000 mV locally.
The wire version and settings schema are unchanged.

TP 2.0 as its own protocol (`TP2_0_PS`, protocol id 0x800E, SAE J2534-2
section 19; `j2534_channel_tp20.c`, contributed by Elias Kotlyar, merged
2026-11-17). The device does VW's transport as SAE J2819 describes it:
the channel setup and the connection setup (`REQUEST_CONNECTION`), a
message split into 7-byte telegrams and a received one put together, the
block acknowledges, a block sent again from the telegram the ECU names,
"receiver not ready" and a break, and one connection test per `T_CTA` on
an idle connection, answered with a connection acknowledge. Four
connections on the one channel, messages of up to 4092 bytes, broadcast
requests with their key bytes, `GET_DEVICE_INFO`. The application's
message goes out as written and the ECU's comes in as sent: the two length
bytes a VW ECU uses are the application's (SAE J2534-2 19.3.1). The
tester sees whole messages and two indications (`CONNECTION_ESTABLISHED`,
`CONNECTION_LOST`); a tool that does the transport itself still uses a raw
CAN channel, which does not read TP 2.0. The Windows DLL needs the TP 2.0
update: driver 1.7 does not know the protocol. Layouts, defaults and
limits: `J2534_WIRE_PROTOCOL.md` section 11. Proven against a model of
the document and one published real exchange, not yet on a car.

The RX pump (the `j2534_rx` task) pushes what the channels received as it
arrives: it blocks on the open channel's bus queue between frames (the
channel queues are static for the module's life, reset at CONNECT, so a
waiter never sees one deleted; `j2534_channel_wait_queue`) and falls back
to a 2 ms poll only for what no queue announces (an ISO-TP PDU the
provider completes, a J1850 reply, an indication). A burst of received
frames goes out in ONE transport write (`j2534_srv_send_raw`, up to 32
frames or a maximal frame's worth; the wire contract lets frames be
coalesced). Until 2026-11-06 the pump slept 2 ms between polls and wrote
one frame per transport write: a tool that acknowledges every block of a
transfer (a VW TP 2.0 flasher, the field report of 2026-10-27) paid up to
2 ms of that on every block end, on top of the link's round trip.

J1850 VPW (GM Class 2, protocol id 1) rides the OBD chip, not the native
CAN controller (2026-10-19, modelled on the nightjoker7 4.47p fork but
rebuilt on the v6 `obd_chip` claim model and the chip's own
`VTFullyRequestCk` USDT segmenter). Its messages are `[3-byte
header][payload]` whole, no CAN-id prefix. CONNECT runs the VPW init on the
chip (ATTP2, headers on, auto-receive); WRITE sets the header (ATSH,
cached) and sends the payload, parsing the chip's reply lines back into the
channel; PASS/BLOCK filters are applied in software over the header.
Request/response only (no monitor task for unsolicited frames); GM 4x
programming speed is not implemented. `J1850PWM`/`ISO9141`/`ISO14230` are
chip-wired on the hardware but not built yet (`ERR_NOT_SUPPORTED`).

**The VPW init, first run against a VPW node on 2026-11-11.** Until ECU
sim v2 the bench had no J1850 node, so the channel's reply path had never
run, and it could not have heard one: CONNECT succeeded, the write was
accepted, and no reply came back. Two faults in the init
(`run_vpw_init()`):

- `ATD` stood behind `ATTP2`. "All to defaults" includes the protocol:
  the chip went back to its stored one (`ATDPN` `6` on the bench) and the
  request went out on CAN. `ATD` is first now. The request engine sends
  its `ATM0` ahead of the `ATTP2` behind it, so the chip's memory function,
  which `ATD` switches on again, is off for the session.
- three ST pass filters (`STFAP 00F000,00FF00`, `00FE00`, `006C00`: replies
  to F0, broadcast FE, "ECUs on 6C"). With any ST pass filter set the
  MIC3624 handed on no reply at all: a functional `48 6B 10 41 00 ..` came
  back as `NO DATA`, also with a filter for 6B added. Under CAN, where the
  chip sat because of the first fault, the same words answer `?`. They are
  gone. The chip receives as an ELM does (`ATAR`), and what the
  application gets is decided by the channel's PASS / BLOCK filters in
  software (`j2534_legacy_poll`), which is the J2534 model. `STFCP` stays,
  to clear what an earlier session left.

Bench: the app repo's `.\test.ps1 j2534vpw` (the simulator on VPW alone,
`01 00` and `01 0C` each answered `48 6B 10 41 ..`, nothing written to the
chip). Not checked there: physical addressing (the simulator answered no
`6C 10 F1` request on the app path either), and the last byte of a
delivered message, which is the J1850 CRC as the chip prints it with
headers on (`48 6B 10 41 00 18 3F 80 03 0F`): whether a J2534 application
expects it stripped is an open question.

ISO15765 messages are laid out as SAE J2534-1 says (2026-10-19): the
4-byte CAN id first, then the UDS payload, in both directions, and the
pad flag on each message. The device took the bare payload before, which
no standard tool sends: a udsoncan tester's `10 03` reached the bus as the
6-byte PDU `00 00 07 E0 10 03`, unpadded, and every request timed out
(the field report of 2026-10-19, `test-reports/2026-10-19_0815_j2534_std_tool_repro.md`
in the app repo). The Windows DLL copies messages verbatim and did not
change. The wire version stays 1 (`J2534_WIRE_PROTOCOL.md` section 10).

The CAN bitrate is the device's, not the tester's (2026-10-03): the native
node runs at the `can_manager` setting, or at what its listen-before-talk
policy read off the bus. `CONNECT` (CAN or ISO15765) with another baud is
answered `ERR_INVALID_BAUDRATE` instead of a channel that reads and writes
at the wrong rate (the baud was ignored before). With `can_manager.baud =
auto` and nothing proven yet the tester's baud is taken when the bus has
been silent (2026-10-17, `can_manager_adopt_baud()`: a gatewayed OBD port
speaks only when asked, and a tester there read the refused writes as
"connected, no comm to the CAN bus"); the node runs at it until frames
prove it or the watchdog demotes it. When it cannot be taken (the bus not
silent, unreadable traffic seen, a rate this node does not know) the
channel opens and writes answer `ERR_FAILED`: one `W` line per `CONNECT`,
`tx_refused` on `/api/j2534`, and can_core's own `W transmit refused`
line names the cause. Pure rule for the check: `j2534_connect_baud_check()`.

Layout (each file under the 700-line cap):

| File | Role |
|---|---|
| `include/j2534_proto.h`, `j2534_proto.c` | PURE codec + constants: header/message encode/decode, filter match, status names, the transport-agnostic framer (`j2534_frame_read`), `j2534_ack_encode`, the TCP gate predicate. Host-tested |
| `j2534_server.c` | core: lifecycle, request dispatch (`j2534_srv_handle_frame`), periodic messages, status, the Exclusive-bus hold |
| `j2534_server_pump.c` | the RX pump (the `j2534_rx` task, its batch and scratch buffers). Out of the core on 2026-11-13: the core had grown to 743 lines, past the cap this table states; the code moved unchanged |
| `j2534_server_voltage.c` | READ_VBATT: the battery owner and checked millivolt conversion; host-tested |
| `j2534_server_transport.c` | the `j2534_transport_t` seam: session lock, send lock, coalesced frame writes, `j2534_server_serve_transport()`, the TCP listener + `allow_lan` gate, the legacy serial vtable adapter |
| `j2534_channel.c/.h`, `j2534_channel_private.h` | channels: lifecycle, CAN via `can_manager`, the filter table, the bus-bitrate rule, the `reflash_seen` count (the content gate went 2026-10-27) |
| `j2534_channel_iso.c` | the ISO15765 side (2026-10-19): one ISO-TP session per FLOW_CONTROL filter on the registered provider (`can_isotp.h`; `can_isotp_esp` in public builds), the SAE J2534-1 message layout `[4-byte id][payload]` both ways, padding per message, single frames without a session (functional addressing), the TxDone and START_OF_MESSAGE indications |
| `j2534_channel_legacy.c` | the J1850 VPW side (2026-10-19): the VPW init, header-cached WRITE through `obd_chip` (short payload as one frame, longer through `VTFullyRequestCk`), reply-line parsing with `7F xx 78` ridden out, a small PSRAM RX ring the pump drains; pure chip-command helpers live in `j2534_proto.c` |
| `j2534_server_settings.c` / `_http.c` / `_cli.c` | descriptor, `/api/j2534`, the `j2534` command |

## API

| Function | Purpose |
|---|---|
| `j2534_server_init/start/stop()` | lifecycle; `start` is a no-op when `enabled` is false |
| `j2534_server_status(out)` | `j2534_server_status_t` incl. `transport` (`tcp`/`serial`/`ble`/`none`) |
| `j2534_server_is_enabled()` | the setting (stable across runtime stop): BLE glue keys its channel registration on it |
| `j2534_server_is_running()` | started and enabled |
| `j2534_server_set_exclusive(on)` / `_exclusive()` | the runtime Exclusive-bus switch |
| `j2534_server_serve_transport(t)` | run one tester session on a `j2534_transport_t` (`name`, `read(ctx,buf,n,timeout_ms)` >0 / 0 timeout / <0 link down, `write(ctx,buf,n)` one whole frame per call). Blocks for the session; returns at once when not running; refuses with `ERR_DEVICE_IN_USE` when a tester holds the session |
| `j2534_server_set_serial_transport(t)` / `_serve_serial()` | the legacy CDC-ACM vtable, thin wrappers over the above (kept for `usb_cdc_device`) |
| `j2534_server_register_http()` | `GET/POST /api/j2534` |

A transport owner runs a task that waits for its link, drains stale
bytes, calls `serve_transport`, and throttles 200 ms when it returns
early (`usb_cdc_device.c`, `ble_j2534.c` are the two templates).

## Dependencies

`battery_monitor` (started before the server; cached ADC reading),
`can_manager`, `obd_gate` (diagnostics hold + per-conversation gate),
`obd_chip` (J1850 VPW: the ELM/STN transactions), the ISO-TP provider slot
(`can_isotp.h`), `settings_manager`, `log_manager`, `http_server_manager`,
`cmdline_manager`, lwIP, `esp_netif`, `esp_timer`, cJSON.

## Settings (`j2534_server`, version 2, reboot-to-apply)

| Key | Default | Meaning |
|---|---|---|
| `enabled` | `false` | the whole feature (TCP listener, serial and BLE sessions, the BLE characteristics) |
| `port` | `6809` | TCP listener port |
| `allow_lan` | `false` | TCP only: also accept connections arriving over WiFi STA / USB-Ethernet uplinks |
| `exclusive` | `true` | boot default of the runtime switch: AutoPID off the bus while a tester is attached |
| `cli` | `true` | register the `j2534` command |

`allow_reflash` (the ECU-flashing content gate) was removed on 2026-10-27:
version 2, `on_migrate` drops the stored key (host-tested; the schema would
also tolerate it, unknown keys are permitted, but a stored shape that
changed gets a migration per the standard §8). A backup from version 1
restores through the same migration.
The device forwards whatever a tool writes, on raw CAN, ISO15765 and
J1850; the memory-transfer services it sees are counted on `/api/j2534`
(`reflash_seen`). Why: on raw CAN the gate read every frame as ISO-TP and
refused ordinary frames of a VW TP 2.0 flasher (field report 2026-10-27),
and on ISO15765 it blocked memory reads (0x35/0x36 upload) while missing
every other write service. The owner's consent is `enabled` + `allow_lan`.

## Memory footprint (estimated; PSRAM unless noted)

| What | Size |
|---|---|
| `s_req_payload` (request payload), `s_rx_wire`, `s_tx_frame` (one whole frame) | 4384 + 4160 + 4164 B |
| `j2534_msg_t` scratch: RX pump 1, handler 4, periodic slots 4 (each ~4.2 KB) | ~38 KB |
| `s_pdu` (the ISO-TP reassembly scratch of the RX pump) | 4128 B |
| channel records `s_ch` (4 channels: filters, 4 ISO-TP session slots, the indication ring) | ~1.7 KB |
| J1850 legacy: one chip-reply buffer `s_resp` + the RX ring (8 × 520 B) | ~8.2 KB |
| ISO-TP sessions themselves: the provider's (about 25 KB PSRAM + 1.2 KB internal each while open, `can_isotp_esp` README); each takes one of `can_manager`'s 16 subscriber slots, as does every channel's own CAN queue (32 frames) | per open session |
| a TP 2.0 channel while it is open: its state and, for each of the 4 connections, a reassembly buffer and a delivery buffer of 4092 B; taken from the heap at CONNECT, freed at DISCONNECT (not a PSRAM static yet: an open item against the standard) | ~33 KB heap |
| tasks `j2534` (TCP listener) + `j2534_rx` (RX pump): 4 KB stacks each | 8 KB (+ 2 TCBs internal) |
| mutexes (send, session, channels) | internal, ~270 B |
| READ_VBATT helper | measured compiler frame: 64 B; no heap or mutable static storage |

**Never put a `j2534_msg_t` on a task stack** (three of them overflowed a
4 KB stack and reset the device, 2026-07-07). No flash writes anywhere in
this component.

## HTTP / CLI

`GET /api/j2534` (status incl. `transport`), `POST /api/j2534
{"exclusive":bool}`; `j2534` prints the same. Documented in
`components/HTTP_API.md` 6e13 and `cmdline_manager/README.md`.

## Testing

- Host: `host_test/` (36 Unity cases: codec round trips and bounds,
  filter match, status names, the framer with partial / delayed /
  dropped / malformed / discarded input, the ACK vectors of
  `J2534_WIRE_PROTOCOL.md` section 7, the TCP gate predicate, the
  CONNECT baud rule, the ISO15765 layout helpers: split / join /
  indication / single frame build and parse, and the J1850 VPW chip
  helpers: header/payload split, ATSH, the VTFullyRequestCk builder +
  checksum, reply-line decode / status / responsePending, settings migration,
  and READ_VBATT conversion, session and unavailable/invalid-sample cases,
  and the byte-exact READ_VBATT request and replies of the wire protocol's
  section 3 as a vector, and the TP 2.0 helpers: the timing byte, the
  broadcast key, where a block is sent again from, the device information
  table).
- TP 2.0: `.\test.ps1 j2534tp20` in the firmware checkout
  (`tools/testbench/usb/j2534_tp20_ps_bench.py` -> `J2534 TP20 PASS`):
  ECUs that follow SAE J2819, played by a PCAN, with faults on purpose (a
  lost telegram, "not ready", a break), a published exchange of a real
  engine ECU byte for byte, four connections, 4092-byte messages both
  ways, and the driver DLL called like an application.
- Voltage: `.\test.ps1 j2534vbat` in the firmware checkout, the wire ACK
  and Windows DLL at two real supply voltages, plus fake-device checks for
  older firmware without a result and an unavailable ADC.
- Benches (`tools/testbench/usb/`): `j2534_std_tool_bench.py` (the gate
  `.\test.ps1 j2534std`: a standard tool's byte stream through the
  installed DLL, udsoncan itself when the 32-bit Python is present ->
  `J2534 STD TOOL PASS`), `j2534_bench.py` (TCP handshake +
  `--reflash`), `j2534_transport_probe.py` (raw CAN + ISO15765 on the
  bus), `j2534_auto_baud_bench.py` (`.\test.ps1 j2534auto`),
  `j2534_gate_test.py` (no content gate: 0x34..0x37 forwarded, `reflash_seen`
  counts), `j2534_tp20_gate_bench.py` (raw CAN forwards every TP 2.0 frame
  shape, `TP20 RAW CAN PASS`), `j2534_serial_test.py`
  (CDC-ACM); (`tools/testbench/ble/`): `ble_j2534_bench.py` +
  `ble_j2534_pi.py` -> `BLE J2534 PASS`. `TESTING.md` rows.
