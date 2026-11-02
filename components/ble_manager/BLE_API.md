# WiCAN Pro: BLE GATT interface reference

> The on-air contract for third-party apps (Android / iOS / desktop), the
> BLE counterpart of `components/HTTP_API.md`: anyone can build their own
> app, there is no privileged client. Byte-exact with the legacy WiCAN
> Pro firmware for the four original characteristics, so existing apps
> keep working unchanged; the 2026-09-21 firmware APPENDS stream channels
> that carry the whole HTTP API (`ble_http`) and SAE J2534 PassThru
> (`ble_j2534`) over BLE. Facts below come from the implementation
> (`ble_manager_gatt_svc.c`, `ble_manager_gatt_nimble.c`,
> `ble_manager_channel.c`) and are verified on air against BlueZ/bleak by
> the benches in `tools/testbench/ble/` (working client code to crib from).
>
> Companion documents: `ble_http/BLE_HTTP_PROTOCOL.md` (the HTTP tunnel),
> `j2534_server/J2534_WIRE_PROTOCOL.md` (PassThru). Any change to a
> characteristic or a channel protocol updates these files in the same
> change (coding standard 6c).

## 1. Discovery

| | |
|---|---|
| Advertised name | `WiC_<device_id>`: `device_id` = 12 lowercase hex (SoftAP MAC), e.g. `WiC_14c19f44e349` (legacy ble_uid format; serial 2A25 = name+7 = last 9 hex) |
| Advertised service UUID | `0000FFF0-0000-1000-8000-00805F9B34FB` (128-bit, complete list) |
| Advertising interval | 160 ms, general discoverable, connectable |
| Advertising sets | `advertising` setting (2026-09-21): `legacy` (default) = the 4.2 ADV_IND set above (flags + TX power + the FFF0 UUID; name + `MeatPi` manufacturer data in the scan response), `extended` = one BLE 5 extended PDU carrying flags + UUID + name + `MeatPi` (connectable, primary PHY 1M, secondary PHY per `phy`; 4.2-only scanners do NOT see it), `both` = the two sets at once (the same device shows up as two advertising reports; the legacy set keeps every existing app working) |
| PHY | `phy` setting: `1m` (default, the 4.2 behaviour), `2m`, `coded`, `auto`. The device asks for the PHY right after connect; the central decides (section 6) |
| Address | `address` setting (2026-11-02): `public` (default) = the device's **fixed hardware address** (the station MAC + 2), what firmware 4.x advertised: presence detection by MAC and an app that remembers the address work; `private` = a **resolvable private address** that changes at every advertising restart (so after every disconnect) and every 15 min; a bonded central resolves it to the identity address, which is the public one. Scanning by NAME or the advertised service UUID works under both |
| TX power | `tx_power_dbm` setting (default 9 dBm) |

BLE is **disabled by default** (`ble_manager.enabled = false`): the user
enables it via `/api/settings/ble_manager` (reboot to apply). Advertising
stops while a client is connected (one central at a time) and resumes on
disconnect.

**Feature detection is by discovery.** Which characteristics exist depends
on the device's settings (section 3.2, column "present when"): an app
enumerates the FFF0 service and uses what it finds instead of assuming.

## 2. Pairing and security

- **LE Secure Connections + MITM**, device role DisplayOnly: the phone
  prompts for a **6-digit passkey**, default `123456`, the `passkey`
  setting. **The device asks for the pairing at the connect** (an LL
  Security Request one second after the connect if the link is still
  plain by then and the pairing window is open, as firmware 4.x did; the
  `security_request` setting, default true, 2026-11-02; a central that
  pairs or encrypts on its own first gets no request): the
  phone pairs right away, or encrypts with its bond, so an app's first
  write lands on a secured link. A central that starts first is fine. The
  request had been removed on 2026-09-21 because a peripheral-driven start
  makes **Windows** begin a system pairing that a desktop app's own
  ceremony cannot join (WinRT custom pairing fails with no ceremony
  asked): such an app sets `security_request` false and pairs with
  `DevicePairingKinds.ProvidePin`, answering with the passkey
  (`ble_phy_pc.py` is the reference). With the request off, the central
  starts the pairing on its first secured access (every phone does this
  on the *Insufficient Authentication* answer).
- **Every** characteristic requires an **encrypted + authenticated** link
  (`ENC` + `AUTHEN` on reads and writes, incl. Device Info, and since
  2026-11-02 on the **subscribe** too: the CCCD write of FFF1, FFF3 and
  FFF5 answers *Insufficient Authentication* on a plain link, as the 4.x
  table did, so an app that subscribes first pairs at that step). An
  unpaired connection can discover the layout but read, write or
  subscribe nothing. Note what a plain link does NOT get: an ATT
  write-without-response has no error reply, so a command written to
  FFF2 before the pairing is dropped silently (NimBLE refuses it before
  the device's own code sees it). Apps built for 4.x never met this
  because the device had asked for the pairing at the connect; on the
  v6 betas before 2026-11-02 it was the "no notification comes back"
  report.
- **Bonding** (`bonding` setting, default true): the pairing exchanges
  long-term keys so a paired phone reconnects **without re-entering the
  passkey**, and the keys are **persisted to NVS** across reboots and
  power cycles. Off = the link is still encrypted + MITM-protected every
  session but nothing reusable is kept; every reconnect re-pairs. Up to 3
  bonded phones; a 4th evicts the oldest.
- **Secure Connections only** (`sc_only`, default true): refuses a peer
  that tries to downgrade to LE legacy pairing. Every phone since ~2014
  does SC; turn off only for a genuinely pre-4.2 accessory.
- **Pairing window**: `pairing_at_boot` (default true) opens the window at
  boot; firmware can close it at runtime (`ble_manager_pairing_enable()`
  / `ble_manager_pairing_disable()`). Closed = NEW pairings are rejected;
  a reconnect by an already-bonded phone is unaffected.
- **Bond loss recovery**: the device keeps its bond across reboots; after
  a **re-flash / factory reset** (clears NVS) the phone must Forget/Remove
  the device and re-pair, else its stale bond fails at service discovery
  with an immediate disconnect. The device handles the reverse (phone
  re-pairs, device drops its stale entry).
- **GATT table growth (2026-09-21)**: the stream channels are appended
  after the four legacy characteristics, so FFF1/FFF2/CLI keep their
  attribute handles and an app with a cached database keeps working. It
  will not SEE FFF3..FFF6 until it re-discovers. iOS caches aggressively:
  if discovery shows only the old table after a firmware update, Forget
  This Device and pair again. Android: clear the app's cached
  characteristics (a fresh `connectGatt` after `createBond`, or call the
  hidden `refresh()`). Linux/BlueZ: `bluetoothctl remove <identity>`
  (the cache held FFF3 as "notify" from an older build and StartNotify
  then failed with NotSupported, 2026-09-21). The device does not send a
  Service Changed indication across reboots in this version.

## 3. GATT services

### 3.1 Device Information: `0x180A` (read-only)

Values are served with their **historical lengths**: strings include the
NUL terminator, the serial is space-padded to 32 bytes. Trim trailing
`\0` / spaces before comparing.

| Characteristic | UUID | Value (bytes on air) |
|---|---|---|
| Manufacturer Name | `0x2A29` | `MEATPI.COM\0` (11) |
| Model Number | `0x2A24` | `WiCAN-PRO\0` (10) |
| Serial Number | `0x2A25` | `<device_id minus its FIRST hex char>` space-padded to 32, e.g. id `14c19f44e349` gives `4c19f44e349` + 21 spaces (legacy `name+7` quirk, kept byte-exact) |
| Hardware Revision | `0x2A27` | `1_53         \0` (14) |
| Firmware Revision | `0x2A26` | `400\0` (4) |
| Software Revision | `0x2A28` | `0000\0` (5) |
| System ID | `0x2A23` | 8 x `0x00` |
| IEEE Reg. Cert. | `0x2A2A` | 8 x `0x00` |

### 3.2 Data, CLI and stream channels: `0xFFF0`

All 16-bit UUIDs below sit on the Bluetooth base
(`0000xxxx-0000-1000-8000-00805F9B34FB`). Odd = device to app (notify),
even = app to device (write).

| Characteristic | UUID | Properties | Direction | Present when |
|---|---|---|---|---|
| **Data OUT** | `0xFFF1` | notify, indicate (read returns one dummy `0x00`) | device to app | always |
| **Data IN** | `0xFFF2` | write, write-without-response | app to device | always |
| **CLI OUT** | `0200DEC0-01EF-BC9A-5678-1234DEADF0BE` | notify, indicate (read = dummy) | device to app | always |
| **CLI IN** | `0300DEC0-01EF-BC9A-5678-1234DEADF0BE` | write, write-without-response | app to device | always |
| **HTTP OUT** | `0xFFF3` | **notify, indicate** (read = dummy): your CCCD picks the mode, see 4.3 | device to app | `ble_http.enabled` (default true) |
| **HTTP IN** | `0xFFF4` | write, write-without-response | app to device | `ble_http.enabled` |
| **J2534 OUT** | `0xFFF5` | **indicate** (read = dummy) | device to app | `j2534_server.enabled` (default false) |
| **J2534 IN** | `0xFFF6` | write, write-without-response | app to device | `j2534_server.enabled` |

Subscribe (write the CCCD, i.e. `setNotifyValue` / `enableNotifications`)
to an OUT characteristic before expecting anything back on it. On a
stream channel the CCCD value is a choice (since 2026-09-22): **`0x0002`
indications** = every PDU confirmed by your stack before the next one,
no client-side flow control, slow; **`0x0001` notifications** (FFF3 only)
= the radio's full rate, and the channel's protocol then requires you to
send credits and to check its frame counter (`BLE_HTTP_PROTOCOL.md` 4b).
`ENABLE_INDICATION_VALUE` / `ENABLE_NOTIFICATION_VALUE` on Android; iOS
`setNotifyValue(true)` and BlueZ pick notifications when the property is
offered, so on FFF3 they are in notify mode. FFF5 (J2534) stays
indicate-only. `GET /api/ble` `channels[].out` shows which mode the device
is in. The legacy data pipe (FFF1) keeps plain notifications for the ELM
dialect, which tolerates a drop. All writes carry `WRITE_ENC |
WRITE_AUTHEN`; the device additionally refuses writes in software until
the link is secured.

## 4. The data pipe (FFF2 to device to FFF1)

A transparent **byte stream**, no framing added by BLE. What sits on the
other end is a `bridge_manager` configuration: out of the box (bridge_manager
v2, 2026-11-02) the OBD chip is bridged to Bluetooth by the factory
connection `br_ble_obd` (`{"name":"br_ble_obd","a":"obd","b":"ble"}`),
beside WebSocket, TCP 35000 and USB on the chip's fan-out, so pairing an
app is all it takes, as on 4.x (a device updated from an earlier v6 gets
the row once unless a connection of its own already uses `ble`). The
`ble` end carries one enabled connection: `{"a":"can","b":"ble",
"translator":"slcan"}` gives Lawicel slcan over BLE instead, with
`br_ble_obd` disabled. Without any `ble` connection, writes to FFF2 go
nowhere and nothing is notified on FFF1 (the v6 betas before this).

With the OBD bridge active the dialect is **ELM327/STN ASCII**:

```
app writes  FFF2: "ATI\r"
app notified FFF1: "ELM327 v2.3\r\r>"      <- accumulate until '>' at line start
app writes  FFF2: "0100\r"
app notified FFF1: "41 00 FF FF FF FF \r\r>"
```

- **Notification payload cap**: `min(490, MTU - 3)` bytes (section 6).
- A logical reply may span several notifications: reassemble by the
  dialect's terminator (`>` for ELM), never by packet boundaries.
- App-to-device writes are capped at 490 bytes per write (larger ones are
  rejected with an ATT error): chunk to `<= MTU - 3`.

### 4.1 Dialects available on the data pipe

The pipe carries whatever the configured bridge row puts on it. Every row
is `{"name","a","b":"ble","translator"}` in `/api/settings/bridge_manager`
(reboot to apply; the `ble` jack feeds ONE bridge at a time):

| Row (`a`, `translator`) | Bytes per CAN frame | Dialect | For |
|---|---|---|---|
| `obd`, `raw` | ASCII lines | ELM327/STN over the OBD chip (section 4) | Car Scanner, Torque, any ELM app |
| `can`, `slcan` | 22 to 27 | Lawicel slcan ASCII (`tIIIL DD..\r`, `TIIIIIIIIL..\r`) | slcan tools, `ble_slcan_bridge_test.py` |
| `can`, `gvret` | 20 | SavvyCAN GVRET binary (`F1 00 ts:4 id:4 dlc data xor`), with the handshake answered by the device | SavvyCAN relays |
| `can`, `realdash` | 20 | RealDash `66 33 22 11 id:4 data:8 crc32:4` | RealDash |
| **`can`, `raw`** | **10 + dlc (10 to 18)** | **the timestamped binary CAN record below** | your own app: the most compact stream the device offers, with bus time |

### 4.2 The binary CAN record (`can <-raw-> ble`)

```
off 0  u32 LE  CAN identifier (11-bit or 29-bit)
off 4  u8      flags: bit0 = extended (29-bit) id, bit1 = RTR; other bits 0
off 5  u8      dlc (0..8)
off 6  u32 LE  ts_us: receive time in microseconds since device boot, taken
               in the CAN receive interrupt; wraps every 71.6 min (compute
               deltas modulo 2^32); 0 = not available
off 10 dlc B   data (absent when RTR or dlc 0)
```

One record per CAN frame, 10 to 18 bytes, device to app AND app to device
(write a record to FFF2 and the frame goes on the bus; set `ts_us` to 0,
the device ignores it). The timestamp is the device's bus-side time, so
inter-frame timing survives BLE's batching and jitter: use it for logging,
replay and signal-period analysis; the phone's receive time is only good
for live displays. Records are packed back to back into notifications up
to the payload cap and a record MAY straddle two notifications: parse a
byte stream by `10 + dlc`, never one record per packet. Sanity rules a
parser should enforce: `dlc <= 8`, `flags & ~3 == 0`, `id <= 0x7FF` unless
extended, `ts_us` non-decreasing modulo 2^32 within a session.

Worked example, the ECU's `0100` answer received 1.234567 s after boot
(one 18-byte record as it appears inside an FFF1 notification):

```
E8 07 00 00   id 0x7E8 (u32 LE)
00            flags: 11-bit, data frame
08            dlc 8
87 D6 12 00   ts_us 0x0012D687 = 1234567 us
06 41 00 18 3F 80 03 00   data
```

A 29-bit frame `18DB33F1 [02 01 00]` written BY the app to FFF2 (ts 0):

```
F1 33 DB 18   id 0x18DB33F1
01            flags: extended
03            dlc 3
00 00 00 00   ts_us 0 (the device ignores it on the way to the bus)
02 01 00      data
```

Python: `struct.pack("<IBBI", can_id, flags, dlc, 0) + data` to build,
`struct.unpack_from("<IBBI", buf)` then `buf[10:10+dlc]` to parse.
Kotlin: `ByteBuffer.wrap(bytes).order(LITTLE_ENDIAN)`; Swift:
`withUnsafeBytes { $0.load(fromByteOffset:as:) }` with `UInt32(littleEndian:)`.

Delivery on this pipe is
by notification (the ELM/slcan contract): under a flood the device drops
whole records at its TX queue (`br_ble_can` `stats` on `/api/bridges`,
`tx queue full` in its log) rather than blocking the bus, so a CAN
monitor app must tolerate gaps; a lost notification can also cost the
tail of a straddling record, which the sanity rules above catch (skip one
byte and re-sync). Bench 2026-09-21 (`ble_can_raw_pi.py`, row `br_ble_can`
= `can` / `raw`, UB500 central, WiFi STA up): the pre-timestamp layout
ran at exactly 14.0 bytes per record with 0 malformed and 0 straddling
records over 88 notifications, 20 of 20 `7DF 0100` requests answered from
`7E8` with a p50 of 176 ms request-to-record, a 29-bit record on the bus;
the timestamped layout's run is in `TESTING.md` (row `blecanraw`). The record format is frozen
with this version (`can_manager/include/can_frame_wire.h`); any change
would be a new translator name, never a silent edit.

### 4.3 Stream channels: shared conventions (FFF3..FFF6)

The HTTP and J2534 characteristics are **stream channels**: each pair is
one reliable, in-order byte stream in each direction and nothing more.
The protocol carried on it (documented by its owner) frames itself with a
length-prefixed header.

- A frame may span several ATT writes or notifications, and one unit may
  hold the tail of one frame and the head of the next. **Reassemble by
  the protocol's length field, never by unit size.**
- Write in units of at most `min(490, MTU - 3)` bytes; PDUs from the
  device are at most that size too. On **indications** one is in flight
  at a time (your stack confirms each before the next arrives; the device
  waits up to the ATT indication timeout, 30 s, because a stack confirms
  only after the writes it has already queued went out). On
  **notifications** (FFF3 with CCCD `0x0001`) the controller packs
  several PDUs per connection event and the owner protocol carries the
  flow control and a per-frame counter, because a stack CAN drop a PDU it
  already accepted: measured 2026-09-21/22 on the device (the host counted
  notifications that never reached the air; the controller's ACL TX buffer
  allocation from a starved heap, fixed in the firmware with static
  buffers) and on a Pi (BlueZ's D-Bus notification path). Indications are
  acknowledged end to end;
  notifications are 2x-10x faster and every loss is detected by the
  counter, so the app repeats the request.
- Write-without-response is fastest but has no acknowledgement; the
  device's receive buffer per channel is finite (HTTP 16 KB, J2534
  9 KB). Either use write-with-response, or honour the owner protocol's
  flow-control rule (HTTP: `CREDIT` frames; J2534: at most one request
  pipelined beyond the outstanding one).
- Bytes written before the link is secured are rejected; bytes left over
  when a link drops are discarded, never delivered to the next central.
- One central at a time; one request in flight per channel.
- On a protocol error the device ends the exchange (HTTP: an error
  response; J2534: the session) rather than guessing; reconnect to reset
  everything.

Reassembly pseudo-code (J2534 header shown; HTTP is the same idea with an
8-byte header, `BLE_HTTP_PROTOCOL.md` section 7):

```python
buf = bytearray()
def on_notify(chunk):
    buf.extend(chunk)
    while len(buf) >= 12:
        magic, ver, mtype, seq, ch, length = struct.unpack_from("<HBBHHI", buf)
        if magic != 0x4A35: raise ProtocolError        # disconnect + reconnect
        if len(buf) < 12 + length: return              # wait for the rest
        frame = bytes(buf[:12 + length]); del buf[:12 + length]
        deliver(mtype, seq, ch, frame[12:])
```

### 4.4 The HTTP API over BLE (FFF4 in, FFF3 out)

Every route in `components/HTTP_API.md` is reachable from a paired phone:
the app sends a small JSON request head plus an optional streamed body,
the device replays it against its own web server and streams the status,
content type and body back. That is how a BLE app manages **storage**
(`/api/fs/list|info|upload|download|file|mkdir`, internal flash and the
SD card), **settings** (`/api/settings/...` + `submit`), **status and
logs**, the **UDS terminal** (`/api/uds/request`, `/api/uds/session`),
**AutoPID**, **scripts** and **OTA**. Contract, framing, credits, errors
and byte-exact examples: `ble_http/BLE_HTTP_PROTOCOL.md`. Bench and
reference client: `tools/testbench/ble/ble_http_pi.py`.

### 4.5 J2534 PassThru over BLE (FFF6 in, FFF5 out)

The same SAE J2534 wire protocol the Windows DLL speaks over TCP and USB
CDC-ACM, carried as a byte stream: HELLO / OPEN / CONNECT (CAN,
ISO15765) / WRITE_MSGS / filters / periodic messages / unsolicited
RX_MSG. Present only while `j2534_server.enabled` is true. One tester
across all transports: a PC tool attached over TCP makes a BLE tester's
first frame answer `ERR_DEVICE_IN_USE`, and the other way round.
`/api/j2534` reports `transport:"ble"` during a BLE session; the
Exclusive-bus rule (AutoPID off the bus) applies unchanged (the reflash
gate went on 2026-10-27: every write is forwarded, `reflash_seen` counts
the memory-transfer services). Full specification: `j2534_server/J2534_WIRE_PROTOCOL.md`
(section 8.3 = the BLE binding). Reference client:
`tools/testbench/ble/ble_j2534_pi.py`.

## 5. The CLI pipe (CLI IN to device to CLI OUT)

The full WiCAN command line (same registry as UART/WebSocket; commands in
`cmdline_manager/README.md`):

- Write ASCII **lines** terminated `\n` or `\r` to CLI IN (partial writes
  fine, the device reassembles; 1024-byte line max).
- The response streams as CLI OUT notifications and **ends with the
  `wican> ` prompt**: that is the "command done" marker.
- One command at a time device-wide (a mutex serialises all surfaces);
  lines are queued to the command dispatcher, never run inside the BLE
  stack.
- Keep to short-running commands from BLE (status/info reads are the
  intended use). `blehttp` and `j2534` print the two channels' status.

## 6. Connection behaviour an app MUST expect

- **WiFi hands over to you** (interface_manager, default rules ON): while
  your app is connected over BLE the device **suspends WiFi STA/AP**; it
  drops off the LAN and its HTTP API is unreachable over WiFi until you
  disconnect (resumes automatically a few seconds later). "User connects
  phone = user is in the car" is the product behaviour, and it gives BLE a
  clean radio (measured notify throughput ~77 KB/s). The HTTP API itself
  stays available to you through the BLE tunnel (4.4). Conversely a
  station on the WiCAN's own AP stops BLE (`ap_ble_exclusive`).
- **MTU**: the device prefers **517**. Android must request it
  (`requestMtu(517)`) right after connecting, before discovery; iOS
  negotiates on its own (185 on older devices, up to 512 on recent
  ones). The notification cap follows the negotiated MTU:
  `min(490, MTU - 3)`. Never depend on a particular packet size.
- **Connection parameters**: the device requests a window per the
  `conn_profile` setting: `ios` (default) 20-40 ms interval,
  `android_fast` 7.5-15 ms (halves command RTT on Android; iOS ignores
  out-of-policy requests, hence the default). Measured CLI round trip
  ~75 ms p50 on the default profile.
- **One central at a time**; advertising (every configured set) resumes
  on disconnect.
- **PHY** (BLE 5, 2026-09-21): the `phy` setting is the device's PHY
  preference, set as the controller default: `1m` (default) answers any
  central's PHY request with 1M only, so the link **stays on 1M** even
  with a 5.0 central that asks for 2M on its own (PC Intel adapters do);
  `2m` / `coded` / `auto` make the controller start the PHY switch right
  after the connection, and the central answers with what it supports (a
  4.2 phone keeps 1M). Result: `I ble_manager: PHY now tx 2M rx 2M`,
  `GET /api/ble` `phy_tx`/`phy_rx` (1/2/3 = 1M/2M/coded). An app can also
  request a PHY itself (Android `setPreferredPhy`; iOS decides alone).
  Nothing in the GATT contract changes with the PHY, only the on-air time
  per packet. Measured (`TESTING.md` row `blephy`, PC Intel adapter, WiFi
  handed over): write-without-response uploads run at 40-50 KB/s on
  either PHY, while indication-paced downloads (one 490 B indication per
  round trip) and command round trips are bounded by the connection
  interval, not the PHY. Downloads on notifications (protocol v2) scale
  with the PHY like uploads do (`TESTING.md` row `blethru`). Known central limit: the TP-Link UB500
  (RTL8761BU) drops a 2M link with the ESP32-S3 at the first 2M
  connection event (both sides time out); keep `1m` for that adapter.
- **Flow control**: the device retries a notification for up to 2 s when
  the radio credits are exhausted (a slow phone throttles a transfer
  instead of losing data); a burst it cannot absorb on FFF2 is counted
  and dropped, which the ELM dialect tolerates. On the stream channels a
  notification is retried up to 30 s (the same budget as an indication)
  and the HTTP tunnel keeps at most 16 KB unacknowledged by the app's
  `CREDIT` frames.
- Do not force LL data-length extension from a central that lets you:
  long LL packets destabilise the link under WiFi coex (measured, see
  `ble_manager_gatt_nimble.c`).

## 6b. Platform notes

- **Which OUT mode you get on FFF3 (measured 2026-09-22):** Android apps
  write the CCCD themselves (`ENABLE_NOTIFICATION_VALUE` = the fast path,
  `ENABLE_INDICATION_VALUE` = confirmed PDUs, no credits); iOS
  `setNotifyValue(true)` and BlueZ (`StartNotify`, `AcquireNotify`) pick
  NOTIFICATIONS when offered; Windows through bleak/WinRT picks INDICATIONS
  and refuses a direct CCCD write ("Cannot write to CCCD directly"), so a
  bleak client on Windows runs the slow path (5.6 KB/s at its 45 ms
  interval) while a native WinRT app can choose with
  `WriteClientCharacteristicConfigurationDescriptorAsync(Notify)`. Read
  `GET /api/ble` `channels[].out` when in doubt.

**Android (BluetoothGatt)**

- `connectGatt(..., TRANSPORT_LE)`, then `requestMtu(517)`, then
  `discoverServices()`.
- Pairing: let the first encrypted operation raise the system passkey
  dialog, or call `device.createBond()` first; the passkey is the
  device's `passkey` setting.
- Enable notifications with `setCharacteristicNotification(ch, true)`
  AND write the CCCD descriptor `0x2902` = `ENABLE_NOTIFICATION_VALUE`.
- Throughput: `WRITE_TYPE_NO_RESPONSE` for body/data writes. Only one
  GATT operation may be outstanding: wait for `onCharacteristicWrite`
  before the next write (API 33+: check the return code of
  `writeCharacteristic(ch, bytes, type)`).
- After a firmware update that grew the GATT table, bond again if the
  new characteristics are missing (section 2).

**iOS (CoreBluetooth)**

- Chunk writes to `peripheral.maximumWriteValueLength(for:
  .withoutResponse)`; gate them on `canSendWriteWithoutResponse` /
  `peripheralIsReady(toSendWriteWithoutResponse:)`.
- The pairing prompt appears on the first encrypted read or write
  (Insufficient Authentication), not at connect: read a Device Info
  characteristic first to trigger it deliberately.
- `setNotifyValue(true, for:)` on the OUT characteristics.
- iOS caches the GATT database per bond: Forget This Device after a
  firmware update if FFF3..FFF6 do not appear.

**Linux / BlueZ (bleak, desktop tools)**

- Subscribe with `AcquireNotify` (bleak: `start_notify(..., use_notify_acquire=True)`),
  not the default `StartNotify`: the D-Bus PropertiesChanged path dropped
  whole 490-byte notifications at 8 KB/s on the bench Pi (device counters
  65730 B sent vs 63770 B received, no device-side timeouts, 2026-09-21).
  The acquired socket keeps every one.
- After a fresh `pair()` the service collection can be stale: reconnect
  once bonded. Bonded devices do not show up in a scan (the RPA resolves
  to the identity address): connect to that address directly.

## 7. Settings that shape this interface

`/api/settings/ble_manager` (reboot-to-apply): `enabled` (default false),
`passkey` (123456), `tx_power_dbm` (9), `pairing_at_boot` (true),
`bonding` (true), `sc_only` (true), `conn_profile` (`ios` |
`android_fast`), and since descriptor v4 (2026-09-21) `phy` (`1m` |
`2m` | `coded` | `auto`, default `1m`) and `advertising` (`legacy` |
`extended` | `both`, default `legacy`): the defaults are byte-for-byte
the 4.2 behaviour, so a stored v3 config migrates by filling the
defaults. `GET /api/ble` mirrors both settings and the live link's
`phy_tx`/`phy_rx`.

Since descriptor v6 (2026-11-02, the 4.x parity of section 1 and 2):
`address` (`public` default | `private`) = the address the device
advertises with, and `security_request` (true default) = the LL Security
Request at the connect. Both fill in as defaults on an older stored
object. `GET /api/ble` echoes them as `address_type`, `security_request`
and the public address itself as `address`.

Since descriptor v5 (2026-10-31) the **Bluetooth memory profile**
`ble_ram_profile` (`lean` default | `full` | `custom`) sizes the ESP32-S3
controller at the next boot (the README's "Bluetooth memory profile"
section has the numbers). What an app sees: under `lean` the device runs
**one central at a time** (it always did: advertising stops on connect),
the **legacy advertising set on the 1M PHY only** (`advertising` other
than `legacy` and `phy` other than `1m` are refused with a reason), and
never scans; nothing in the GATT contract, the pairing or the pipes
changes. `full` keeps the BLE 5 options (2M and coded PHY, extended
advertising) at the Full memory cost; `custom` reads `ble_max_act`
(2..10), `ble_acl_tx_bufs` (0..12) and `ble_scan_cache` (0 = scanning off,
or 10..1000); the controller's BLE 5 feature set itself is never a knob
(the host is built with it). A v4 device that had chosen a BLE 5 option
migrates to `full`; every other device takes `lean`. `GET /api/ble` echoes
`ram_profile` and, once the stack is up, the sizing applied (`ram`).
`/api/settings/ble_http`: `enabled` (true), `cli`.
`/api/settings/j2534_server`: `enabled` (false) gates the J2534 channel,
`exclusive`, `allow_lan` (the TCP listener only; `allow_reflash` was removed
2026-10-27). `/api/settings/interface_manager`
(`sta_ble_handover`, `ap_ble_exclusive`) for the WiFi behaviour in 6.

## 8. Reference clients

`tools/testbench/ble/ble_bench.py` (pairing agent + echo + CLI),
`ble_ab.py` (device info + CLI RTT + pipes), `ble_blast.py` (throughput
counting), `ble_slcan_bridge_test.py` (slcan over the data pipe),
`ble_http_pi.py` (the HTTP tunnel incl. storage and UDS legs),
`ble_j2534_pi.py` (PassThru): bench-proven Python/bleak implementations
of everything above, including the passkey agent, identity-address
reconnects and frame reassembly.

## 9. Document ownership

This file owns the GATT contract (services, characteristics, UUIDs,
properties, security, MTU and chunking rules) and the stream-channel
registry table in 3.2. A component that registers a channel or changes
the bytes on one updates 3.2 and its own protocol document in the same
commit (`MEATPI_COMPONENT_STANDARD.md` 6c). Characteristics are only ever
appended within the FFF0 service so bonded clients' cached handles stay
valid.
