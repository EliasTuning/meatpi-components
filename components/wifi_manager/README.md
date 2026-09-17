# wifi_manager

## Summary

Feature component owning the WiFi radio. Rewrite of the legacy WiCAN WiFi manager
against Coding Standard rev 2.1: same field-proven connection logic — STA with up
to 5 prioritized fallback networks, scan-based candidate selection, a per-entry
attempt-failure memory (2026-09-06, replaces the timed ban list: an entry whose
last 3 connection attempts failed — any reason but "not found" — is only
DEPRIORITISED — every other visible network
on the list is tried first, and when it is the only one around it keeps being
tried at the normal reconnect cadence, because it is our best chance; nothing
is ever blocked for a period. Memory is per config entry so two entries can
carry the same SSID with different passwords — home vs elsewhere, meatpi — and
it fades after 2 min so the entry regains its place in the order; a failed
roam-to-preferred trial deprioritises with one strike and the device goes
straight back), blind sequential fallback when nothing visible matches (hidden
SSIDs + dense-airspace scan truncation), roam-to-preferred (while connected to
a fallback, periodically re-scan and migrate to a higher-priority network —
"home beats the car hotspot"), strongest-BSSID association when one SSID has
several APs (WIFI_ALL_CHANNEL_SCAN + WIFI_CONNECT_AP_BY_SIGNAL), a supervised
reconnect task that pauses while AP clients are attached (escalating
retry backoff since 2026-07-10: the first two attempts keep the 5 s
cadence for a fast candidate walk, then 10/20 s doubling to a 30 s cap
so a parked device doesn't scan every 5 s forever — resets on got-ip;
pure `wm_backoff_skip_loops`, host-tested), AP with static IP `192.168.0.10/24` + DHCP server (the legacy WiCAN address per legacy wifi_mgr.c/safemode.c and the classic ELM327-WiFi-adapter convention `192.168.0.10:35000`, so OBD apps work out of the box — restored 2026-07-18, superseding a brief 192.168.80.1 period),
APSTA with AP-auto-disable and AP-channel-follows-STA, subnet-overlap warning,
backup-DNS fallback to 1.1.1.1 — but **all configuration is a settings_manager
descriptor and applies only at boot** (reboot-to-apply, §4.2). The legacy runtime
APIs (`set_mode`, `set_sta_config`, `enable`/`disable`, `deinit`, …) are gone by
design; that removed roughly a third of the old code (mode-change/teardown paths).

Recovery when STA can't connect: the device does NOT auto-open its AP
(a deauth attacker could force it open, meatpi 2026-07-08). The user
forces AP mode with a hardware button long-press (legacy behavior; owned
by the future button/input manager).

## API

| Function | One-liner |
|---|---|
| `wifi_manager_init()` | Allocate state; register the `"wifi_manager"` settings descriptor. |
| `wifi_manager_start()` | Radio up per boot-applied settings; `ESP_ERR_INVALID_STATE` if unconfigured; ESP_OK with radio down when mode is `off`. |
| `wifi_manager_stop()` | Stop reconnect task + radio. |
| `wifi_manager_is_enabled/is_sta_connected/is_ap_started()` | Status flags (event-bit backed). |
| `wifi_manager_sta_reconnect()` | Drop a STA association the caller knows is dead (AP rebooted inside the beacon-loss window); the normal selection path re-joins. |
| `wifi_manager_get_sta_ip(buf,len)` | Current STA IPv4 ("" if none) — caller buffer, no shared statics. |
| `wifi_manager_get_ap_station_count()` | Associated AP clients. |
| `wifi_manager_get_sta_dns(...)` | Main/backup DNS as strings ("N/A" fallback). |
| `wifi_manager_get_event_group()` | Event group with `WIFI_MANAGER_BIT_*` for waiters. |
| `wifi_manager_scan_networks()` | Blocking scan → malloc'd JSON (caller frees). |
| `wifi_manager_set_callbacks(cbs)` | STA up/down + AP client join/leave hooks (event-loop context). |

## Dependencies

- `event_manager` (2026-09-17, PRIV): `wifi_manager_events.c` declares the
  `wifi.sta {connected, ssid}` source and the `${wifi.ssid}` /
  `${wifi.connected}` live values, and publishes from the got-IP / disconnect
  handlers (a real drop only; failed attempts stay quiet). Declares are
  static writes, so init order against event_manager is free; publishing
  never blocks. event_manager sits below wifi_manager in the graph (its own
  dependencies never reach wifi_manager), so no cycle.

- `settings_manager` (private) — configuration descriptor; **init order:**
  `settings_manager_init()` → `wifi_manager_init()` → `settings_manager_start()`
  (runs `on_apply`) → `wifi_manager_start()`.
- `esp_wifi`, `esp_netif`, `esp_event` (private); `espressif/cjson` (managed,
  private) for scan JSON.
- `main` must run `nvs_flash_init()` before `wifi_manager_start()` (esp_wifi
  requirement). WiFi credential storage is RAM-only (`WIFI_STORAGE_RAM`) — the
  settings partition is the single source of truth.
- `log_manager` (private) — registers `{"wifi_manager", INFO}` at init for
  per-TAG runtime levels (§9.2). Reconnect attempts and candidate selection
  log at DEBUG (§10 hot-path rule) — raise the level at runtime to see them.
- `dev_status_manager` (private) — publishes `DEV_STATUS_BIT_STA_CONNECTED`
  (got-IP / disconnect), `DEV_STATUS_BIT_AP_ENABLED` (AP start/stop) and
  `DEV_STATUS_BIT_STA_AP_OVERLAP` (subnet clash, cleared with either side)
  into the ONE device-status group; all three cleared on `stop()`. Safe if
  dev_status_manager was never inited (its setters are no-ops then).

## Settings (`"wifi_manager"`, version 6)

`cli` (bool, default true): register this component's console command(s) with cmdline_manager on the settings boot apply (reboot-to-apply). Ownership: the component registers its own commands — main wires nothing (2026-07-05).

| Key | Type | Default | Notes |
|---|---|---|---|
| `mode` | enum `off/sta/ap/apsta` | `apsta` | |
| `sta_ssid` / `sta_password` | string ≤32 / ≤64 | `""` | empty ssid = no STA network |
| `fallback1..5_ssid` / `_password` | string | `""` | priority order; flat keys (schema subset has no arrays) |
| `hostname` | string ≤32 | `""` | DHCP/DNS host name of the STA netif. Empty → `wican_<12-hex device id>` (the name `mdns_manager` advertises as `.local`), pushed with `esp_netif_set_hostname` before the DHCP client starts, so a router's client list shows it instead of lwIP's default `espressif` (2026-09-08; pure builder `wm_default_hostname`, host-tested) |
| `sta_auto_reconnect` | bool | `true` | |
| `sta_max_retry` | int −1..1000 | `-1` | −1 = infinite; limit hits trigger a 1-min cooldown |
| `sta_ip_mode` / `fallback1..5_ip_mode` | enum `dhcp/static` | `dhcp` | v6: PER-NETWORK STA addressing — each network (primary + every fallback) carries its own choice, since they live on different LANs. Applied per CONNECT ATTEMPT (`apply_sta_network`): DHCP client stopped + `esp_netif_set_ip_info` for a static candidate, restarted (stale address cleared) for a DHCP one; got-ip still fires so the reconnect/roam machinery is unchanged. Bench-verified 2026-07-11 (static reachable from the LAN, DHCP restore clean) |
| `sta_static_ip` / `fallback1..5_static_ip` | string (IPv4) | `""` | v6: REQUIRED when that network is static (validated: dotted quad, host octet 1..254) |
| `sta_static_netmask` / `fallback1..5_static_netmask` | string (IPv4) | `255.255.255.0` | v6: must be a contiguous mask (`wm_netmask_valid`, host-tested) |
| `sta_static_gw` / `fallback1..5_static_gw` | string (IPv4) | `""` | v6: optional (isolated-LAN use); validated as a host address when set |
| `sta_dns` | string (IPv4) | `""` | v6: GLOBAL custom DNS override for BOTH ip modes (Pi-hole/1.1.1.1 case). Empty = automatic: the DHCP-provided server, or the connected network's gateway when static. In DHCP mode the override is re-asserted on every got-ip (the lease overwrites MAIN DNS); the 1.1.1.1 backup-DNS resilience still applies |
| `ap_ssid` | string ≤32 | `""` | empty → derived `WiCAN_<12-hex device id>` (SoftAP MAC, lowercase — legacy on-air format) |
| `ap_password` | string 8..64 | `@meatpi#` | legacy default; key material; auth mode set by `ap_auth`. **Cannot be kept** (2026-09-07): after the boot pass a write whose effective password is still the factory one is refused, `/api/wifi/status` reports `ap_default_password`, the web UI stops Submit until a new one is typed |
| `ap_auth` | enum auto/wpa2/wpa2wpa3/wpa3 (`open` removed in v7, 2026-09-07 — a stored `open` migrates to `auto`) | `auto` | v4: `auto` = WPA2 with a password (the 8-char minimum rules OPEN out). WPA3 + mixed enable PMF; an encrypted mode with no password is rejected |
| `ap_ip` | string (IPv4) | `192.168.0.10` | v4: AP gateway address; `/24` assumed, DHCP pool follows it. Validated dotted-quad, host octet 1..254. Live value echoed in `/api/wifi/status` `ap_ip`. Legacy default = the classic ELM327-WiFi-adapter address (`192.168.0.10:35000`) so OBD apps work unconfigured |
| `ap_channel` | int 1..13 | `6` | In APSTA the single radio parks on the associated upstream AP's channel, so once the STA connects the softAP FOLLOWS it (config synced from `esp_wifi_sta_get_ap_info` on association / got-IP / `WIFI_EVENT_HOME_CHANNEL_CHANGE`, incl. upstream-router CSA) and stays there across STA drops; this setting only takes effect while the STA is unassociated (HIL S8) |
| `ap_bandwidth` | enum ht20/ht40 | `ht20` | v4: 40 MHz for throughput vs 20 MHz for robustness |
| `ap_max_connections` | int 1..10 | `4` | |
| `ap_hidden` | bool | `false` | v4: don't beacon the SSID |
| `ap_auto_disable` | bool | `false` | drop AP once STA has an IP; back on STA loss |
| `sta_roam_interval_s` | int 0..86400 | `300` | v2: while connected to a FALLBACK, re-scan this often and migrate when a higher-priority network is visible; 0 = never roam |
| `wifi_ram_profile` | enum full/lean/custom | `full` | v5: trades internal RAM for WiFi throughput at boot. `full` = IDF default buffers. `lean` frees **~14.4 KB internal** (static RX 10→6, static TX 8→4, cache TX 32→16) at **no measured throughput cost** for the device's real traffic (MQTT/HTTP are app-capped below WiFi's ceiling; bench 2026-07-08). `custom` = the three knobs below. Runtime — the counts are `wifi_init_config_t` fields applied at `esp_wifi_init`; reboot to take effect. Use `lean` to run WiFi + BLE together (the internal-RAM cliff). |
| `wifi_static_rx` | int 2..25 | `10` | custom only: static RX buffers (each ~1.6 KB internal DMA) |
| `wifi_static_tx` | int 1..64 | `8` | custom only: static TX buffers (each ~1.6 KB internal DMA) |
| `wifi_cache_tx` | int 0..128 | `32` | custom only: cache TX buffers. Ranges match the IDF Kconfig bounds so no combination is invalid (rx_ba_win + dynamic pools are left at defaults / PSRAM) |
| `sta_trusted` / `fallback1..5_trusted` | bool | `true` | v3 network-trust lockdown: when the CURRENT STA network is untrusted, ALL inbound admin surfaces (every /api route, the UI catch-all, every /ws channel) that arrive via the STA address return 403 / refuse the handshake. The device's own AP + the USB link are always non-STA and keep full admin (the recovery path). Outbound clients — autopid HTTP posts, MQTT — don't traverse httpd and are unaffected. Enforced by http_server_manager's per-request gate (a trampoline wrapping every route incl. WS pre-handshake); wired by main to `wifi_manager_http_request_allowed()`. Fail-closed if the request can't be classified. Use case: configure on your home network, keep the device muted on shared/office WiFi. |
| `power_save` | enum `none/min/max` | `none` | |

`on_validate` (cross-field): rejects `sta_password` set with empty
`sta_ssid`; static mode without a valid `sta_static_ip`; malformed
netmask/gateway/DNS addresses; and, after boot, an `ap_password` that is
still the factory `@meatpi#` (the API's blank-password merge means "keep
it" is refused as well; a fresh device still boots with it).

## HTTP API (requirement — full endpoint reference: `HTTP_API.md` in this directory; conventions: `components/HTTP_API.md`)

As a **feature component** it registers its own routes with
`http_server_manager` at init (unlike core components, which go through the
`api_http` glue): `GET /api/wifi/status` (wraps the status getters) and
`GET /api/wifi/scan` (the scan JSON, blocking ≈2 s). Configuration strictly
via the generic `/api/settings/wifi_manager` — no bespoke config routes.
Passwords are redacted in settings GETs (HTTP_API.md §1).

## Memory footprint (measured 2026-07-26, `idf.py size-components`)

- `wifi_manager_events.c` (2026-09-17, estimated): 40 B internal `.bss`
  (the current SSID + connected flag + spinlock), no task, no heap.

| Where | What | Size |
|---|---|---|
| Flash | code + rodata | 15.5 KiB |
| PSRAM `.bss` | config + status + select/scan state + reconnect-task stack (`StackType_t` = bytes on xtensa) | **9,544 B** |
| Internal `.bss`/`.data` | event group + mutex + TCB | **553 B** |
| Heap | esp_wifi/lwip internals (IDF-owned; the `wifi_ram_profile` A/B measured lean = +13.8 KiB internal free vs full, 2026-07-22) | per IDF config |

The reconnect task performs no flash writes (persistence is settings_manager's),
so a PSRAM stack is compliant with §2.

## Tests

- **Host (`host_test/`):** the pure selection/failure-memory module —
  priority order, fallback pick, deprioritise after threshold, memory fade,
  success clears, all-failed keeps trying (round-robin), duplicate-SSID
  entries independent, roam never to the same SSID, sequential rotation.
- **On-target (`test_apps/`):** composition boot (settings apply → start), AP
  comes up with derived SSID, scan returns JSON, invalid settings rejected via
  `settings_manager_set`, no-apply-at-runtime. Builds against the main partition
  table (rev 2.1 §7). A full STA-association test needs a live AP —
  hardware-in-the-loop follow-up per §7 (README with bench setup) when added.

## CLI

`wifi_manager_register_cli()` (main, CLI builds) registers the `wifi [scan]` command with cmdline_manager (`wifi_manager_cli.c`).

## Factory AP password (2026-09-07)

`on_validate` refuses any save that keeps the factory AP password once
the device is up (the boot pass still accepts it, or a fresh device could
never come up). `wifi_manager_ap_password_is_factory()` lets consumers
that write wifi_manager themselves (espnetlink_link's pairing store)
detect the state and hold off with a clear warning instead of failing
opaquely (field-hit 2026-09-07).

## AP-client pause policy (2026-08-31)

While a client sits on the WiCAN's own AP, a STA (re)connect that hops
the radio channel knocks that client off, so the reconnect loop defers.
Two escapes exist (`wm_sta_pause_for_ap_clients`, host-tested): the
**first association of a boot always goes ahead** — a user configuring
a fresh device over its AP (the ESPNetLink zero-touch pairing story) was
otherwise never getting an uplink, field-hit on a fresh WiCAN Pro — and
the pause is **bounded** to `WM_AP_CLIENT_MAX_PAUSES` × 10 s (60 s), so
an uplink outage never lasts as long as a phone stays parked on the AP.
The client sees at most one channel hop.
