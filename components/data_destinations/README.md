# data_destinations

## Summary

Cyclic delivery of the live AutoPID parameter set ("autopid_data") to
up to 8 **destinations**: MQTT topics, HTTP/HTTPS endpoints (bearer /
API-key header or query / basic auth, extra query parameters, a
cert_manager certificate set for private CAs and mutual TLS) and the
**ABRP** (Iternio) telemetry API. Feature component; the v6 home of the
legacy autopid `destinations[]` (v4.51p), rebuilt on the v6 services:
`autopid` (the snapshot + the cached PID tables), `mqtt_manager` (the
one broker client), `http_client_manager` (outbound HTTP(S) with TLS
through `cert_manager` sets), `dev_status_manager` (identity, link
bits, uptime), `battery_monitor` (the status block's voltage).

The Automate → Data destinations page of the web UI edits the table;
`GET /api/destinations` shows the live counters and `POST
/api/destinations/test` delivers one destination now (the Test
button). Everything else (conditions, templates, event-driven pushes)
stays with the rules engine (`event_manager` `timer.tick` →
`mqtt.publish` / `http.post`), which this component does not replace.

## API (`include/data_destinations.h`)

| Call | Behavior |
|---|---|
| `data_destinations_init()` | Settings (`data_destinations`) + log descriptors. No I/O. |
| `data_destinations_start()` / `_stop()` | The poster task (PSRAM stack, 32 KB); refuses `ESP_ERR_INVALID_STATE` when unconfigured (§4.3 step 5). |
| `data_destinations_status(&json)` | The `/api/destinations` document: `enabled`, `running`, `network`, `mqtt` + per destination `name,type,enabled,url,period_s,auth,has_token,has_api_key,cert_set,success,fail,skipped_offline,consecutive_failures,backoff_s,next_in_s,last_status,last_error,last_error_time,last_ok_time,full_sent`. Secrets never appear. |
| `data_destinations_abrp_coverage(&json)` | The `/api/destinations/abrp` document: `present` (ABRP fields the live snapshot provides), `missing` (`{field, parameter}`: the autopid name that would fill each high-priority field), `period_floor_s`, `request_gap_ms`. |
| `data_destinations_test(name, &result)` | Deliver once now on the poster's stack (SYNCHRONOUS, ≤ 20 s): `ok`, HTTP `status`, `elapsed_ms`, `error`. `ESP_ERR_NOT_FOUND` / `_INVALID_STATE` (busy or poster down) / `_TIMEOUT`. |
| `data_destinations_register_http()` / `_register_cli()` | Own routes (§6e14) / the `destinations` command (self-registered on settings apply behind `cli`, §6b). |

## Behavior

- **One poster task, 1 s laps.** A row's period counts from the START of
  its delivery (not its end: a 5 s ABRP row then sent every 7.5 s, its
  1.8 s TLS round trip plus the queue; fixed 2026-10-13), so a 5 s row
  sends every 5 to 6 s; a delivery slower than its period is followed by
  the next one at once. For every enabled destination that is
  DUE: MQTT needs the broker connected, HTTP/HTTPS/ABRP need a network
  (`DEV_STATUS_NETWORK_CONNECTED_MASK`, i.e. STA or USB-Ethernet). A
  down link is a **skipped lap** (`skipped_offline`, next lap after the
  link returns delivers): never a failure, never a backoff, and it
  logs ONE `W` per outage (`<name>: no network - waiting`) + one `I`
  when the link is back, so a car out of WiFi range does not fill the
  log.
- **Active on (2026-11-08, Ali: "let the user select on which WiFi SSID
  the destination is active").** A row with a `networks` list sends only
  while the station is on one of the named networks
  (`wifi_manager_sta_on_network()`, names compared whole, blanks around the
  commas ignored); on another network, or on no WiFi at all (the ESPNetLink
  or USB Ethernet alone), the lap is a **skipped lap** of its own
  (`skipped_network`), never a failure, never a backoff, one I line per
  transition (`<name>: not on a selected network (<ssid>) - waiting` /
  `on a selected network again`); a Test answers `not on a selected
  network`. The status document carries the station's `ssid` and the
  row's `networks` / `on_network`; the UI's row chip reads `Waiting for
  <names>` and the options panel's dropdown with check boxes lists the
  station networks under Settings > WiFi (a stored name that is not
  configured any more is listed struck through).
- **Backoff (pure, host-tested).** A delivery that fails (transport
  error, non-2xx, ABRP `status != ok`) counts in `fail` +
  `consecutive_failures`; from the 3rd consecutive failure the interval
  doubles from 10 s up to `max(8 × period, 60 s)` (never above 10 min);
  the first success resets it; an HTTP **429** (rate limited) goes to that
  cap at once, not after three tries (logged once at `W`). The transition logs once at `W`
  (`delivery failed: http=503`), the ladder start once (`3 consecutive
  failures - backing off to 10s`), recovery at `I`; per-attempt detail
  is `D`. The IDF HTTP client itself still prints its `esp-tls` /
  `HTTP_CLIENT` connect-failure lines at E for an unreachable host
  (the bench whitelist).
- **ABRP rate guard (2026-10-13).** Iternio allows 2 requests per second
  per api_key and takes one point per 5 s: an ABRP row runs at **5 s at
  least** (a shorter stored period is applied as 5, never rejected), and
  the device starts ABRP requests **at least 2.5 s apart**, the Test
  button included (a due row and a Test both wait for the gap on the
  poster, at most 2.5 s; a pacer that skipped the lap instead starved the
  third ABRP row of a table for good), so no table or click pattern
  passes a fifth of the limit. The status document's `abrp_pacer`
  (`gap_ms`, `waits`, `waited_ms`, `last_request_age_s`) shows the pacer
  at work. `GET
  /api/destinations/abrp` says which of ABRP's main fields the live
  snapshot provides and which parameter name would fill a missing one
  (`soc` from `SOC`, `power` from `HV_W`, `is_charging` from `CHARGING`,
  ...); the UI shows it under the row, because a profile without those
  names gives ABRP a position and nothing to plan with.
- **Payloads.** MQTT: the flat `{"Name": value, …, "timestamp": epoch}`
  snapshot, retained by default (`retain`), `~/x` → `<prefix>/x`.
  HTTP(S): with `full_first` (default) the FIRST successful push
  carries `{"config": <PID tables>, "status": {device_id, fw_version,
  hw_version, device_type, uptime, network_connected, mqtt_connected,
  ecu_status, batt_voltage, timestamp}, "autopid_data": {...}}` and
  every later push `{"autopid_data": {...}}`; without it the short form
  always. ABRP: `token=<user token>&tlm=<url-encoded JSON>` form POST
  to `https://api.iternio.com/1/tlm/send` (the default URL), the
  `api_key` as `?api_key=` (default) or `Authorization: APIKEY <key>`
  (`auth=api_key_header`); `tlm` = the snapshot mapped through the
  legacy name table (`SOC→soc`, `HV_W→power`, `SPEED→speed`,
  `CHARGING→is_charging`, `CHARGING_DC→is_dcfc`, `PARK_BRAKE→is_parked`,
  `HV_CAPACITY_KWH→capacity`, `HV_CAPACITY_R→soe`, `SOH→soh`,
  `TMP_A→ext_temp`, `BATT_TEMP→batt_temp`, `HV_V→voltage`,
  `HV_A→current`, `ODOMETER→odometer`, `RANGE→est_battery_range`,
  `T_CAB→cabin_temp`, `TYRE_P_xx→tire_pressure_xx`; case-insensitive)
  plus the ESPNetLink GPS fix (`gps_latitude→lat`, `gps_longitude→lon`,
  `gps_altitude→elevation`, `gps_heading→heading`, `gps_speed→speed`
  when the vehicle has no `SPEED`), `utc` (device clock) and
  `car_model` (the ABRP model id, per destination). Iternio answers
  `{"status":"ok"}`; any other `status` is a failure even on HTTP 200
  (`last_error` = `abrp: error ["missing utc"]`); a refused key or
  token is HTTP 401 with `{"status":"error","errortext":"Unauthorized
  Key"}` / `"Unauthorized Token"` (`last_error` = `abrp: error
  "Unauthorized Token"`, `last_status` 401; seen live 2026-10-13: the
  detail is read from `missing`, `errors`, `errortext` or `error`, in
  that order). Checked against the
  Iternio doc 2026-09-19: values are passed through UNCONVERTED, so the
  vehicle profile must already use ABRP's units and signs, `power` in
  kW with driving positive / charging NEGATIVE, `current` in A,
  `tire_pressure_*` in kPa, `odometer`/`est_battery_range` in km;
  `is_charging`/`is_dcfc`/`is_parked` become 0/1 (booleans and
  "on"/"off" strings are converted). Iternio wants a point every 5 s
  (30 s+ is discouraged) and speed/power/is_charging at least every
  10 s: the poster walks the table sequentially, so a slow or failing
  HTTP row in the same table delays the ABRP row by its request time
  (up to the 6 s timeout): keep ABRP's table short or its neighbours
  healthy. Verified on the bench against a mock Iternio endpoint
  (`tools/testbench/actors/dd_receiver.py`); the live service needs a
  real user token + api_key (`data_destinations_bench.py --abrp-token
  --abrp-key`).
- **TLS.** `https` destinations (and ABRP) verify through
  `http_client_manager`: `cert_set` names a cert_manager set (its CA,
  plus client cert + key when both exist = mutual TLS), empty = the
  built-in CA bundle. A raw-IP HTTPS host skips the CN check (bench /
  LAN receivers with IP certificates). A set that does not exist falls
  back to the bundle with a `W` from http_client_manager.
- **Test.** `POST /api/destinations/test {"name"}` / `destinations -t
  <name>` run one delivery on the poster (its 32 KB stack carries the
  TLS handshake), ignoring the cycle/backoff, and book it in the
  counters like a scheduled one.

## Dependencies

`PRIV_REQUIRES settings_manager log_manager http_server_manager
cmdline_manager http_client_manager mqtt_manager autopid
dev_status_manager battery_monitor console esp_http_server esp_timer`.
Init after `autopid_init` and `mqtt_manager_init`; start after
`mqtt_manager_start` and `autopid_start` (main does; the gates inside
make the order non-critical).

## Settings (`/api/settings/data_destinations`, version 1, reboot-to-apply)

`enabled` (master, default true: an empty table is quiet), `cli`
(true), `destinations[]` (≤ 8 flat items):

| Key | Type | Default | Meaning |
|---|---|---|---|
| `name` | string 1..15, required | - | unique id; the UI's row key and the test/CLI handle |
| `type` | `mqtt` \| `http` \| `https` \| `abrp`, required | `mqtt` | |
| `enabled` | bool | true | pause without deleting |
| `url` | string ≤ 255 | `""` | MQTT topic (`~/autopid` when empty), URL (a bare host gets the type's scheme; `https` refuses `http://`), ABRP endpoint (Iternio's when empty) |
| `period_s` | 1..86400 | 5 | cycle; an ABRP row runs at 5 s at least (a shorter value is applied as 5) |
| `auth` | `none` \| `bearer` \| `api_key_header` \| `api_key_query` \| `basic` | `none` | HTTP(S): how `auth_token` is sent; ABRP: `api_key_header` switches the api_key from the query string to the header |
| `auth_token` | ≤ 255, secret | `""` | bearer / API-key value; **ABRP user token** (required there) |
| `auth_name` | ≤ 63 | `x-api-key` / `api_key` / `Authorization` | header or query-parameter name for the API-key modes |
| `basic_username` / `basic_password` | ≤ 63 (password secret) | `""` | basic auth |
| `api_key` | ≤ 255, secret | `""` | ABRP developer api_key |
| `query` | ≤ 255 | `""` | extra query parameters, raw `k=v&k2=v2` (HTTP(S)/ABRP) |
| `cert_set` | ≤ 24 | `""` | cert_manager set for HTTPS/ABRP, empty = bundle |
| `car_model` | ≤ 63 | `""` | ABRP `car_model` id |
| `retain` | bool | true | MQTT retained publish |
| `full_first` | bool | true | HTTP(S): the config+status push once |
| `networks` | ≤ 199 | `""` | "active on" (2026-11-08): `""` = every network, else the station networks' names separated by commas (the row sends only while the station is on one of them; an added key with a default, no version bump) |

`on_validate` = the pure parser: an ENABLED entry without a URL, an
`https` entry with an `http://` URL, ABRP without `auth_token`, bearer/
API-key modes without `auth_token`, basic without `basic_username`,
duplicate names → rejected with the reason (`dest1: url required`).
Secrets: `auth_token`, `api_key`, `basic_password` come back `""` on
GET and an empty PUT value keeps the stored one, `api_http` matches
array rows by `name` (rows may be reordered or deleted), then by index.

## Files

| File | Role |
|---|---|
| `data_destinations_core.c` | PURE: item parse/normalize, scheduler + backoff, percent-encoding, URL compose, `~/` expansion |
| `data_destinations_abrp.c` | PURE: the ABRP tlm map, form body, `APIKEY` header value, response verdict |
| `data_destinations.c` | lifecycle, config/state caches, the poster task, status + test |
| `data_destinations_post.c` | payload builders + the MQTT / HTTP(S) / ABRP deliveries |
| `data_destinations_settings.c` | field table, on_validate, on_apply |
| `data_destinations_http.c` | `/api/destinations`, `/api/destinations/test` |
| `data_destinations_cli.c` | `destinations [-t <name>]` |

## Memory footprint (estimated 2026-09-19)

PSRAM `.bss`: the applied table (8 × ~1.3 KB ≈ 10.6 KB) + the settings
parse scratch (another ~10.6 KB) + the 32 KB poster stack + ~1 KB of
state; internal RAM: the TCB, two static semaphores, ~100 B of flags.
Heap (PSRAM, transient per delivery): the snapshot JSON (≤ ~6 KB for a
192-parameter profile), the first HTTP push's `config` copy (the PID
tables, up to ~100 KB for the widest profiles), ABRP form body ≤ 3 ×
the tlm JSON, one http_client_manager response ≤ 4 KB. Flash: none,
counters are RAM only.

## Testing

- Host: `.\test.ps1 host data_destinations`, 22 Unity tests
  (`host_test/README.md`; the `networks` parse and the network skip since
  2026-11-08).
- Bench (the networks, 2026-11-08): `.\test.ps1 destnets`
  (`tools/testbench/system/dest_networks_bench.py`, `DEST NETWORKS PASS`):
  a row and the Home Assistant webhook restricted to another network wait
  (counted, nothing at the receiver, the ring's line), restricted to the
  station's own network they deliver; five planned restarts.
- Bench: `python tools/testbench/system/data_destinations_bench.py`
  (`DATA DEST PASS`), MQTT / HTTP / HTTPS with a cert set (and without:
  the bundle must refuse the bench CA) / mutual TLS / a mock ABRP with
  the token + api_key + tlm contract; the server-down backoff, the
  broker-down and WiFi-down / reconnect scenarios, counters and the
  E-line whitelist. See `TESTING.md`.
- Live: `.\test.ps1 abrp` (`tools/testbench/system/abrp_live_bench.py`,
  `ABRP LIVE PASS`): the ABRP rows against the real Iternio API with
  the credentials in `internal/abrp_credentials.json` (gitignored:
  `api_key` + the vehicle's `user_token`); without a token only the
  refusal path runs (`ABRP LIVE PARTIAL PASS`). See `TESTING.md`.
