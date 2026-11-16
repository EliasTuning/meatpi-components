# WiCAN Pro: J2534 PassThru wire protocol (version 1)

> The one contract shared by the Windows J2534 DLL (`wican-j2534-driver`),
> the Python benches (`tools/testbench/usb/j2534_bench.py`,
> `j2534_serial_test.py`, `tools/testbench/ble/ble_j2534_pi.py`) and phone
> apps that speak PassThru over BLE. The device side is `j2534_server`
> (`include/j2534_proto.h` is the codec these tables are taken from; the
> byte-exact examples below are the host-test vectors). Transport
> bindings in section 8. Changes to `j2534_proto.h` update this file in
> the same commit.

## 0. Scope

| | |
|---|---|
| Wire version | 1 (`HELLO` answers it) |
| Transports | TCP (port 6809), USB CDC-ACM serial, BLE (stream channel FFF5/FFF6) |
| Protocols | SAE J2534-1: `CAN` (5) and `ISO15765` (6) on the native controller, `J1850VPW` (1, GM Class 2) on the OBD chip; `J1850PWM`/`ISO9141`/`ISO14230` answer `ERR_NOT_SUPPORTED` (chip-wired, not built yet). SAE J2534-2: `TP2_0_PS` (0x800E, VW TP 2.0 as SAE J2819 describes it) on the native controller, section 11 |
| Byte order | little-endian everywhere, EXCEPT the CAN id inside message data (big-endian, section 4) |
| Session | one tester at a time across all transports (section 2) |

## 1. Framing

Every message is a frame: a 12-byte header, then `length` payload bytes.

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 2 | magic | `0x4A35` (`"5J"` on the wire: `35 4A`) |
| 2 | 1 | version | `1` |
| 3 | 1 | type | section 3 |
| 4 | 2 | seq | tester's correlation id, echoed in the `ACK`; `0` in unsolicited frames |
| 6 | 2 | channel | `0` for device-level messages, else the channel id (1..4) |
| 8 | 4 | length | payload bytes that follow |

Limits: a request payload is at most `4384` bytes (`J2534_RX_CAP`: one
maximal `WRITE_MSGS`); a device frame is at most `4164` bytes
(`J2534_MAX_FRAME`: header + a 24-byte message head + 4128 data bytes).
The device reads exactly `length` bytes after every header, so frames may
be split or coalesced arbitrarily by the transport (TCP segments, USB
bulk packets, BLE notifications); reassemble by `length`, never by unit
boundaries. A header with a bad magic or version, or a `length` above the
cap, ends the session (the device logs `bad frame header` and drops the
tester): there is no in-band resync in v1, reconnect instead.

## 2. Session model

```
tester                                   device
  HELLO (seq 1)                 ->        ACK NOERROR, result = wire version
  OPEN  (seq 2)                 ->        ACK NOERROR, result = device id (always 1)
  CONNECT CAN (seq 3)           ->        ACK NOERROR, hdr.channel = result = 1
  WRITE_MSGS ch 1 (seq 4)       ->        ACK NOERROR
                                <-        RX_MSG ch 1 (seq 0, unsolicited) ...
  START_FILTER ch 1 (seq 5)     ->        ACK NOERROR, result = filter id
  DISCONNECT ch 1 (seq 6)       ->        ACK NOERROR
  CLOSE (seq 7)                 ->        ACK NOERROR
```

- `seq` is yours; the `ACK` carries it back. An `ACK` is sent after the
  operation completed on the bus side.
- `RX_MSG` frames are unsolicited (`seq 0`, `channel` = the source
  channel) and MAY arrive between a request and its `ACK`. A client
  reads frames continuously and routes by type.
- Exactly ONE tester across TCP, USB and BLE. A second tester's first
  frame is answered `ACK ERR_DEVICE_IN_USE (0x1A)` (with its `seq`), then
  that session ends (TCP: the socket closes; BLE/serial: nothing more is
  read on the link until it drops and comes back). Before 2026-09-21 the
  device closed silently; the ACK is the superset.
- A dropped link is an implicit `CLOSE`: channels, filters and periodic
  messages are torn down, the Exclusive-bus hold is released.
- The device is `enabled` per the `j2534_server` settings (default off);
  a disabled server serves nothing on any transport.

## 3. Message catalogue

Types 0x01..0x15 are tester to device; 0x80.. are device to tester.

| Type | Name | Payload (tester) | ACK `result` |
|---|---|---|---|
| `0x01` | `HELLO` | none | `u32` wire version (1) |
| `0x02` | `OPEN` | none | `u32` device id (1) |
| `0x03` | `CLOSE` | none | none; tears every channel/filter/periodic down |
| `0x04` | `CONNECT` | `protocol u32, flags u32, baud u32 [, tx_id u32, rx_id u32]` | `u32` channel id (1..4), also in `hdr.channel` |
| `0x05` | `DISCONNECT` | none, `hdr.channel` = the channel | none |
| `0x10` | `WRITE_MSGS` | `count u32`, then `count` PASSTHRU_MSGs (section 4) | none; the first failing message's status |
| `0x11` | `START_FILTER` | `type u32` (1 PASS, 2 BLOCK, 3 FLOW_CONTROL), then mask MSG, pattern MSG [, flow-control MSG] | `u32` filter id |
| `0x12` | `STOP_FILTER` | `filter_id u32` | none |
| `0x13` | `START_PERIODIC` | `interval_ms u32` (clamped to >= 5), then one MSG | `u32` periodic id |
| `0x14` | `STOP_PERIODIC` | `periodic_id u32` | none |
| `0x15` | `IOCTL` | `ioctl_id u32` [, `body_len u32`, then `body_len` bytes] | `READ_VBATT`: millivolts `u32`; `GET_CONFIG` on a TP2.0 channel and `GET_DEVICE_INFO`: a list (section 11); otherwise none |
| `0x80` | `ACK` | device: `status u32 [, result u32]` (payload 4 or 8 bytes; longer for the two list answers) | |
| `0x81` | `RX_MSG` | device: one PASSTHRU_MSG; `seq 0`, `hdr.channel` = source channel | |
| `0x82` | `EVENT` | reserved, never sent in v1 | |

`CONNECT` details: the bus parameters come from `can_manager`, and since
2026-10-03 `baud` is checked against them for CAN and ISO15765: a tester
that asks for another bitrate than the bus runs at (the device's setting,
or what its listen-before-talk policy read off the wire) gets
`ERR_INVALID_BAUDRATE` instead of a channel at the wrong rate. Whole
kbit/s decide (83333 matches the setting `83`); with `can_manager.baud =
auto` and nothing proven yet the tester's baud is TAKEN when the bus has
been silent (2026-10-17, `can_manager_adopt_baud()`): the node then runs
there in the configured mode until frames prove it or the watchdog demotes
it, and the next `CONNECT` is checked against that rate. When it cannot be
taken (the bus not silent, unreadable traffic seen earlier, or not a rate
this node knows) the channel still opens, `WRITE_MSGS` answers
`ERR_FAILED`, the device logs one `W j2534_server: ... write refused` line
per `CONNECT` and counts `tx_refused` on `/api/j2534`. `flags` bit `0x100`
(`TX_CAN_29BIT_ID`) selects 29-bit
ids; `tx_id`/`rx_id` are optional (for ISO15765 they may instead come
from a FLOW_CONTROL filter). Errors: `ERR_DEVICE_NOT_CONNECTED` before
`OPEN` or with a short payload, `ERR_NOT_SUPPORTED` for a protocol other
than CAN/ISO15765 (or no ISO-TP provider in the build), `ERR_FAILED` when
all 4 channel slots are in use.

`WRITE_MSGS`: messages are sent in order; the loop stops at the first
failure and that status is the `ACK`. ISO15765 messages carry the CAN id
in their first 4 data bytes (section 4): the id picks the ECU's session
(its FLOW_CONTROL filter, or the ids given at CONNECT); a message to an id
with no session goes out as ONE single frame when the payload is at most 7
bytes (functional addressing, 7DF) and is refused with
`ERR_NO_FLOW_CONTROL` when it is longer. Each message's
`ISO15765_FRAME_PAD` decides whether its frames are padded to 8 bytes.
The reply is an `RX_MSG` with the ECU's id first (the ISO-TP reassembly
is done for you). A successful ISO15765 transmit is followed by a TxDone
indication: an `RX_MSG` whose `rx_status` is `TX_MSG_TYPE |
TX_INDICATION` (0x09) and whose data is the 4-byte id alone; a first
frame arriving on a session's id raises a `START_OF_MESSAGE` (0x02)
indication the same way. Periodic messages raise no indications.
`ISO15765_ADDR_TYPE` (extended addressing) is not supported:
`ERR_INVALID_MSG`.

Filters (max 8 per channel): `PASS`/`BLOCK` match `mask`/`pattern` over
the message `data` bytes (the 4 id bytes first): a message matches when
`((data[i] ^ pattern[i]) & mask[i]) == 0` over `min(mask_len,
data_len)`; an empty mask passes everything. On a CAN channel with no
filter every frame passes. On an ISO15765 channel the sessions deliver
their ECUs' messages whatever the filters; single frames from ids no
session owns (the replies to a functional request) are delivered only
through a PASS filter. `FLOW_CONTROL` (ISO15765 only) opens an ISO-TP
session for one ECU: `pattern` data = the ECU's response id (4 bytes
BE), the flow-control message's data = the request id, its `tx_flags`
the 29-bit choice and the padding of the frames the device sends to
that ECU. Up to 4 sessions per channel (and 8 device-wide); a filter for
an id the channel already serves rebinds that session and its old
filter id lapses; `STOP_FILTER` closes the session; `ERR_EXCEEDED_LIMIT`
when the filter or session table is full.

`IOCTL` v1: `CLEAR_MSG_FILTERS (0x0A)` and `CLEAR_PERIODIC_MSGS (0x09)` act;
`GET_CONFIG`, `SET_CONFIG`, `CLEAR_TX_BUFFER`, `CLEAR_RX_BUFFER` answer
`NOERROR` without effect (bus timing comes from the device settings). On a
TP2.0 channel `GET_CONFIG` and `SET_CONFIG` do act (section 11).

`IOCTL` payload: the id alone (4 bytes), or the id, `body_len u32` and
`body_len` bytes. The id alone is what `READ_VBATT` has always sent and
what driver 1.7 and earlier send for every IOCTL; it stays valid. An
IOCTL with parameters (the TP2.0 ones and `GET_DEVICE_INFO`) carries
them in the body. `body_len 0` (8 bytes) means the same as the id alone;
firmware before 2026-11-17 reads the id and ignores what follows it. A
payload of 8 bytes or more whose `body_len` is larger than what follows
it is `ERR_INVALID_MSG`.

`READ_VBATT (0x03)` is device-wide: use `hdr.channel = 0` after OPEN,
without CONNECT. The ACK is `NOERROR u32, millivolts u32` (8 payload bytes).
It uses the latest calibrated, averaged battery-monitor sample, rounded to the
nearest millivolt. Before OPEN or after CLOSE it returns
`ERR_DEVICE_NOT_CONNECTED`; no sample or an invalid reading returns `ERR_FAILED`.
An error ACK has only its 4-byte status. Older firmware acknowledges this IOCTL
without a result; a client must report unsupported firmware, not invent a value.
Transport bindings are unchanged.

Byte-exact example (seq 3, channel 0), a 13.890 V reading; a host-test vector
(`host_test/main/test_voltage.c`), with the reply of a device that is not open
(`35 4a 01 80 03 00 00 00 04 00 00 00 08 00 00 00`):

```
request: 35 4a 01 15 03 00 00 00 04 00 00 00 03 00 00 00
reply:   35 4a 01 80 03 00 00 00 08 00 00 00 00 00 00 00 42 36 00 00
```

## 4. PASSTHRU_MSG on the wire

24-byte head, then `data_size` bytes:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `protocol_id` (1 J1850VPW, 5 CAN, 6 ISO15765, 0x800E TP2_0_PS) |
| 4 | 4 | `rx_status` (device to tester: `0x01 TX_MSG_TYPE` + `0x08 TX_INDICATION` = the TxDone indication after an ISO15765 or J1850 transmit, `0x02 START_OF_MESSAGE` = an ISO15765 first frame arrived, `0x100 CAN_29BIT_ID`, `0x10000 CONNECTION_ESTABLISHED` and `0x20000 CONNECTION_LOST` = the two TP2.0 indications) |
| 8 | 4 | `tx_flags` (tester to device: `0x100 CAN_29BIT_ID`, `0x40 ISO15765_FRAME_PAD`, `0x10000 TP2_0_BROADCAST_MSG`; `0x80 ISO15765_ADDR_TYPE` is refused) |
| 12 | 4 | `timestamp` (device: microseconds since boot, taken in the CAN receive interrupt for CAN frames, at reassembly for ISO15765 PDUs; wraps every 71.6 min) |
| 16 | 4 | `extra_data_index` |
| 20 | 4 | `data_size` (0..4128) |
| 24 | n | `data` |

Data conventions:

- **CAN**: `data = [4-byte BIG-ENDIAN CAN id][0..8 frame bytes]`, so
  `data_size` is 4..12. `7DF` is `00 00 07 DF`.
- **ISO15765**: `data = [4-byte BIG-ENDIAN CAN id][UDS payload]` in
  both directions, exactly as SAE J2534-1 lays a PASSTHRU_MSG out: a
  tester writes `00 00 07 E0 10 03`, the ECU's answer arrives as
  `00 00 07 E8 50 03 ...`, an indication is the 4 id bytes alone.
  `ISO15765_FRAME_PAD` in `tx_flags` pads that message's frames with
  `0xAA`. **Until 2026-10-19 the device took the bare payload and read
  the pad flag from `CONNECT`**; udsoncan and the OEM suites never did
  that, so every UDS request they sent went out mangled and timed out.
  A client written to the old convention adds the id on write and
  strips it on read.
- **J1850VPW** (GM Class 2, through the device's OBD chip): `data =
  [3-byte header][payload]` whole, with NO CAN-id prefix, both ways, as
  SAE J2534-1 carries a J1850 message (header = priority, target, source;
  e.g. `68 6A F1` for a request to the ECU, `6C F1 10` on a reply). A
  TxDone indication carries the 3 header bytes alone. The device sets the
  header on the chip (ATSH), sends a short payload as one frame and a
  longer one through the chip's `VTFullyRequestCk` segmenter, and parses
  the chip's reply lines back into messages (UDS `7F xx 78`
  responsePending interims are ridden out, not delivered). Request and
  response only: unsolicited bus traffic between requests is not surfaced,
  and GM 4x programming speed (service `$A1`, 41.6 kbit/s) is not
  implemented (the bus stays at 10.4 kbit/s; `SET_CONFIG DATA_RATE` is
  accepted but has no effect). `CONNECT` needs the OBD chip ready, else
  `ERR_FAILED`.
- **TP2_0_PS**: `data = [4-byte BIG-ENDIAN CAN id][message bytes]`, both
  ways, up to 4092 message bytes. The device puts nothing in front of the
  message and takes nothing off it (SAE J2534-2 19.3.1): the two length
  bytes a VW ECU wants in front of a message are the application's on a
  write, and the ECU's own two are the first bytes of what is read.
  Details in section 11.

## 5. Status codes

| Value | Name | WiCAN meaning |
|---|---|---|
| `0x00` | `NOERROR` | |
| `0x01` | `ERR_NOT_SUPPORTED` | a protocol this firmware does not run (J1850 PWM, ISO9141, ISO14230); until 2026-10-27 also the reflash gate (section 6, gone) |
| `0x02` | `ERR_INVALID_CHANNEL_ID` | `DISCONNECT`/data op on an unused channel |
| `0x03` | `ERR_INVALID_PROTOCOL_ID` | |
| `0x04` | `ERR_NULL_PARAMETER` | |
| `0x05` | `ERR_INVALID_IOCTL_VALUE` | a TP2.0 IOCTL or `GET_DEVICE_INFO` with a malformed body, a value out of range, a teardown of an id with no connection |
| `0x06` | `ERR_INVALID_FLAGS` | |
| `0x07` | `ERR_FAILED` | no free channel / periodic slot, timer failure, unknown message type |
| `0x08` | `ERR_DEVICE_NOT_CONNECTED` | `CONNECT` before `OPEN` |
| `0x09` | `ERR_TIMEOUT` | ISO-TP send/receive timed out |
| `0x0A` | `ERR_INVALID_MSG` | malformed PASSTHRU_MSG, short payload, unknown periodic id, extended addressing |
| `0x0C` | `ERR_EXCEEDED_LIMIT` | the channel's filter table (8) or its ISO-TP sessions (4; 8 device-wide) are all in use |
| `0x10`..`0x12` | `ERR_BUFFER_*` | |
| `0x14` | `ERR_MSG_PROTOCOL_ID` | message protocol does not match the channel |
| `0x15` | `ERR_INVALID_FILTER_ID` | |
| `0x16` | `ERR_NO_FLOW_CONTROL` | an ISO15765 message longer than one frame to an id with no FLOW_CONTROL filter (session) |
| `0x17` | `ERR_NOT_UNIQUE` | |
| `0x18` | `ERR_INVALID_BAUDRATE` | `CONNECT` (CAN, ISO15765) with a baud the bus does not run at |
| `0x19` | `ERR_INVALID_DEVICE_ID` | |
| `0x1A` | `ERR_DEVICE_IN_USE` | a tester is attached on another transport |
| `0x00010001` | `ERR_NO_CONNECTION_ESTABLISHED` | a TP2.0 message longer than one CAN frame to an id with no connection (SAE J2534-2 table 123: its own return values start at 0x10000) |

## 6. Device behaviours

- **Exclusive bus** (setting `exclusive`, default on; runtime switch
  `POST /api/j2534 {"exclusive":bool}`): on tester attach the device asks
  AutoPID (PID polling + DTC scans) off the bus and waits up to 700 ms
  for the acknowledgement before processing the first frame;
  `/api/j2534` shows `autopid_paused`. Released on detach.
- **No content gate** (2026-10-27): the device forwards whatever the tool
  writes, on every channel type. Until then a closed `allow_reflash`
  setting refused UDS `0x34..0x37` with `ERR_NOT_SUPPORTED` on ISO15765
  and, by reading every raw CAN frame as an ISO-TP single/first frame, on
  raw CAN as well; that guess refused ordinary frames of tools with their
  own framing (a VW TP 2.0 flasher, "randomly"), and the ISO15765 check
  blocked memory reads (`0x35`/`0x36` upload) while missing every other
  write service. The owner's consent is `enabled` (the server is off by
  default) and `allow_lan`. For observability the memory-transfer services
  seen in ISO15765 and J1850 requests are counted on `/api/j2534`
  (`reflash_seen`, since boot). A stored `allow_reflash` key is ignored.
- **ISO-TP sessions**: one per FLOW_CONTROL filter (ECU), 4 per channel,
  8 across the device, one per rx id; the device's own UDS terminal keeps
  its session 5 s after its last request, so a filter for that ECU
  inside those 5 s fails (`ERR_FAILED`). Retry. The flow control the
  device sends as a receiver is BS 8 / STmin 1 ms; N_Bs and N_Cr are
  1 s (ISO 15765-2). A multi-frame write waits for its last frame
  before the `ACK` (about 10 ms per frame at the ECU's STmin, capped at
  5.5 s).
- Limits: 4 channels, 8 filters per channel, 4 periodic messages,
  periodic interval >= 5 ms. Received messages are pushed as soon as the
  RX pump sees them (no device-side queue beyond the channel's 32-frame
  CAN queue): drain promptly.
- Interface exposure (TCP only): with `allow_lan` off the listener
  accepts connections that landed on loopback, WiCAN's own AP or the USB
  network link, and refuses the shop LAN (WiFi STA / USB-Ethernet).
  Serial and BLE never consult it (a cable / a paired link).

## 7. Worked examples (hex, little-endian; `|` separates header, fields)

```
HELLO seq 1
  -> 35 4A 01 01 01 00 00 00 00 00 00 00
  <- 35 4A 01 80 01 00 00 00 08 00 00 00 | 00 00 00 00 | 01 00 00 00     (NOERROR, version 1)

OPEN seq 2
  -> 35 4A 01 02 02 00 00 00 00 00 00 00
  <- 35 4A 01 80 02 00 00 00 08 00 00 00 | 00 00 00 00 | 01 00 00 00     (device id 1)

CONNECT CAN seq 3 (protocol 5, flags 0, baud 500000 = 0x0007A120)
  -> 35 4A 01 04 03 00 00 00 0C 00 00 00 | 05 00 00 00 | 00 00 00 00 | 20 A1 07 00
  <- 35 4A 01 80 03 00 01 00 08 00 00 00 | 00 00 00 00 | 01 00 00 00     (channel 1)

WRITE_MSGS seq 4, ch 1, one CAN message 7DF [02 01 00]  (payload 4 + 24 + 7 = 35 = 0x23)
  -> 35 4A 01 10 04 00 01 00 23 00 00 00 | 01 00 00 00 |
     05 00 00 00  00 00 00 00  00 00 00 00  00 00 00 00  00 00 00 00  07 00 00 00 |
     00 00 07 DF 02 01 00
  <- 35 4A 01 80 04 00 01 00 04 00 00 00 | 00 00 00 00                   (NOERROR)

RX_MSG (the ECU's answer, DLC 8; tt = timestamp, pp = pad byte)
  <- 35 4A 01 81 00 00 01 00 24 00 00 00 |
     05 00 00 00  00 00 00 00  00 00 00 00  tt tt tt tt  00 00 00 00  0C 00 00 00 |
     00 00 07 E8 06 41 00 18 3F 80 03 pp

START_FILTER seq 5, ch 1, PASS 7E8 (mask FF FF FF FF, pattern 00 00 07 E8)
  -> 35 4A 01 11 05 00 01 00 3C 00 00 00 | 01 00 00 00 |
     <mask MSG: head with data_size 4 | FF FF FF FF> <pattern MSG: head with data_size 4 | 00 00 07 E8>
  <- 35 4A 01 80 05 00 01 00 08 00 00 00 | 00 00 00 00 | 01 00 00 00     (filter id 1)

CONNECT ISO15765 seq 6 (protocol 6, flags 0, baud 500000; no ids: a FLOW_CONTROL filter follows)
  -> 35 4A 01 04 06 00 00 00 14 00 00 00 | 06 00 00 00 | 00 00 00 00 | 20 A1 07 00 | 00 00 00 00 | 00 00 00 00
  <- ACK NOERROR, channel 2
START_FILTER seq 7, ch 2, FLOW_CONTROL: mask FF FF FF FF, pattern 00 00 07 E8, flow control 00 00 07 E0
  (the three MSG heads carry tx_flags 40 00 00 00: the session pads its frames)   -> ACK NOERROR, filter id 1
WRITE_MSGS ch 2: one ISO15765 message, tx_flags 0x40, data 00 00 07 E0 22 F1 90   -> ACK NOERROR
  <- RX_MSG ch 2, rx_status 09 00 00 00, data 00 00 07 E0                              (TxDone)
  <- RX_MSG ch 2, rx_status 02 00 00 00, data 00 00 07 E8                              (START_OF_MESSAGE: a first frame)
  <- RX_MSG ch 2, data 00 00 07 E8 62 F1 90 31 57 43 41 4E 30 46 57 30 50 30 30 30 30 30 30 31   (VIN 1WCAN0FW0P0000001)
  on the bus: 7E0 [03 22 F1 90 AA AA AA AA], then the ECU's first frame, the device's flow control, the consecutive frames

A memory-transfer service: WRITE_MSGS ch 2, data 00 00 07 E0 34 00 44 00 00 00 04 00
  <- 35 4A 01 80 07 00 02 00 04 00 00 00 | 00 00 00 00                   (NOERROR: forwarded, /api/j2534 reflash_seen +1;
                                                                          until 2026-10-27 the closed gate answered 01 00 00 00)

Second tester's HELLO (seq 4) while one is attached
  <- 35 4A 01 80 04 00 00 00 04 00 00 00 | 1A 00 00 00                   (ERR_DEVICE_IN_USE)

CLOSE seq 9
  -> 35 4A 01 03 09 00 00 00 00 00 00 00
  <- 35 4A 01 80 09 00 00 00 04 00 00 00 | 00 00 00 00
```

## 8. Transport bindings

### 8.1 TCP

Port `j2534_server.port` (6809), `TCP_NODELAY`. The peer closing the
socket ends the session. Subject to the `allow_lan` gate (section 6).
Reference client: `tools/testbench/usb/j2534_bench.py`.

### 8.2 USB CDC-ACM serial

`usb_host_manager.device_class = cdc`: the WiCAN enumerates as a serial
port; asserting DTR starts the session, any baud rate. Reference:
`tools/testbench/usb/j2534_serial_test.py`.

### 8.3 BLE

Stream channel `j2534` under service 0xFFF0: **FFF5 = device to tester
(indicate: enable indications on its CCCD, each PDU is confirmed by your
stack)**, **FFF6 = tester to device (write / write-without-response)**.
Present in the GATT table only while `j2534_server.enabled` is true
(reboot-to-apply). Requires the paired, encrypted, MITM-authenticated
link like every WiCAN characteristic (`BLE_API.md` section 2).

- Write frames to FFF6 in ATT units of at most `min(490, MTU - 3)`
  bytes; the device reassembles by `length`. Its notifications on FFF5
  are at most that size too and carry frames back to back.
- The device's receive buffer holds 9 KB (two maximal requests): do not
  pipeline more than one request beyond the one outstanding when using
  write-without-response.
- A session starts when the link is secured and ends when it drops
  (implicit `CLOSE`); reconnect to start another.
- `/api/j2534` reports `"transport":"ble"` while a BLE tester is
  attached (`"tcp"`, `"serial"`, `"none"` otherwise).
- While a phone is connected over BLE the device suspends WiFi by
  default (`interface_manager.sta_ble_handover`), so a companion HTTP
  witness must use USB or turn that rule off.

Reference: `tools/testbench/ble/ble_j2534_pi.py` (bleak).

## 9. Observability

`GET /api/j2534`: `enabled, port, listening, client_connected,
device_open, channels, frames_rx, frames_tx, tx_refused, reflash_seen, allow_lan,
exclusive, autopid_paused, transport, phase`. `POST /api/j2534
{"exclusive":bool}`. Console: `j2534`. Logs (tag `j2534_server`):
`tester connected (ble)` / `tester disconnected (ble)` at I, refusals
(`tester already attached on tcp; refusing ble`, `bad frame header`) at W.

## 10. Versioning

`HELLO` returns the wire version; a client refuses to proceed on a
version it does not know. Any change to the header, a payload layout or
a status meaning bumps `J2534_WIRE_VERSION` in `j2534_proto.h` and this
document together.

2026-10-19, still version 1: the bytes of every frame and of the
PASSTHRU_MSG head are unchanged; what changed is the meaning of an
ISO15765 message's `data` (section 4: the id first, as J2534-1 says) and
the device now sends the indications above. The Windows DLL copies
messages verbatim and needed no change; the bench scripts and the BLE
bench moved with the device.

2026-11-17, still version 1: TP2.0 (section 11) added a protocol id, two
IOCTLs with a body, `GET_DEVICE_INFO`, two `rx_status` bits, one
`tx_flags` bit and two status codes. Nothing a client of the day before
sends changed meaning: an IOCTL that is its id alone is read as before.
The one edge is a client that put parameters straight behind an IOCTL id
without a length (none is known): its second word is now read as
`body_len`.

## 11. TP2.0 (`TP2_0_PS`, protocol 0x800E)

VW's TP 2.0 transport as its own J2534 protocol (SAE J2534-2 section 19;
the transport itself is SAE J2819). The device sets a connection up,
splits a message into telegrams and puts a received one together,
acknowledges blocks, sends again what the ECU asks for again and tests an
idle connection; the tester sees whole messages and two indications. A
raw `CAN` channel does none of this: a tool that does the transport
itself keeps using protocol 5.

`CONNECT` with protocol `0x800E` opens the channel. Up to four
connections live on it at once (`ERR_EXCEEDED_LIMIT` for a fifth).
`DISCONNECT` sends a disconnect (`A8`) on every connection first.

**IOCTLs on the channel** (payload: id, `body_len`, body; section 3):

| IOCTL | Body | Answer |
|---|---|---|
| `SET_CONFIG (0x02)` | `count u32`, then `count` pairs `parameter u32, value u32` (at most 24) | status; the first refused pair ends the request |
| `GET_CONFIG (0x01)` | the same list, values ignored | `status u32`, then `count u32` and the pairs with the values filled |
| `REQUEST_CONNECTION (0x800A)` | `u32 11`, then the 11 bytes of the SBYTE_ARRAY (below) | status at once; the outcome is an indication |
| `TEARDOWN_CONNECTION (0x800B)` | `u32 4`, then the connection's RX id, 4 bytes big-endian | status; `ERR_INVALID_IOCTL_VALUE` when no connection has that id |

Config parameters (SAE J2534-2 table 77; times in ms):

| Id | Name | Default | Note |
|---|---|---|---|
| `0x01` | `DATA_RATE` | - | kept and read back; the bus rate comes from the device settings |
| `0x03` | `LOOPBACK` | 0 | 0 or 1 |
| `0x17`, `0x18` | `BIT_SAMPLE_POINT`, `SYNC_JUMP_WIDTH` | - | kept and read back |
| `0x8001` | `J1962_PINS` | `0x060E` | pins 6 and 14, the one pair this hardware has; any other value is `ERR_NOT_SUPPORTED` |
| `0x8044` | `TP2_0_T_BR_INT` | 20 | gap between the telegrams of a broadcast |
| `0x8045` | `TP2_0_T_E` | 100 | wait for the answer to a setup telegram |
| `0x8046` | `TP2_0_MNTC` | 10 | tries of a setup telegram |
| `0x8047` | `TP2_0_T_CTA` | 1000 | one connection test per this time on an idle connection |
| `0x8048` | `TP2_0_MNCT` | 5 | unanswered connection tests before the connection counts as lost |
| `0x8049` | `TP2_0_MNTB` | 5 | times a block is sent again when the ECU names a telegram in it |
| `0x804A` | `TP2_0_MNT` | 2 | times a telegram is sent again when no acknowledge comes |
| `0x804B` | `TP2_0_T_WAIT` | 100 | wait after "receiver not ready" |
| `0x804C` | `TP2_0_T1` | 100 | wait for an acknowledge |
| `0x804D` | `TP2_0_T3` | 0 | gap between telegrams, the value the device states in its connection setup |
| `0x804E` | `TP2_0_IDENTIFER` | 0 | the tester's own address, for a connection the ECU asks for |
| `0x804F` | `TP2_0_RXIDPASSIVE` | 0 | 0, or 0x300..0x7FF |

Any other parameter is `ERR_NOT_SUPPORTED`.

**`REQUEST_CONNECTION`**, the 11 bytes (SAE J2534-2 table 78): bytes 0
to 3 the CAN id the setup request goes out on, big-endian (`00 00 02 00`);
byte 4 the ECU's address; byte 5 the opcode, `C0`; bytes 6 and 7 the TX
id offered; bytes 8 and 9 the RX id asked for (low byte first; bit 4 of
the second byte set means "not given"); byte 10 the application type. Bytes 4 to 10 are the setup telegram as it goes on the bus
(`01 C0 00 10 00 03 01` on `200` asks ECU 1 for a channel the tester
receives on `300`); the answer is expected on `0x200 + address`. Refused
at once with `ERR_INVALID_IOCTL_VALUE` (not 11 bytes, another opcode, id
0), `ERR_NOT_UNIQUE` (the address or an id is already in use) or
`ERR_EXCEEDED_LIMIT`. Otherwise the ACK is `NOERROR` and the outcome
comes as an `RX_MSG` indication on the channel:

- `rx_status 0x10000` (`CONNECTION_ESTABLISHED`), `data` = the RX id then
  the TX id, 4 bytes big-endian each: the ids the ECU gave. Messages are
  written to the TX id and read from the RX id.
- `rx_status 0x20000` (`CONNECTION_LOST`), `data` = the RX id (4 bytes
  big-endian) and one reason byte: `01` nothing answered (the setup after
  `MNTC` tries, the connection tests, a block), `00` a disconnect (the
  tester's `TEARDOWN_CONNECTION` or the ECU's own), `D6` to `D8` the
  ECU's negative answer to the setup, passed on. A setup that fails is
  reported with the RX id that was asked for.

**`WRITE_MSGS`**, by the id in the first four data bytes:

- The TX id of a connection: the bytes behind the id are the message, at
  most 4092 (`ERR_INVALID_MSG` above that, nothing sent). They go out as
  written in telegrams of 7 bytes; the id alone is one empty
  end-of-message telegram. The ACK comes when the ECU has acknowledged
  the last block. `ERR_TIMEOUT`: no acknowledge after the repeats, the
  connection is then reported lost. `ERR_FAILED`: the ECU sent a break;
  the connection stays. No TxDone indication follows.
- `tx_flags 0x10000` (`TP2_0_BROADCAST_MSG`): a broadcast request, five
  telegrams `T_BR_INT` apart, the last two bytes of each the key (`55 55`,
  then `AA AA`, changing with every telegram; SAE J2819 5.1.2).
  `ERR_INVALID_MSG` when the id is not a broadcast address.
- Any other id: up to 8 bytes go out as one CAN frame; a longer message
  is `ERR_NO_CONNECTION_ESTABLISHED (0x00010001)`.

`START_PERIODIC` with the broadcast flag is the re-triggered broadcast
(SAE J2534-2 19.3.2.3): the five telegrams at the start, then one per
interval, the key changing over the whole run.

**Received messages**: one `RX_MSG` per message, `data` = the RX id and
everything the ECU sent up to its end-of-message telegram. A message
longer than 4092 bytes is acknowledged on the bus and not delivered (one
W line). A frame that belongs to no connection is delivered as on a CAN
channel when a PASS filter of the tester matches it.

**`GET_DEVICE_INFO (0x800C)`** is device-wide (`hdr.channel = 0` after
`OPEN`; SAE J2534-2 section 25). Body: `count u32` (at most 16), then
`count` pairs `parameter u32, value u32`. ACK: `status u32`, `count u32`,
then `count` triples `parameter u32, value u32, supported u32`. A
parameter the device does not have comes back with `supported 0` and its
value as sent; the request is still `NOERROR`. Answered:
`J1850VPW_SUPPORTED (0x03)`, `CAN_SUPPORTED (0x06)`,
`ISO15765_SUPPORTED (0x07)` and their `_SIMULTANEOUS` (`0x36`, `0x39`,
`0x3A`) with value `0x00000001`; `TP2_0_SUPPORTED (0x13)` and
`TP2_0_SIMULTANEOUS (0x46)` with `0x00000100` (one `TP2_0_PS` channel);
`TP2_0_PS_J1962 (0x28)`, where the value is the question (CAN-H pin in
the high word, CAN-L pin in the low word, bit 0 = pin 1) and `supported`
is 1 for `0x00202000` (pins 6 and 14) alone.

Not proven on a bench: the connection an ECU asks the tester for
(`TP2_0_IDENTIFER`, `TP2_0_RXIDPASSIVE`) and loopback. Not built:
`GET_PROTOCOL_INFO`. Not run on a real ECU yet (2026-11-17). Reference
client and proof: the stage `j2534tp20` of
the firmware repo (`tools/testbench/usb/j2534_tp20_ps_bench.py`, ECUs
that follow SAE J2819 played by a PCAN; `testing/j2534tp20.md`).
