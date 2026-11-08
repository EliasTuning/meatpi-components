> **App developers (Android/iOS/desktop): the on-air contract lives in**
> **[`BLE_API.md`](BLE_API.md)**, advertising, pairing, every service/
> characteristic UUID with byte-exact values, pipe semantics, and the
> WiFi-handover behavior an app must expect (2026-07-05). The HTTP API
> and J2534 ride on stream channels: `ble_http/BLE_HTTP_PROTOCOL.md`,
> `j2534_server/J2534_WIRE_PROTOCOL.md` (2026-09-21).

## The advertised address and the pairing request (settings v6, 2026-11-02)

Three field reports of 2026-11-02 (a Home Assistant user whose presence
automation lost the device, a Discord thread about apps that worked on 4.x
and got nothing on 6.0) came down to three differences from firmware 4.5x,
all undone on Ali's word ("behave like 4.5x unless there is a major security
concern"):

| Key | Type | Default | Meaning |
|---|---|---|---|
| `address` | enum public / private | `public` | `public` = the fixed hardware address (the station MAC + 2), what 4.x advertised (Bluedroid kept `BLE_ADDR_TYPE_PUBLIC`; its `config_local_privacy` call only resolved the peers' addresses): presence detection by MAC and apps that remember the address work. `private` = the controller's resolvable private address from our IRK, a new one at every advertising restart (so after every disconnect) and every `CONFIG_BT_NIMBLE_RPA_TIMEOUT` (15 min): the v6 betas before this. A bond holds the public identity either way, so the switch keeps paired phones |
| `security_request` | bool | `true` | the device sends an LL Security Request one second after the connect, if the link is still plain by then and the pairing window is open, as 4.x did (`esp_ble_set_encryption` on connect): the phone pairs now, or encrypts with its bond, and the app's first write lands on a secured link. Off for a Windows app with its own pairing ceremony (WinRT custom pairing cannot join a peripheral-started system pairing, the 2026-09-21 finding that had removed the request; `ble_phy_pc.py` turns it off for its leg). The second matters: a central that pairs or encrypts on its own at the connect (a bonded Android, the simulator's bench central) is secured first and gets no request at all; sent from the connect event itself, or a quarter second in, the request went out while a bonded NimBLE central's LL encryption was still in flight (no SMP procedure on our side to answer `EALREADY`), the central answered it with a second encryption, and that restarted its discovery (the blethru 2M leg, 2026-11-02, twice) |

The third difference is not a setting: the subscribe (a CCCD write on FFF1,
FFF3, FFF5) needs the paired link again, as the 4.x table had it
(`ESP_GATT_PERM_WRITE_ENC_MITM` on the CCCD). NimBLE gates a CCCD only
through the `BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC/AUTHEN` flags of its
characteristic, which the table did not set, so a plain link subscribed
FFF1 silently and its write-without-response to FFF2 was dropped before
the access callback (no error, no log line): an app that only subscribes
and writes never paired. With the flags the subscribe is answered
Insufficient Authentication and the phone pairs right there. Measured
before the change with `tools/testbench/ble/ble_addr_probe_pi.py`
(`testing/ble_addr_probe.md`); `GET /api/ble` carries `address`,
`address_type` and `security_request`. The factory bridge for the data
pipe (`br_ble_obd`, bridge_manager v2) is the fourth piece: on 4.x
Bluetooth on WAS the ELM pipe to the chip.

## A bonded phone's reconnect: the encryption can land before the connect (2026-11-08)

With this NimBLE (IDF 6.1) the peripheral's CONNECT event is not reported at
the connection-complete event: `ble_gap_rx_conn_complete()` only starts a
Read Remote Version exchange and `ble_gap_event_connect_call()` reports the
connect when that completes, about three connection intervals later. A
bonded iPhone starts encryption within one interval, so its ENC_CHANGE
reaches the GATT backend BEFORE its CONNECT. The backend's `s_secured` flag
followed ENC_CHANGE and the CONNECT handler cleared it: on the iPad's
reconnect of 2026-11-08 14:11 the console read `pairing success` 92 ms
before `client connected`, the subscribe went through on what the flag
called a plain link (NimBLE's ATT server gates the CCCD on the real state,
which was encrypted), the app's first writes were refused as unpaired
(`data write rejected: link not securely paired`), the one-second security
request went out on an encrypted link and iOS paired again (a second PIN
prompt), and Car Scanner reported "connection failed" after 12 s. The RealDash
report of 2026-11-07 ("four to five attempts at every app start" on an iPhone)
is this.

Since the fix: the CONNECT handler reads the link's live security state
(`ble_gap_conn_find`, `sec_state.encrypted && authenticated`) instead of
assuming a plain link, logs `link already encrypted with the bond at the
connect` and raises the channels' SECURED event when it is; the security
request timer checks the live state as well; `blm_gatt_secured()` re-syncs
the flag from the live state whenever it reads false on a connected link.
Proven on the iPad the same afternoon: the reconnect at 14:22 logged the
same order, then the new line, the subscribe at +0.3 s, the first write at
+0.9 s, no rejection, no pairing request, no PIN.

## Session log lines (2026-11-08)

A field report about an app ("RealDash takes four tries", "Car Scanner
pairs and shows nothing", "drops after a minute") used to leave two
console lines, `client connected` and `client disconnected`, and the build
compiles the debug level out (`CONFIG_LOG_MAXIMUM_LEVEL` 3), so nothing
said who connected, which connection window the phone settled on, when it
paired, when the app first wrote, how much flowed, or why the link ended.
Those facts are what separate an app problem from a link problem from a
pipe problem, so every session logs them at INFO, one line per milestone
(a handful per session, standard section 10), all under the `ble_manager`
tag:

| Line | When | What it says |
|---|---|---|
| `client connected: peer 5A:.. (random), interval 30.00 ms latency 0 timeout 720 ms; asking 20-40 ms` | the connect | the central's on-air address and type (a phone shows a random one), the window it opened with, the window this device requests (`conn_profile`) |
| `connection update: interval 15.00 ms latency 0 timeout 4000 ms (120 ms after the connect)` | every accepted parameter update | what the central settled on: a 30 ms or 45 ms interval is an iPhone's default, 7.5 or 15 ms a fast Android; each ELM exchange costs two to four intervals |
| `security request sent (the link was still plain)` | one second after the connect, if nothing secured it first | the `security_request` setting at work; a bonded Android encrypts before it and the line is absent |
| `pairing success (bonded 1)` or `pairing failed (status 0x...)` | the encryption change | as before, plus whether the keys were kept |
| `data pipe subscribed (notify, by the central, 1900 ms after the connect)` | the FFF1 CCCD write | the ELM apps' first step; `restored from the bond` when the stack replayed a bonded phone's subscription; `link still plain` names a subscribe that slipped through unpaired |
| `first app write 2310 ms after the connect (secured, 4 B)` | the first FFF2 write of the session | when the app started talking; `plain` would mean a write on an unsecured link (the GATT flags refuse it, so it should never read so), `no bridge on the pipe` names a device without a `ble` connection row |
| `client disconnected: the peer ended it (0x213); 83.2 s, secured 1.8 s, first write 2.3 s, last write 0.4 s before end, pipe subscribed, app->dev 412 wr 3214 B, dev->app 1380 ntf 25160 B, drops sub 0 tx 0` | the disconnect | the reason in the Core spec's words with NimBLE's code (0x200 + the HCI code: 0x213 the phone closed it, 0x208 the radio link was lost, 0x216 this device closed it, 0x23E the connection never came up, 0x206 the phone holds a stale bond, 0x205 the pairing failed), then the whole session in numbers: its length, when it was secured, the app's first and last write, whether the pipe was subscribed, the writes and bytes app to device (`wr`), the notifications and bytes device to app (`ntf`), the drops of the subscriber queue (`sub`, bridge side) and of the radio queue (`tx`). Short words on purpose: the log manager's line is 256 bytes and a longer line is cut without its newline |

Reading them: a session that is `never secured` with `no app write` is a
central that connected, was asked to pair and left (a stale bond on the
phone, a refused passkey, or an app that scans and probes every device;
the bench Pi's stale bond on 2026-11-08 read `pairing failed (status
0x007)` then `authentication failure (0x205); 1.0 s, never secured, no app
write`, sixteen times in a row, until the bond was removed); a secured
session with writes and `0 ntf` is a pipe with nobody on the other end
(the Connections page, or the OBD chip down); a big `last write ... before
end` with a supervision timeout is a phone that walked away; writes
without a `data pipe subscribed` line is an app that never asked for
notifications. The words come from
`ble_manager_diag.c` (pure, `host_test/main/test_diag.c`), the live
session sits in `ble_manager.c`, the GATT layer feeds it from its events
(`blm_core_on_connect` with the link's `blm_link_info_t`,
`blm_core_on_conn_update`, `blm_core_on_secured`,
`blm_core_on_data_subscribe`, `blm_core_on_tx` from the IO task's
notifications, `blm_core_on_disconnect` with the reason). The Bluedroid
backend passes no link and no reason and keeps the two plain lines. The
bench side that reads them beside the counters is
`tools/testbench/ble/app_session_capture.py` (`testing/app_session_capture.md`).

## Bluetooth memory profile (settings v5, 2026-10-31)

Bluetooth on this build costs about 38 KB of internal RAM (the bench unit:
49.5 KB free with it off, 11.3 KB with it on, station on the Lean WiFi
profile with the ESPNetLink dongle enumerated; the boot line says `WICAN
RAMMAP ble_manager=39860`), which left a default-mode device with Bluetooth
one burst from failed saves and no room for Tailscale (issue 1024). The
ESP32-S3 controller sizes itself from `esp_bt_controller_config_t` at
`esp_bt_controller_init()`, RUNTIME fields read once, so the sizing is a
setting applied at the next boot, the `wifi_ram_profile` model:

| Key | Type | Default | Meaning |
|---|---|---|---|
| `ble_ram_profile` | enum full / lean / custom | `lean` | `full` = the build's Kconfig sizing (today's behaviour and the BLE 5 options); `lean` = one phone at a time, legacy advertising on the 1M PHY, no scanning: `ble_max_act` 2, the 12 static ACL TX buffers kept, the scan feature and its duplicate caches off; `custom` = the three knobs below |
| `ble_max_act` | int 2..10 | 4 | custom only: controller activities = connections plus advertising instances, 828 B each (Kconfig `BT_CTRL_BLE_MAX_ACT`) |
| `ble_acl_tx_bufs` | int 0..12 | 12 | custom only: static ACL TX buffers, about 270 B each; fewer than 12 brings back the silent notification loss of 2026-09-22 when the heap is low |
| `ble_scan_cache` | int 0 or 10..1000 | 100 | custom only: the scan duplicate cache; 0 = the scan feature and both caches off (the product never scans); 1..9 refused by `on_validate` |

Rules: under `lean` the `advertising` and `phy` words must be `legacy` and
`1m` (`on_validate` refuses the others with a reason the UI and the API
show; `on_apply` forces them for a stored object from before the rule).
Migration v4 to v5 (`on_migrate`): a device that had chosen a BLE 5 option
(`advertising` extended or both, `phy` other than 1m) gets `full`, every
other device takes the default. The controller's BLE 5 feature set is NOT a
knob: the NimBLE host is compiled with it (`CONFIG_BT_NIMBLE_50_FEATURE_
SUPPORT`) and `esp_bt.h` ties `ble_50_feat_supp` to the host's choice; the
measurement build's `ble_5_features=false` row took the device off the bench
network within a minute of its boot (no crash recorded), and the knob went
before the final image.

Mechanics: `blm_ident_ram_config()` (pure, `ble_manager_pack.c`, host-tested
in `host_test/main/test_ram.c`) maps the profile onto `blm_ram_cfg_t`;
`blm_ctrl_configure()` (`ble_manager_ctrl.c`) writes it into the
`BT_CONTROLLER_INIT_CONFIG_DEFAULT()` struct either GATT backend builds, logs
`ble ram profile=<word> (act=.. acl_tx=.. scan=.. ble5=..)` once per
bring-up and keeps the applied sizing for `GET /api/ble` (`ram_profile`,
`ram`). The NimBLE backend makes the port's three calls itself
(`esp_bt_controller_init`, `esp_bt_controller_enable`, `esp_nimble_init`)
because `nimble_port_init()` builds the default config internally and
refuses a controller that is already up; `nimble_port_deinit()` still undoes
all three. Each bring-up also logs `internal RAM per step: controller init
N, enable N, host init N, services N, host task N B`.

**Measured (2026-10-31, bench unit 68ee8f5a653d, station on the Lean WiFi
profile, the dongle enumerated; `ble_ram_bench.py --measure`, the free
internal heap at 75 s of uptime, `.\test.ps1 bleram` for the gate rows):**

| row | internal free | min free | against Full |
|---|---|---|---|
| Bluetooth off | 49.5 KB | 38.8 KB | - |
| Full (act 4, ACL 12, scan cache 100) | 11.3 KB | 5.7 KB | Bluetooth costs 38.1 KB |
| act 2 | 13.2 KB | 7.6 KB | +1.9 KB |
| ACL buffers 6 | 12.9 KB | 7.3 KB | +1.6 KB |
| ACL buffers 0 | 14.4 KB | 8.8 KB | +3.1 KB |
| scanning off | 12.6 KB | 7.0 KB | +1.3 KB |
| **Lean** (act 2, ACL 12, scanning off; the gate run on the final image, where Full read 11.4 KB) | 14.5 KB | 9.0 KB | +3.2 KB (Bluetooth costs 34.9 KB) |

Where the 38 KB goes (the per-step line of a Full bring-up, 84.5 KB free
before it): `esp_bt_controller_init` 34.3 KB (the activities 3.3, the ACL
buffers 3.2, the scan caches 1.3, the controller task's 3.5 KB stack, and
about 22 KB of the controller library's own state that no config field
reaches), `esp_bt_controller_enable` 0, `esp_nimble_init` 1.0 KB, the GATT
services 16 B, the NimBLE host task 4.4 KB (its 4 KB stack is internal by
IDF's design: the NVS bond store writes from it, and a flash write from a
PSRAM stack is forbidden, standard §2). So the profile moves what the config
can move: about 3 KB on Lean, 8 KB at most with the ACL buffers given up.
The structural remainder (the library's 22 KB, the two task stacks' 7.9 KB)
needs other levers; moving the bond store's write off the host task would
free its 4 KB and is the next one worth measuring. The Tailscale hold reason
(vpn_manager) does not name this profile on purpose: 3 KB is no help against
a 30 KB floor.

**The NimBLE host task on a PSRAM stack (2026-11-01, lever 3 of
`internal/TASK_ble_tailscale_margin.md`).** The host task was the one BLE
stack that had to be internal, because the bond store writes NVS from it:
`ble_store_config.c` calls a persist function of IDF's `ble_store_nvs.c`
after every write or delete of its tables (a pairing, a bonded peer's CCCD
change, the round-robin eviction of the oldest bond), and that opens NVS,
writes the blob and commits. `ble_manager_store_nvs.c` wraps the six
persist functions this build compiles at link time (`CMakeLists.txt`, the
`--wrap` + `-u` idiom of ip4_routes) and runs the real one through
`flash_jobs_run()`, so the write lands on the flash worker with the host
task blocked on it, as it was blocked on its own write before. The store's
load (`ble_store_config_init()`, every table read from NVS) is wrapped the
same way, because the host calls it from its own task at every startup
(`ble_hs_pvcy_set_default_irk()` under `CONFIG_BT_NIMBLE_STATIC_TO_DYNAMIC`;
the first image without that wrap aborted there at 1 s of uptime, IDF's
assertion naming `nimble_host`). With that, `ble_manager_gatt_nimble.c`
creates the host task itself (`nimble_host`, 6 KB: the port's
`CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE` 4 KB left 1028 B of headroom right
after a pairing in the first `blesec` run, the standard's line, so 2 KB
were added where they cost nothing; priority and core as
`esp_nimble_enable()`) on a PSRAM static with a static TCB, once for the
firmware's life: it runs
`nimble_port_run()` until `nimble_port_stop()` returns it, parks on a task
notification, and the next bring-up wakes it; `blm_gatt_stack_down()` waits
for the park before `nimble_port_deinit()`. Nothing is deleted or freed, so
the runtime restarts (interface_manager around an access-point station,
the config mode, the sleep path) race no task lifetime. The per-step line
reads `host task 0 B` now. A PSRAM stack's overflow is silent (standard
§2): the task's watermark is on `/api/status/tasks` (`stack_ext` true)
whenever Bluetooth is on, `blesec` reads it after the pairing (the ECDH of
LE Secure Connections is the deepest path the task runs) and `stackaudit`
asserts the placement. A persist the worker could not run (no internal RAM
for it) is reported to NimBLE as `BLE_HS_ESTORE_FAIL` with one W line: the
bond works for the session and is not persisted; `health.flash_jobs`
`refusals` counts it. Measured (`bleram`, 2026-11-01, the bench unit without
the dongle): Bluetooth costs **30.1 KB on Full and 27.0 KB on Lean** (34.6
and 31.5 KB the night before: 4.5 KB less on either profile, the host task's
stack and TCB no longer from the internal heap); `blesec` on the same image:
3924 B of headroom before a pairing, 3076 B after it, the pairing 5 jobs on
the worker, the reconnect after a reboot 33, 0 refusals, the bond persisted.

**The lever that did reach (2026-10-31 evening): `CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY`**
(the firmware's `sdkconfig.defaults`), the controller's code in flash instead
of IRAM. On the ESP32-S3 every IRAM byte freed becomes an internal-heap byte:
measured +19.4 KB at idle (51.5 to 69.3 KB free), and Bluetooth itself got
cheaper (Full 34.6 KB, Lean 31.5 KB on that build), so a Lean device with
Bluetooth on has 37.9 KB free and Tailscale starts beside it (the first time:
`.\test.ps1 tspublic` PASS with Bluetooth on, 18.6 KB steady). The price, from
`blethru` with the station up: the 2M PHY about a quarter slower (notify 374
kbps, was 484; write 245, was 407; tunnels 333 to 380, were 456 to 512), 1M
unchanged; and the controller cannot run while the cache is off for a flash
erase (no auto-suspend on this flash chip), so a settings save or a firmware
update pauses the link for each erase; `blehttp`'s 512 KB uploads to the
internal flash over a live link still completed byte-exact with 0 link drops.

## Stream channels (2026-09-21)

Besides the legacy data pipe (FFF1/FFF2, the bridge_manager `ble` jack)
and the CLI pipe, `ble_manager` hosts a bounded registry of **stream
channels**: a consumer registers `{name, uuid_out, uuid_in, rx_storage,
rx_size, on_event}` BEFORE `ble_manager_start()` (the GATT table is built
from the registry at start; `ble_manager_channel_lock()` freezes it) and
gets one notify + one write characteristic APPENDED to the FFF0 service
(handles of the legacy four stay stable for apps with a cached GATT
database). Semantics: a reliable in-order byte stream, no framing here.

| Function | Contract |
|---|---|
| `ble_manager_channel_register(desc, &id)` | pre-start only; 16-bit UUIDs on the FFF0 base (odd out, even in); refuses duplicates / FFF1 / FFF2; `ESP_ERR_NO_MEM` + `E` when the registry (cap 4) is full (§12) |
| `ble_manager_channel_read(id, buf, n, timeout_ms)` | `>0` bytes, `0` timeout, `<0` link down or unknown id. Leftovers of a dropped link are discarded on the first read after it (a link generation counter; the reader resets its own StreamBuffer, which is the only legal place). 100 ms slices bound link-down detection. This is the `j2534_transport_t.read` contract verbatim |
| `ble_manager_channel_write(id, buf, n)` | MTU-chunked PDUs in the OUT mode the central selected with its CCCD among the owner's `out_modes` (`ble_manager_channel_out_mode`): **indications** (`blm_gatt_indicate_handle`: one in flight per link, each confirmed within `BLM_CHANNEL_TX_WAIT_MS` 30 s = the ATT indication timeout; a phone confirms only after the writes it already queued went out, so a 2 s budget stalled uploads) or **notifications** (`blm_gatt_notify_handle`, bounded ENOMEM retry inside the same 30 s, VHCI ingress gate; 2026-09-22, opt-in per channel because a notification has no end-to-end acknowledgement, so the owner's protocol must carry credits + a frame counter: ble_http v2 does, ble_j2534 stays indicate-only); `n`, `-1` no secured link, `-2` timeout. Never from the BT host task. Counters `tx_notifications` / `tx_indications`, the live mode in `GET /api/ble` `channels[].out` |
| `ble_manager_channel_stats/capacity` | counters (`rx_overflow`, `tx_timeouts`, ...) and the `WICAN CAPS ble_ch=used/cap` line |

Events (`on_event`, BT host task, flag-and-wake only): `CONNECTED`
(unusable yet), `SECURED` (bytes flow), `DISCONNECTED`. Writes on a
channel are refused until the link is secured (`WRITE_ENC|WRITE_AUTHEN`
plus the software check). A full RX buffer answers *Insufficient
Resources* to write-with-response and counts `rx_overflow` for
write-without-response; the owner's protocol carries the flow control
(ble_http: CREDIT frames, both directions in notify mode; j2534: one
request pipelined at most). The GATT properties of an OUT characteristic
follow the descriptor's `out_modes` (0 = indicate only, the pre-2026-09-22
contract); the GAP `SUBSCRIBE` event (`blm_channel_on_subscribe`) records
the central's CCCD per channel, reset on disconnect (a bonded central's
CCCDs come back as `SUBSCRIBE reason=RESTORE`). The pure picker
`blm_channel_pick_out` (notify wins a `0x0003`, unsubscribed = indicate)
is host-tested. **The notification loss of 2026-09-21 (2 of 146 PDUs
counted by the host, never on air) was the CONTROLLER, not NimBLE**: with
`CONFIG_BT_CTRL_BLE_STATIC_ACL_TX_BUF_NB=0` the ESP32-S3 controller mallocs
each ACL TX buffer from the internal heap at transmit time and drops the PDU
silently when that fails, which on a device booting with ~8 KB free happened
in 7 of 24 notify downloads (btmon arbiter, 1-4 consecutive PDUs each).
`CONFIG_BT_CTRL_BLE_STATIC_ACL_TX_BUF_NB=12` (the Kconfig maximum, ~3 KB
internal at controller init) made 0 of 12 lose a PDU; gating on
`esp_vhci_host_check_send_available()` and 32/48 NimBLE mbuf blocks (PSRAM,
kept) did not. `ble_manager_gatt_tx.c` holds the notify/indicate primitives
(split out for the 700-line rule). **Cost:** the 12 static buffers take
3.2 KB of internal RAM at controller init (`WICAN MEM internal_free` at boot
with BLE on: 7991 -> 4787). At that level the bench DUT ran into the
internal-RAM cliff within minutes (`E httpd: Failed to post
esp_http_server event: ESP_ERR_TIMEOUT` every 2 s, the tunnel's loopback
requests failing with 502, `RAM Min free ever: 155 bytes`), so the bench DUT
now runs `wifi_manager.wifi_ram_profile = lean` (boot free 18403, +13.6 KB;
its own README recommends lean for WiFi + BLE). Any device that ships with
BLE on needs the same, or a smaller static count re-measured for drops.

Files: `ble_manager_channel.c` (registry, stack-agnostic),
`ble_manager_gatt_svc.c` (the NimBLE service table + access callbacks,
split out of `ble_manager_gatt_nimble.c` which keeps GAP/SM/adv/notify).
NimBLE only: the Bluedroid A/B backend stubs `blm_gatt_notify_handle()`
with `ESP_ERR_NOT_SUPPORTED`. `CONFIG_BT_NIMBLE_MAX_CCCDS` is 16 (5
notify characteristics x 3 bonds; the Kconfig default 8 would lose
subscriptions).

Memory (estimated): registry + 4 `StaticStreamBuffer_t` ~0.7 KB internal
`.bss`, two indication semaphores ~180 B internal; the mutable FFF0 table
+ UUIDs + a 490 B RX scratch in PSRAM; the RX storage belongs to the
consumer (PSRAM). `GET /api/ble` (`ble_manager_http.c`) reports the link
and every channel's counters.

**Congestion fix (2026-09-21).** The former `s_congested` latch in
`notify_handle` was set on ENOMEM and cleared only by a successful
notify, while the IO layer's TX task never notified while it was set:
one overload (the slcan bench's 1000 fps flood) killed BLE TX for the
rest of the boot. It is now a self-expiring 5 ms window
(`s_congested_until`) that `blm_gatt_notify_handle()` feeds while it
retries with a bounded deadline (data/CLI pipes 100 ms, channels 2 s), and
it resets on connect / disconnect / host reset. The `E` line in the TX
task's pacing loop is a `D` (a data-path condition, §10). Regression
witness: `ble_slcan_bridge_test.py --flood` leg E.

## BLE 5 as settings: PHY + advertising sets (2026-09-21)

Descriptor v4 adds `phy` (`1m` default | `2m` | `coded` | `auto`) and
`advertising` (`legacy` default | `extended` | `both`); v3 configs migrate
by filling the defaults (`on_migrate` = fill-missing, pure). Design and
measurements: `TASK_ble5_phy_adv.md`. NimBLE's 5.0 feature set was already
compiled in (`CONFIG_BT_NIMBLE_50_FEATURE_SUPPORT`); the change compiles
extended advertising in once (`CONFIG_BT_NIMBLE_EXT_ADV=y`, 2 instances,
256 B max) and chooses at runtime:

| Piece | Where | Behaviour |
|---|---|---|
| advertising sets | `ble_manager_gatt_adv.c` (`blm_adv_configure/start/stop`) | instance 0 = the legacy PDU set (today's bytes), instance 1 = the extended set (flags + UUID + name + `MeatPi` in one PDU, connectable, primary 1M, secondary per `phy`). With `BLE_EXT_ADV` on NimBLE turns `ble_gap_adv_start()` into an ENOTSUP stub, so the legacy set also goes through the extended API (`legacy_pdu = 1`). All instances stop on connect (one central), the configured ones restart on disconnect / failed connect |
| PHY | `ble_manager_gatt_nimble.c` | `ble_gap_set_prefered_default_le_phy(mask, mask)` in `on_sync` for EVERY value (`1m` = 0x01 keeps a 2M-asking central on 1M; the controller itself starts the switch for the others); a host-side `ble_gap_set_prefered_le_phy()` follows on CONN_UPDATE / ENC_CHANGE only when the controller did not already switch; `BLE_GAP_EVENT_PHY_UPDATE_COMPLETE` logged at I (W when refused, never E); `blm_gatt_phy()` reads the live PHYs for `GET /api/ble` (`phy_tx`/`phy_rx`) |
| no Security Request at connect | `ble_manager_gatt_nimble.c` CONNECT | the legacy `esp_ble_set_encryption`-on-connect mirror is gone (2026-09-21): phones pair on first encrypted access anyway, and the peripheral-driven start broke Windows apps' custom pairing (WinRT `Failed`, no ceremony; the DUT saw ENOTCONN 2 s in). `pairing failed` now logs the SMP status at W |
| advertising TX-power AD | `ble_manager_gatt_adv.c` | the configured level, not `BLE_HS_ADV_TX_PWR_LVL_AUTO`: AUTO issues the legacy LE_Read_Advertising_Channel_Tx_Power HCI command, which the controller answers *Command Disallowed* once it runs the extended API (the first bench boot advertised nothing) |
| pure mapping | `ble_manager_pack.c` (`blm_ident_phy_mask`, `blm_ident_adv_mode`), host-tested | `1m`->0x01, `2m`->0x02, `coded`->0x04, `auto`->0x03, unknown->1M; `legacy`/`extended`/`both`, unknown->legacy |

Memory: extended advertising compiled in = +1.8 KB flash, 0 B static
internal RAM (measured 2026-09-21, `idf.py size` before/after; NimBLE's
allocations are in PSRAM here); the runtime instances add nothing while
`advertising = legacy`. The Bluedroid A/B backend ignores both settings
(1M, legacy) and reports `phy_tx/rx = 1` while connected.

## Bench blast route (2026-09-21)

`POST /api/ble/blast {"bytes":N,"chunk":490}` (`ble_manager_http.c`) is the
July `blast` console command as an HTTP route: a PSRAM-stack task pumps N
bytes of a counting pattern through `ble_manager_send()` onto the data pipe
(FFF1 notifications), yielding 2 ms whenever the TX queue is full, and
`GET /api/ble` reports `blast:{running,bytes,target,ms}`. Because it is an
`/api/*` route it is reachable through the BLE tunnel, so a central can
start it with WiFi handed over: the `notify` mode of `ble_central_bench`
does exactly that and counts what arrives. Not a product feature; a bench
source (one at a time, 409 while running).

## GATT backend: Bluedroid vs NimBLE (switchable, 2026-07-05)

The stack-facing layer is behind the `blm_gatt_*` contract, with two
implementations selected by the sdkconfig BT host choice (CMake picks
the file): `ble_manager_gatt.c` (Bluedroid) or `ble_manager_gatt_nimble.c`
(NimBLE, `CONFIG_BT_NIMBLE_ENABLED` + `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_
EXTERNAL` so host allocs ride PSRAM). Same on-air contract either way --
UUIDs, byte-exact Device Info (incl. legacy NUL/padding), SC+MITM+BOND
static passkey, MTU 517, adv layout, conn-window request. Verified with
`tools/testbench/ble_ab.py` (pair, Device Info, CLI RTT, both pipes
through a `ble <-> obd0` bridge).

Measured on the composed main (WiFi STA+AP + mutual-TLS MQTT active --
coexistence throttles BLE in these numbers; the earlier 4.6 ms RTT bench
ran WiFi-off):

| | Bluedroid | NimBLE (EXTERNAL) |
|---|---|---|
| BLE start, internal RAM | 58,408 | **41,644** (-16.8K) |
| internal free after boot | 79,115 | **90,031** (+10.9K net) |
| firmware size | 1.98 MB | **1.80 MB** (-183K) |
| CLI RTT p50 (coex-loaded) | 226 ms | 390 ms |
| TX notify (coex-loaded) | 20.5 KB/s | 21.7 KB/s |
| pair / Device Info / pipes | PASS | PASS |

NimBLE mapping notes (in the file header too): no controller-credit API --
notify does a bounded ENOMEM retry feeding the congestion flag; no
per-CCCD permissions -- notify additionally refuses until the link is
secured; long CLI writes arrive reassembled.

**WiFi-state sweep (2026-07-05, meatpi asked)** -- CLI RTT p50, both
stacks, same bench:

| WiFi state | Bluedroid | NimBLE |
|---|---|---|
| APSTA + MQTT (first runs) | 226 ms | 390 ms |
| STA-only | 390 ms | 390 ms |
| WiFi OFF (`wifi --stop`; BLE is the sole radio user) | **195.0 ms** | **195.0 ms** |

Conclusion: the stacks are IDENTICAL in every condition (the day-one
226-vs-390 was run-to-run environment, not the stack). WiFi coex costs
~2x CLI RTT (195 -> 390 ms); the 195 ms radio-off floor is the bench's
write-with-response chaining across BlueZ connection events, not
firmware (the historic 4.6 ms figure measured the FFF2->FFF1 echo path
with the WiFi-off test app). `wifi --stop` (ephemeral, reboot restores)
exists for exactly this kind of isolation run.

**Re-run on the CLEAN bench (2026-07-05, TP-Link UB500 dongle: the
Pi's internal adapter, disabled since, was inflating every earlier RTT
number ~3x):**

| metric | Bluedroid | NimBLE (EXTERNAL) |
|---|---|---|
| CLI RTT p50, interface_manager suspends WiFi (product path) | 76.6 ms | 76.2 ms |
| CLI RTT p50, WiFi forced on (rules off, DUT coex) | 113.0 ms | 115.3 / 117.0 ms |
| TX notify, coex-loaded | 28.2 KB/s | 24.0 / 28.6 KB/s (par - RF run-to-run band) |

Same verdict, now on clean data: performance identical; NimBLE keeps
the default slot on memory (-16.8K internal) and flash (-183K).

## Throughput vs the esp-idf ble_throughput demo (2026-07-05)

meatpi: "the esp32 throughput BLE demo shows better numbers". Ladder
run on the clean bench (NimBLE, UB500), tuning one lever at a time:

| configuration | TX notify | CLI RTT p50 |
|---|---|---|
| product (`ios` profile), WiFi coex forced on (rules off) | 24-28.6 KB/s | 115 ms |
| + DLE forced (`ble_gap_set_data_len` 251) | **2.1 KB/s: collapse** | unstable connects |
| `android_fast` profile (15 ms interval), coex | 26.1 KB/s | 66.3 ms |
| **clean radio** (rules ON -> BLE connect suspends WiFi), `blast` over UART | **77.1 KB/s** | - |

Conclusions:

- **The gap is coex, not the stack.** With WiFi suspended (which IS the
  product path: interface_manager suspends STA/AP on a BLE connect)
  the same firmware does 77 KB/s, demo territory. The demo runs with
  no WiFi at all.
- **Connection interval helps RTT only** (115 -> 66 ms with
  `android_fast`); TX is airtime-bound under coex, so the interval
  lever doesn't move it.
- **DLE is harmful under coex**: forcing 251-byte LL packets collapsed
  TX 28 -> 2 KB/s and destabilised connects (long LL packets lose the
  coex arbitration). Reverted; do-not-retry note lives in
  `ble_manager_gatt_nimble.c`.
- Measurement notes: `blast N` (main_cli.c) pumps N bytes through
  `ble_manager_send`; fire it over **UART only**: BLE CLI lines run
  inline in the NimBLE host task, so a long command wedges the stack
  (GATT Unlikely Error; fix item filed: route BLE CLI through the
  cmdline dispatcher queue). Pi side: `tools/testbench/ble_blast.py`
  (passive subscribe + count). NimBLE logs an INFO pair per notify:
  console spam during blast, harmless.
