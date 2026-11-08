# ha_webhooks

## Summary

The Home Assistant integration (v6 port of the legacy `ha_webhooks` +
autopid webhook poster). Owns the HA telemetry link in both directions:

- **inbound** `/api/webhook`: the HA HACS integration auto-registers its
  webhook URL (discovery push), applied live;
- **outbound** a poster task that, every `interval_s` while enabled +
  network up, POSTs `{status, autopid_data, config, gps}` to the URL(s)
  with failover. autopid may be OFF: `autopid_data` is then simply omitted
  (a status-only push is a valid contract push).

Feature component. Consumes `http_server_manager` (the route),
`http_client_manager` (outbound POST + TLS), `cert_manager` (private-CA
sets), `autopid` (`autopid_snapshot` + `autopid_config_json_dup`),
`dev_status_manager` (the status section + `device_id`),
`battery_monitor` (`batt_voltage`), `wifi_manager` (`sta_ip`),
`vpn_manager` (`vpn_status`/`vpn_ip`) and `mdns_manager` (the `mdns`
configuration URL). Config lives in
`settings_manager` (`"ha_webhooks"`); the URL push is a documented live
exception to reboot-to-apply.

**This is the cross-product MeatPi↔HA contract** (device-contract v2,
see `device-contract/` and the payload reference in `HTTP_API.md`):
identical on every product built from the V6 core.

## Design notes

- **PSRAM-safe poster (§2 corollary).** The poster runs on a PSRAM stack,
  so every payload source must be RAM: `autopid_snapshot` (RAM cache),
  `autopid_config_json_dup` (a NEW PSRAM cache of `config.json`, the
  poster must NOT read flash), `dev_status_manager` getters. Stats are a
  cache-only write (no flash) via a mutex-guarded status struct.
- **Payload contract.** Byte-verified against the live HACS integration
  (`coordinator.py handle_webhook_data`); contract-v2 shape 2026-07-11:
  `{schema:1, status, autopid_data, config}` with
  `status.device_id/fw_version/hw_version` GUARANTEED every push (diff
  mode overlays them back after the diff: HA's identity check + update
  entity read them). `status` also carries `device_type`, `mdns`,
  `wifi_mode`, `ble_status`, `batt_voltage`, (while the station holds an
  address) **`sta_ip`** (2026-10-11, issue #977: the integration's station
  IP diagnostic sensor and the device card's `configuration_url` read it;
  the legacy `/check_status` had it for the polling integration, no
  webhook push ever did), `vpn_status` (always: `disabled | waiting |
  paused | connecting | connected`, vpn_manager's own words since issue
  #985) and, while the tunnel is up, `vpn_ip` (HA's away-from-home
  backup endpoint).
  Sections are diffed in `changed` mode and omitted when empty; a fresh
  registration forces a full **resync** so a new HA sees the complete
  state (since 2026-10-11 that full push also primes the diff caches,
  so the next push is a real diff; it used to go out whole once more,
  the sta_ip bench caught it). The builders live in
  `ha_webhooks_payload.c` (`hw_payload_build()`), the poster task,
  gzip and the failover in `ha_webhooks_post.c`. **`gps`** (2026-09-09): the contract §5.4 block
  `{latitude, longitude, accuracy, altitude, speed, heading, satellites}`
  from `usb_acm_cli_gps_get()` while a live fix exists: what HA's
  Location device_tracker reads (it never updated for WiCAN Pro before);
  sent whole on change, omitted without a fix.
- **No autopid gate (2026-09-08).** The poster used to skip every lap
  while `autopid.enabled` was false. A fresh device ships autopid OFF,
  so HA's discovery push got `201 Created` and then not one telemetry
  push ever left the device (`GET /api/webhook` stayed
  `status:"disabled"`, success/fail 0/0) - every entity in HA stayed
  "unavailable" with no error anywhere. The HA contract
  (`DEVICE_COMPATIBILITY_GUIDE.md` §4: "a push with only `status` is
  fine") and the integration's fixed sensors (battery voltage, WiFi
  mode, uptime, ECU online) need no vehicle data, so the poster now
  gates on enabled + network only; `autopid_data` is omitted until
  autopid produces values. Regression bench:
  `tools/testbench/ha/ha_webhook_gate_bench.py`.
- **Failover** across `url` + `url2` (PRO); first 2xx wins. An HTTP 403
  from HA (identity rejection) skips failover; 3 consecutive rejected
  cycles pause the poster (`status: "rejected"`) until the next
  registration, the contract's "stop; needs user attention" action.
- **gzip** (setting, default off; either data mode): the push body is
  compressed via the **ROM miniz** deflate (`tdefl_*` are mask-ROM
  symbols: zero flash cost) with hand-rolled gzip framing (10 B header
  + raw deflate + CRC32/ISIZE; CRC = `mz_crc32` → `esp_rom_crc32_le`).
  The ~166 KB compressor state is a lazy one-time PSRAM alloc reused
  across posts (poster is the single caller). Compression failure falls
  back to the plain body: delivery beats savings. Off by default:
  HA integrations older than 2026-07-10 can't inflate. STACK LESSON
  (bench, 2026-07-11): ROM tdefl keeps several KB of Huffman/blocking
  locals on the CALLER's stack, the poster's 24 KB stack overflowed on
  the first gzip push (clean canary panic); now 32 KB (PSRAM, cheap).
  Bench-measured ratio ~2.3-2.4× on small (0.6-1.6 KB) payloads; bigger
  autopid sets compress better.
- **TLS**: built-in bundle by default; `cert_set` for a private-cert HA;
  raw-IP HTTPS auto-skips CN.
- **Active on (2026-11-08, Ali).** `networks` names the station networks
  the pushes go out on (`""` = every network, else names separated by
  commas); the poster asks `wifi_manager_sta_on_network()` before every
  attempt and waits while the station is on another network or on no WiFi
  at all, nothing counted as a failure, one I line per transition (`not on
  a selected network: pushes wait` / `on a selected network again`).
  `GET /api/webhook` and the `webhook` command show the list and whether
  the station is on one of its networks now; the UI's card has the
  dropdown with check boxes beside the other settings and its stats line
  reads `Waiting for <names>` while the pushes wait. The registration push
  keeps the stored list.
- **Not the generic path.** For routing individual PIDs to arbitrary
  endpoints, use `event_manager` rules (`autopid.param → http.post`). This
  component is the fixed, always-on HA telemetry channel: different shape,
  which is why it's a dedicated poster (isolated blocking I/O) rather than
  an event_manager action on the shared dispatcher.

## Testing

- `tools/testbench/ha_webhook_bench.py` (live-suite stage `ha_webhook`): a
  mock HA receiver on the Pi → register via `POST /api/webhook` → assert
  `{status+device_id, autopid_data?, config}` telemetry, failover to
  `url2`, GET stats, DELETE.
- `tools/testbench/ha/ha_webhook_gate_bench.py` (`HA WEBHOOK GATE
  PASS`): the fresh-device regression - autopid DISABLED (settings
  PUT + submit), register a receiver, expect status-only pushes
  (`status.device_id` present, no `autopid_data`) and `status:"ok"`
  stats; restores autopid and clears the webhook. Receiver on any
  host both sides reach (`--serve` on the PC, or `--pi rpi001`).
- `tools/testbench/ha/ha_webhook_sta_ip_bench.py` (`HA WEBHOOK STA IP
  PASS`, issue #977): the station address in the push. Registers a
  receiver; the first push (the resync) must carry `status.sta_ip` equal
  to the address of `/api/wifi/status`, a later push in `changed` mode
  must not repeat it unchanged (in `full` mode it is there every time),
  no ha_webhooks E line in the log ring; puts the registration it found
  back (or DELETEs when there was none) and the interval through
  settings. Same receiver options as the gate bench.
- `tools/testbench/ha/ha_lte_push_bench.py` (`HA LTE PUSH PASS`, runs ON
  rpi001): the car-on-the-road leg - the DUT roams to the paired
  ESPNetLink AP (LTE + GPS), `--capture` asserts the `gps` block + the
  `gps_*` sensors in the pushes; the HA mode registers the PRO dual URLs
  and proves the HTTPS failover carries the pushes over LTE; `--dut-vpn`
  is the REAL test against a bench HA reached through the WiCAN's own
  WireGuard tunnel (`tools/testbench/vpn/wg_ha_route.sh`). 2026-09-09:
  capture PASS; VPN mode PASS - HA registered its tunnel URL, the DUT
  roamed to LTE and kept pushing through the tunnel, HA's Location
  tracker took the `gps` block (`Updated GPS location ... accuracy 3m`).
  The HTTPS leg reached an HA over LTE but the dev HA's Nabu Casa remote
  domain belongs to another instance (TESTING.md).
  Real HA end-to-end available on `rpi002` (`~/workspace/ha-wican`).

## Open items

- **URL ordering on the road (2026-09-09).** `post_failover` always
  tries `url` (HA's local http URL) first; over LTE that address is
  unroutable, so EVERY cycle spends ~6 s in a connect timeout and logs
  the IDF connect-failure E triplet (`esp-tls select() timeout` /
  `Failed to open a new connection` / `Connection failed, sock < 0`)
  before `url2` delivers. A car parked on LTE does this forever. Worth
  a small policy: after N consecutive connect failures on `url`, try
  `url2` first and re-probe `url` every few minutes (never drop it -
  driving home must switch back).

## HTTP API

See `HTTP_API.md` in this directory. Routes: `GET/POST/DELETE
/api/webhook`. Settings: `/api/settings/ha_webhooks`.
