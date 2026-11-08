# wifi_manager: HTTP API reference

> **Implemented (2026-07-03)**: `wifi_manager_http.c`, registered via `wifi_manager_register_http()` (build-verified; live over-RF check = HIL S10). Unlike core components, `wifi_manager`
> is a **feature component** and registers these routes itself with
> `http_server_manager` at init (§9.1). Conventions:
> `components/HTTP_API.md` §1. Configuration is strictly via the generic
> `/api/settings/wifi_manager`: there are no bespoke config routes here.

## GET /api/wifi/status

Live connection state (wraps the component's status getters; cheap).

**Response 200**
```json
{
  "enabled": true,
  "sta_connected": true,
  "ssid": "HomeWiFi",
  "ip": "10.42.0.62",
  "ap_started": true,
  "clients": 1,
  "ap_ip": "192.168.0.10",
  "dns": ["10.42.0.1", "1.1.1.1"]
}
```
- `ip` is `""` while disconnected.
- `ssid` (2026-11-08) is the network the station is on, `""` while
  disconnected: what the senders' "active on" lists (the destinations, the
  Home Assistant webhook) are compared against; the UI marks it in their
  network dropdowns.
- `ap_ip` (2026-07-08) is the AP's live gateway address: reflects the
  configurable `ap_ip` setting; omitted when the AP interface has no
  address (AP off).
- `dns` entries read `"N/A"` when unavailable.
- The broader picture (overlap warning, time synced, …) lives in
  `GET /api/status`: this route is the WiFi panel's detail view.

## Configuration: `PUT /api/settings/wifi_manager` (v6)

No bespoke config routes; everything is configured through the generic
settings endpoint.

STA addressing (v6, 2026-07-11):

| Key | Type | Default | Notes |
|---|---|---|---|
| `sta_ip_mode` + `fallback1..5_ip_mode` | enum `dhcp`/`static` | `dhcp` | PER-NETWORK: each network carries its own addressing (applied per connect attempt). |
| `sta_static_ip` + `fallback1..5_static_ip` | string | `""` | REQUIRED when that network is static; dotted IPv4, host octet 1..254 (400 otherwise). |
| `sta_static_netmask` + `fallback1..5_static_netmask` | string | `255.255.255.0` | Must be a contiguous mask (400 otherwise). |
| `sta_static_gw` + `fallback1..5_static_gw` | string | `""` | Optional; validated as a host address when set. |
| `sta_dns` | string | `""` | Custom DNS override for BOTH ip modes. Empty = automatic (DHCP-provided, or the gateway when static). Re-asserted on every got-ip in DHCP mode. |

The AP LAN knobs (v4, 2026-07-08):

| Key | Type | Default | Notes |
|---|---|---|---|
| `ap_ip` | string | `192.168.0.10` | AP device/gateway IPv4; a `/24` is assumed and the DHCP pool follows it. Validated as a dotted quad with host octet 1..254 (rejects network/broadcast). Blank falls back to the legacy default. |
| `ap_hidden` | bool | `false` | Suppress SSID in beacons (clients must know the name). |
| `ap_bandwidth` | enum `ht20`/`ht40` | `ht20` | 40 MHz raises throughput at the cost of spectrum/robustness. |
| `ap_auth` | enum `auto`/`open`/`wpa2`/`wpa2wpa3`/`wpa3` | `auto` | `auto` = the historic rule (WPA2 with a password, OPEN without). WPA3/mixed enable PMF automatically. Any encrypted mode is rejected (400) when there is no `ap_password`. |

The WiFi memory profile (v5, 2026-07-08): trades internal RAM for WiFi
throughput by sizing the driver buffers at `esp_wifi_init` (the counts
are `wifi_init_config_t` fields, so this is a **runtime** setting, no
recompile; reboot to apply):

| Key | Type | Default | Notes |
|---|---|---|---|
| `wifi_ram_profile` | enum `full`/`lean`/`custom` | `lean` (since 2026-09-22; `full` before) | `full` = IDF-default buffers. `lean` frees **~14.4 KB internal** (static RX 10→6, static TX 8→4, cache TX 32→16) at **no measured throughput cost**: the device's real traffic is app/TLS-capped (~900 KB/s) below WiFi's buffer-limited ceiling (bench A/B 2026-07-08). Use `lean` to run WiFi + BLE together (the internal-RAM cliff). `custom` = the three knobs below. |
| `wifi_static_rx` | int 2..25 | `10` | custom only: static RX buffers (~1.6 KB internal DMA each). |
| `wifi_static_tx` | int 1..64 | `8` | custom only: static TX buffers (~1.6 KB internal DMA each). |
| `wifi_cache_tx` | int 0..128 | `32` | custom only: cache TX buffers. Ranges match the IDF Kconfig bounds, so no combination fails `esp_wifi_init`; `rx_ba_win` and the dynamic (PSRAM) pools are left at their defaults. |

Existing AP keys (`ap_ssid`, `ap_password`, `ap_channel`,
`ap_max_connections`, `ap_auto_disable`) are unchanged. All of the above
is reboot-to-apply like every setting.

## GET /api/wifi/scan

Blocking scan (≈2 s: the UI shows a spinner; in AP-only mode the radio
briefly switches to APSTA and back). Response is
`wifi_manager_scan_networks()` verbatim.

**Response 200**
```json
{
  "networks": [
    {
      "ssid": "HomeAP",
      "rssi": -52,
      "channel": 6,
      "auth_mode": "WPA2_PSK",
      "bssid": "aa:bb:cc:dd:ee:ff"
    }
  ]
}
```
`auth_mode` ∈ `OPEN, WEP, WPA_PSK, WPA2_PSK, WPA_WPA2_PSK, WPA3_PSK,
WPA2_WPA3_PSK, UNKNOWN`.

**Errors**: `503 {"error":"scan unavailable"}` (mode `off` or start refused).
Concurrent scans are serialized internally; a second request simply waits.

## Settings slice (via the generic surface)

`GET/PUT /api/settings/wifi_manager` + `/schema`, see
`components/settings_manager/HTTP_API.md`. Reminder: `sta_password`,
`ap_password`, `fallbackN_password` are redacted to `""` in GETs; sending
`""` back keeps the stored secret.

## POST /api/wifi/try, GET /api/wifi/try (2026-10-06)

The connection trial, for Quick Setup's "test it before storing and
rebooting" (Ali): join a network with credentials that are never saved,
report connected or the exact failure, let go. Needs a station interface
(mode `apsta` or `sta`, not suspended). The configured station, if it was
connected, drops for the trial and is re-joined right after (no AP-client
pause, no backoff: the user asked for the radio activity).

**POST** `{"ssid": "Neighbor", "password": "letmein-please", "channel": 11}`
(`password` 8..63, or absent/empty for an open network; `channel` optional,
the network's channel from the scan row, 1..13: the station scans that
channel first and alone, about one dwell instead of the thirteen during
which the access point cannot beacon (bench 2026-10-08: about 3 s of
silence, which is what a phone on the access point drops on; the switch
itself is announced and followed). Absent or out of range = all channels,
the case of a name typed by hand.)

- `202 Accepted`: the trial's status (`"state": "running"`). The radio
  action starts 300 ms after this answer: the station drops inside the
  trial, and a page that reaches the device over the station would never
  see its 202 otherwise. The phone on the access point blinks for a few
  seconds while the radio moves to the network's channel: poll the GET.
- `400 {"error": "need ssid (1..32) and password (8..63)"}`
- `409 {"error": "a test is already running"}`: one at a time.
- `409 {"error": "the station is off: the test needs Access point + Station"}`

**GET** the trial's status, polled once a second by the wizard:

```json
{"state": "done", "ssid": "Neighbor", "result": "connected",
 "reason": 0, "ip": "192.168.1.23", "rssi": -61, "channel": 6,
 "took_ms": 4200, "age_s": 3}
{"state": "done", "ssid": "Neighbor", "result": "password", "reason": 204, "took_ms": 6100, "age_s": 1}
{"state": "done", "ssid": "Nieghbor", "result": "not_found", "reason": 201, "took_ms": 2800, "age_s": 1}
{"state": "idle", "ssid": "", "result": "none", "reason": 0, "took_ms": 0, "age_s": 0}
```

| `result` | What the driver reported | What the wizard says |
|---|---|---|
| `connected` | an address (`ip`, `rssi` at that moment, `channel`, `took_ms` from the connect attempt) | accepted the password |
| `password` | disconnect reason 204 `HANDSHAKE_TIMEOUT`, 15 `4WAY_HANDSHAKE_TIMEOUT`, 2 `AUTH_EXPIRE`, 202 `AUTH_FAIL` (the bench AP gives 15, the HIL suite's router 204) | did not accept the password |
| `not_found` | 201 `NO_AP_FOUND` | not found: out of range, 5 GHz only, or a hidden name typed differently |
| `refused` | any other reason (203, 205, 200, ...) | the router refused or dropped the connection (reason N) |
| `no_ip` | associated, no `IP_EVENT_STA_GOT_IP` within 8 s | joined, but got no address: a full DHCP table or a MAC filter |
| `timeout` | nothing conclusive within 20 s | no answer in 20 s |

The last result stays until the next POST (`age_s` counts from it). The
console's `wifi --try <ssid> --password <pw>` runs the same trial and
prints the result line. Nothing is written: the credentials live in RAM for
the seconds of the trial (the driver's config storage is RAM too), and the
configured networks' attempt-failure memory never hears of it.
