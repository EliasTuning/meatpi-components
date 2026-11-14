# ha_webhooks: HTTP API reference

> **THE cross-product MeatPi ↔ Home Assistant contract** (device-contract
> v2, `device-contract/` in this directory: ask #5): `/api/webhook` plus
> the periodic push below is the Level-1 MANDATORY surface every MeatPi
> product implements identically. Conventions: `components/HTTP_API.md`
> §1. Feature component: registers its own routes via
> `ha_webhooks_register_http()` (§9.1); the network-trust gate wraps them
> like every admin surface (VPN-tunnel requests classify as non-STA and
> stay allowed: HA's away-from-home path). Contract verified against the
> live HACS integration (`custom_components/wican`, 2026-07-09); contract
> v2 (integration 3.0) alignment 2026-07-11.

## POST /api/webhook

The HA integration's **discovery push**: it registers its webhook URL so
the device knows where to send telemetry. Applies **live** (no reboot;
the whole point of discovery) and persists.

**Request**
```json
{ "url": "http://ha:8123/api/webhook/<id>", "enabled": true, "interval": 15,
  "urls": ["http://ha:8123/api/webhook/<id>", "https://x.ui.nabu.casa/api/webhook/<id>"],
  "manual_override": false }
```
- `url` (required, http/https). `urls` (optional, ≤2, ordered by priority):
  `urls[0]` must equal `url`; the 2nd is the failover target.
- `enabled` (default true), `interval` (1..3600 s), `manual_override`.
- **`manual_override`**: when already set, an external push that omits it
  is ignored (200, unchanged), the user pinned the URL.
- Idempotent (re-POST of the same URL → 200), persisted across reboots.
- No `Authorization` header / token is required (the webhook id inside
  the URL is the shared secret: contract v2 Security).
- **`rev`** (optional, 2026-11-14, settings in the push reply below): the
  revision Home Assistant gives this set of settings, 1 to 16 characters of
  `A-Z a-z 0-9 _ -`. Stored with the settings and reported in every push as
  `status.webhook_rev`. A body with a `rev` is held to every field, because
  a stored revision has to stand for exactly what was sent: `interval`
  required and a whole number from 1 to 3600, `enabled` a boolean when
  present, each address at most 191 characters (not cut), `urls` an array of
  strings. A body without a `rev` keeps the older, forgiving rules (a
  missing or out of range `interval` keeps the current one, a long address
  is cut) and leaves the device with no revision. Unknown keys are ignored
  either way.

**Responses**: `201 Created` first set · `200 OK` update · `400` with the
field's name as the text: `url` (missing, not http/https), `too many urls`,
`url must match urls[0]`, `url entry`, and with a `rev` also `rev`,
`interval`, `enabled`, `url too long`. Body = the GET shape.

## GET /api/webhook

Current config + runtime stats.

**Response 200**
```json
{
  "url": "http://ha:8123/api/webhook/<id>",
  "urls": ["…", "…"],
  "enabled": true, "interval": 15, "manual_override": false,
  "data_mode": "changed", "gzip": false,
  "networks": "", "on_network": true,
  "status": "ok", "last_post": "2026-07-09T12:00:00Z", "retries": 0,
  "success_count": 42, "fail_count": 1,
  "last_error": "", "last_error_time": "",
  "rev": "9f8e7d6c",
  "reply": { "applied": 1, "ignored": 0, "last": "applied" }
}
```
- **`rev`** (2026-11-14): the revision the pushes report, `""` when none is
  stored or the settings were changed since it was stored. (A device whose
  webhook is pinned still shows its stored one here; its pushes carry no
  `webhook_rev`.)
- **`reply`**: what became of the reply bodies since boot. `applied` counts
  the settings taken, `ignored` the bodies refused, `last` is the verdict on
  the last reply that carried a body: `applied`, `same` (the revision in
  use: nothing to do, counted in neither), `untrusted`, `pinned`,
  `too_long`, `not_json`, `invalid`, `failed` (valid, the device could not
  store it), `""` when no reply has carried a body.

## DELETE /api/webhook

Clear + disable, the revision with it. `204 No Content`.

## Telemetry (device → HA, outbound): the push contract

Every `interval_s`, while enabled + network up, the poster POSTs to the
URL(s) with failover (first 2xx wins). autopid may be off: the push
then carries `status` (+ `config`) only - a status-only push is valid
(guide §4) and is what a fresh device sends until AutoPID is set up
(the old autopid gate silenced fresh devices entirely, 2026-09-08):
```json
{ "schema": 1,
  "status": { "device_id": "…", "fw_version": "6.0.0", "hw_version": "WiCAN-PRO",
              "device_type": "wican_pro", "mdns": "http://wican_<id>.local",
              "bits": {…}, "uptime": "…", "version": "…", "temp_c": 33.1,
              "wifi_mode": "Station", "sta_ip": "192.168.1.23",
              "ble_status": "disable", "batt_voltage": 12.53,
              "sleep_mode": "off", "ecu_pids_num": 12,
              "can_protocol": "ISO 15765-4 CAN, 11-bit, 500 kbit/s",
              "loop_status": "Running",
              "vpn_status": "connected", "vpn_ip": "10.6.0.2" },
  "autopid_data": { "RPM": 820, "Speed": 0,
                    "gps_latitude": -25.3444, "gps_longitude": 131.0369,
                    "gps_altitude": 88.8, "gps_speed": 61.6,
                    "gps_heading": 270.5, "gps_satellites": 7 },
  "config": { … the /data/autopid/config.json tables … },
  "gps": { "latitude": -25.3444, "longitude": 131.0369, "accuracy": 3,
           "altitude": 88.8, "speed": 17.1, "heading": 270.5, "satellites": 7 } }
```
- **`schema`**: payload shape version (contract-v2 ask #3); currently
  `1`, bumped only on breaking changes. Always present.
- **`status.device_id` / `fw_version` / `hw_version`**: GUARANTEED in
  every push (contract-v2 ask #5): the integration's identity check
  (403 on mismatch) and update entity depend on them. Diff mode
  overlays them back after the diff.
- **`vpn_status`**: always present, one of `disabled | waiting | paused |
  connecting | connected`: vpn_manager's own words
  (`vpn_manager_state_str()`, the same as `GET /api/vpn` `state`). Until
  2026-10-16 (issue #985) this file kept a four-entry copy of the table
  that `paused` outgrew: `connected` read past the table and `paused`
  pushed "connecting". The integration treats a word containing `not`,
  `disabled`, `disconnected`, `off` or `down` as the tunnel being down;
  `paused` carries no `vpn_ip`, which clears its backup endpoint the
  same way.
- **`vpn_ip`**: present only while the WireGuard/Tailscale tunnel is
  CONNECTED; HA records the address as the away-from-home backup
  endpoint for registration + control commands.
- **`sta_ip`** (issue #977, added 2026-10-11): the station's IPv4 while
  it holds one (`wifi_manager_get_sta_ip()`): the key the integration's
  station IP diagnostic sensor and the device card's `configuration_url`
  read. The legacy `/check_status` carried it for the polling
  integration; no webhook push ever did, so on a push-based install the
  sensor stayed empty. Omitted without a station address: in `changed`
  mode HA keeps the last one until the station joins again, and a new
  address (a roam, a new lease) is a change that goes out at once. On
  the road the station sits on the ESPNetLink dongle's network
  (192.168.80.x) and that address is reported as it is: `vpn_ip` stays
  the road-side endpoint.
- **`sleep_mode`, `can_protocol`, `ecu_pids_num`, `loop_status`**
  (added 2026-11-15): the keys of the four diagnostic sensors the
  integration's device catalog gives every WiCAN (Sleep mode, CAN
  protocol, ECU PID count, AutoPID loop). No firmware sent them before,
  the 4.x line included, so the four sensors never had a value (seen on
  a first-time setup with Home Assistant 2026.11.1). Like the rest of
  `status` they go out whole in a resync and on a change in `changed`
  mode.
  - `sleep_mode`: `on` | `off`, Power Saving as the device runs it
    (`GET /api/sleep` `enabled`).
  - `can_protocol`: the vehicle protocol in effect, in words, the name
    and detail of its row in `GET /api/autopid/protocols` ("ISO 15765-4
    CAN, 11-bit, 500 kbit/s", "ISO 9141-2, K-line, 5 baud init"): the
    `std_protocol` setting when it pins one, else the current car's
    record; `Automatic` while neither names one (the OBD chip's own
    search picks). The key keeps the catalog's name; the protocol is not
    always a CAN one.
  - `ecu_pids_num`: the number of PID rows loaded (`GET /api/autopid`
    `stats.pids`).
  - `loop_status`: `Running` (polling), `Paused` (one of the pauses of
    `GET /api/autopid` `stats` holds: the low-voltage pause, an app on
    the ELM port, a diagnostic tool, the bus guard) or `Stopped`
    (AutoPID is off).
- **`gps`** (contract §5.4, added 2026-09-09): present while there is a
  LIVE fix, `{latitude, longitude, accuracy (m), altitude (m), speed
  (m/s), heading (°), satellites}`, the same shape as `GET /api/gps`
  (source `usb_acm_cli_gps_get()`, which also serves the espnetlink
  HTTP-polled fix). HA's **Location** device_tracker reads exactly this
  block: the WiCAN Pro profile has `supports_gps=True`, and before the
  block existed the tracker never left "unavailable". In `changed` mode
  the block is sent WHOLE whenever any field changed (a partial block
  means nothing to the tracker); without a live fix it is omitted and HA
  keeps the last known location. The same fix ALSO rides `autopid_data`
  as the `gps_latitude`/`gps_longitude`/`gps_altitude`/`gps_speed`(km/h)/
  `gps_heading`/`gps_satellites` sensors (`autopid_publish_external()` +
  main's GPS sink).
- **`data_mode`** (settings): `full` = every section each cycle;
  `changed` (default) = only keys that changed vs the previous post, empty
  sections omitted (except the identity overlay above). A fresh
  registration always sends a full **resync**, and the push after it is
  a diff (since 2026-10-11; until then the resync never primed the diff
  caches and the next push went out whole once more).
- **`gzip`** (settings, default off, works in EITHER data mode): the
  body is compressed (`Content-Encoding: gzip`) via the ROM miniz
  deflate, contract-v2 ask #9 item 4, the LTE data saver (JSON
  compresses 5-10×). HA inflates against a 2 MiB cap (accepted since
  integration 2026-07-10: leave off for older integrations). Any
  compression failure falls back to an uncompressed post.
- Sections are omitted when empty (e.g. `autopid_data` with no vehicle).
- **TLS**: `cert_set=""` → built-in cert bundle (Nabu Casa / public CAs);
  a `cert_set` names a `cert_manager` CA for a private-cert HA; raw-IPv4
  HTTPS hosts auto-skip CN verification.

- **`status.webhook_rev`** (2026-11-14): the revision of the webhook
  settings in use, `""` when the device has none. In EVERY push, diff mode
  included, so Home Assistant can decide on one push alone. Its presence
  says "this firmware reads the reply to a push". It is left out while the
  webhook is pinned on the device (`manual_override`): nothing is taken from
  a reply then, as nothing is taken from a registration.

**HA responses the poster honors** (contract v2): `204` delivered ·
`503` transient (integration reloading) → retry next cycle · **`403`
identity rejected** → no failover to the 2nd URL that cycle; after 3
consecutive 403-cycles the poster PAUSES (`status: "rejected"` in
`GET /api/webhook`) until the next `POST /api/webhook` registration or
reboot, the contract's "stop; needs user attention" action. Other
failures retry next cycle with `retries`/`last_error` tracked. Any `2xx`
is a delivered push, with or without a body.

### Settings in the reply to a push (2026-11-14)

Home Assistant can only register its webhook settings by calling the
device, and a car that is asleep at that moment, or on LTE without a VPN,
cannot be called, although the car itself reaches Home Assistant with
every push. So Home Assistant may hand the settings over in its reply:

```
device                                        Home Assistant
  | push, status.webhook_rev = "1a2b3c4d"          |
  |----------------------------------------------->|  wants "9f8e7d6c"
  | 200 {"webhook": {"rev": "9f8e7d6c", ...}}      |
  |<-----------------------------------------------|
  | (checked, stored, used from the next cycle)    |
  | push, status.webhook_rev = "9f8e7d6c"          |
  |----------------------------------------------->|  the same: 204
```

There is no acknowledgement and no queue: the device says what it has,
Home Assistant repeats what it wants until the device says the same. A
reply lost on the way is made good by a later push.

The body of a **`200`** is read up to 2048 bytes:

```json
{ "webhook": { "rev": "9f8e7d6c",
               "url": "http://192.168.1.10:8123/api/webhook/<id>",
               "urls": ["http://192.168.1.10:8123/api/webhook/<id>",
                        "https://ha.example.org/api/webhook/<id>"],
               "enabled": true, "interval": 20 } }
```

`webhook` is the body of `POST /api/webhook` with a `rev`, and it is read
by the same code under the same strict rules. What the device does:

| The reply | The device |
|---|---|
| `204`, any `2xx` without a body, `{}`, a JSON object without `webhook` | Delivered, nothing else. |
| `webhook.rev` is the revision in use | Nothing (`same`). |
| A valid `webhook` from a server it may take settings from | Stores the settings and the revision in one write, uses them from the next cycle, reports the new revision (`applied`). The diff goes on: no full snapshot. |
| A body over 2048 bytes, not a JSON object, or a `webhook` with a field that is off | The whole body is ignored (`too_long`, `not_json`, `invalid`). |
| A `webhook` while the webhook is pinned on the device | Ignored (`pinned`). |
| Any reply that is not `2xx` | As before; its body is not read for settings. |

Whatever becomes of the body, the push was delivered: it is not counted
as a failure and not sent again.

**From whom settings are taken.** A reply can change where the device
sends its data, so it is taken only where the server is known to be Home
Assistant or where a registration would be accepted:

- the push went over `https` to a host **name**: the certificate's chain
  and its name were verified, wherever the device is; or
- the push went to a **private address** (10/8, 172.16/12, 192.168/16,
  100.64/10, 169.254/16, IPv6 unique-local and link-local: the home network
  or a VPN tunnel) and did not leave through the station's address while
  the station's network is marked untrusted (Settings > WiFi), the rule the
  network-trust gate applies to `POST /api/webhook`.

Everything else is ignored (`untrusted`): plain `http` to a public address
(the reply crossed the internet in the clear), and `https` to an address
by IP when that address is public or the network untrusted (the chain is
verified there, the name is not compared, so any holder of a public
certificate could answer). The device tells the two ends of the connection
from its socket (`http_client_manager`: `conn_known`, `peer_private`,
`local_ip4`).

**The revision stays honest by itself.** It is stored beside a fingerprint
of the four settings it names (`enabled`, `url`, `url2`, `interval_s`). A
change by any route that knows nothing of revisions (the web interface, a
registration without `rev`, `DELETE`, a restored backup, a factory reset)
no longer matches the fingerprint and the device reports `""`. Home
Assistant then sends its settings again: its options are the source of
truth for these four, as they already are at every start of Home
Assistant.

One W line per kind of refusal (`settings in Home Assistant's reply not
taken: <word>`), its repeats at D; one I line per set of settings taken,
without the address (it carries the webhook id).

## Settings: `PUT /api/settings/ha_webhooks` (version 2)

`enabled`, `url`, `url2`, `interval_s` (1..3600), `data_mode`
(changed/full), `gzip` (v2, default false), `manual_override`,
`cert_set`, `networks`, `rev`, `rev_sum`, `cli`. Reboot-to-apply EXCEPT the
URL/enable/interval, which the discovery push (`POST /api/webhook`) and a
push's reply apply live. Passwords: none. v1→v2 migration adds `gzip:false`.

`rev` and `rev_sum` (2026-11-14, added keys with a default: no version
bump): the revision Home Assistant gave the webhook settings and the
fingerprint of the settings it was given for (8 hex characters). Written
by a registration or a push's reply, never by a person: the web UI's card
shows no row for them and sends them back unchanged with a save.

`networks` (2026-11-08, "active on", an added key with a default: no
version bump): `""` = every network, else the names of station networks
(Settings > WiFi: the main one and the backups) separated by commas,
blanks around the commas ignored, names compared whole. The pushes wait
while the station is not on one of them, or is on no WiFi at all (the
ESPNetLink or USB Ethernet alone): nothing is counted as a failure, the
interval is not consumed, one I line per transition. `GET /api/webhook`
carries `networks` and `on_network` (true with an empty list). A
registration push keeps the stored list (a push between the user's save
and the restart does not write the older live value over it). A network
name that itself contains a comma cannot be listed.

## The rest of the Level-1/2 contract (owned elsewhere)

| Surface | Owner |
|---|---|
| mDNS `_meatpi._tcp` + `_wican._tcp`, TXT `device_type`/`device_id`/`mac` | `mdns_manager` |
| `GET /api/info` (identity for the control API, ask #1) | `api_http` (doc: `dev_status_manager/HTTP_API.md`) |
| `GET /api/status` (`bits` = V6 detection) · `POST /api/restart` | `api_http` |
| `POST /api/rtc/sync` | `rtc_manager` |
| `POST /api/ota/upload` + legacy `/upload/ota.bin` bridge | `ota_manager` / `api_http` |
| `GET /api/logger/export?stream=params&since=<cursor>&limit=<n>` (ask #8) | `data_logger`, see below |

### GET /api/logger/export (ask #8, added 2026-07-18; the shape as built)

Incremental export of the numeric-params log stream: the store-and-forward
source the integration's history backfill reads (`history.py`, aligned to
this shape 2026-11-06). Requires the params stream `format` setting to be
`jsonl` or `csv` (400 `params stream is not jsonl or csv` otherwise; the
integration reports that in its diagnostics and imports nothing). The
integration asks at every start and every reconnect, and a device without
Store and forward logs sqlite: since 2026-11-14 that 400 is sent without
a log line (two W lines of the HTTP server per request until then).

- `stream`: only `params` (default).
- `since`: the cursor of a previous response (`<file-epoch>:<offset>`,
  opaque, exact: a cursor at a retired file resumes at the next newer one
  from its start), or a PLAIN epoch (seconds or milliseconds, 2026-11-06:
  "the rows from that moment"), which starts at the newest file opened at
  or before it. Omitted or 0: the OLDEST retained file.
- `limit`: max rows per response (default 500, cap 2000; a response is
  also capped at 64 KB emitted and 512 KB read from the card).

Response `application/x-ndjson` (`text/plain` for csv): a `{"_file":
"<name>"}` line ahead of each file's rows, the logged rows verbatim
(`{"ts":<epoch MILLISECONDS>,"param":"<source>.<name>","value":n}`; the
AutoPID sink's rows are `autopid.<name>` with the name of the push's
`autopid_data` key; csv rows `ts_ms,source.name,value` under one
`ts_ms,param,value` header per file), then a FINAL meta line
`{"_cursor":"<next>","more":true|false}`: feed `_cursor` back as `since`;
`more=true` means call again at once. There is no `X-Next-Since` header
(the response is chunked; the cursor is known at its end). The active
file's trailing partially-written record is never emitted (the next call
picks it up); rows cut only on record boundaries; the in-band clock-step
markers are never emitted. `GET /api/logger` carries `format`,
`autopid_log` and `export{reads, rows, bytes, age_s, cursor}` (what the
route served since boot). Every read that carried rows keeps a counting
device awake a minute more (ten at most): `data_logger/README.md`,
"Store and forward".
