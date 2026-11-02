# fw_update: HTTP API reference

> Implemented 2026-10-18 (`fw_update_http.c`, own routes). Conventions:
> `components/HTTP_API.md` section 1. Errors carry the house `error` text
> plus a snake_case `code`: `{"error":"<human>","code":"<snake>"}`.

## GET /api/fw_update

The snapshot. Never mutates, never fetches.

```json
{"enabled":true,"channel":"stable",
 "cellular":{"policy":"wifi_only","uplink":"wifi","is_cellular":false},
 "last_check":{"time":1760745600,"ok":true,"error":"","etag_hit":false,"age_s":120,"next_in_s":86280,"count":3},
 "targets":{
  "self":{"installed":"6.1.0-alfa.1","build":"v6.1.0p-alfa.1-0-gb1adc7e","release_known":true,"dev_build":false,
          "installed_yanked":false,
          "latest":{"version":"6.1.0","date":"2026-10-20","summary":"...","notes_url":"https://raw.githubusercontent.com/meatpiHQ/meatpi-devices/main/wican_pro/firmware/notes/6.1.0.md","size":3764768},
          "available":true,"installable":true,"reason":"",
          "target":{"version":"6.1.0","step":false,"verdict":"none","wipes":[],"layout_md5":"35c16cb5677477a27c662bd912582329"},
          "layout":{"flashed_md5":"35c16cb5677477a27c662bd912582329","build_md5":"35c16cb5677477a27c662bd912582329"}},
  "dongle":{"present":true,"verdict":"update_available","reachable":true,"transport":"usb",
            "installed":{"fw_version":"v1.22-61-gcc81591","device_id":"206ef1894aa5","api_level":9,"release_known":false,"dev_build":true},
            "last_check":{"ok":true,"error":""},
            "latest":{"version":"1.23.0-alfa.1","date":"2026-10-24","summary":"...","notes_url":"https://raw.githubusercontent.com/meatpiHQ/meatpi-devices/main/espnetlink/firmware/notes/1.23.0-alfa.1.md","size":1912400},
            "available":true,"installable":true,"reason":"","target_version":"1.23.0-alfa.1","cellular_needed":false,
            "last_result":null}},
 "job":{"id":7,"active":true,"target":"self","version":"6.1.0","state":"downloading","received":1048576,"total":3764768,"pct":28,"attempt":1,"error":"","detail":"","elapsed_s":12,"transport":"","backup":{"sd":false}},
 "last_result":{"id":6,"target":"self","from":"6.0.2","to":"6.0.3","state":"verified","by":"","error":"","time":1760650000},
 "rollback":{"pending_verify":false,"running":"ota_1","previous":"ota_0","ota_state":"valid","bootloader_rollback":false,"bootloader_version":1}}
```

- `last_check` is `null` before the first check (then `next_in_s` sits at
  the top level); `error` is a code: `fetch_failed`, `not_found`,
  `http_<status>`, `truncated`, `too_large`, `manifest_invalid`,
  `manifest_wrong_device`, `manifest_empty`, `etag_without_view`.
- `targets.self.reason` when nothing is offered or installable:
  `up_to_date`, `no_release`, `unknown_version` (this build is in no release
  and does not parse: manual update only), `no_path` (the stepping stone is
  missing from the channel), `layout_serial`, `layout_unknown`,
  `no_manifest`. `target.verdict`: `none | lossless | wipe | serial | unknown`;
  `wipes` names the partitions a `wipe` formats. `step` = more updates follow.
- `job` is `null` when there never was one; after a job it stays with
  `active:false` and its final state until the next job. States:
  `checking`, `downloading`, `verifying`, `rebooting`, `failed`, `aborted`.
  `job.backup.sd`: a layout-changing update (`wipe`) refreshes the device's
  backup copies before anything downloads, and `sd` says the card copy was
  written; the step runs while the job is still `checking`, so a status
  that says `downloading` carries the step's result (since 2026-10-31; it
  used to move to `downloading` first).
  Failure codes in `error` (the sentence in `detail`): `manifest_fetch_failed`,
  `unknown_version`, `yanked`, `layout_serial`, `layout_unknown`,
  `needs_confirm_wipe`, `busy_upload`, `no_slot`, `image_too_large`,
  `size_mismatch`, `image_fetch_failed` (after three attempts),
  `image_not_found`, `sha256_mismatch`, `wrong_product`, `wrong_image`,
  `image_invalid`, `flash_failed`, `aborted`.
- `last_result` comes from the NVS record (a job that reached the restart),
  so it survives the reboot and a power cycle: `pending_verify` while the
  new image runs unconfirmed, then `verified`, `rolled_back` (`by` = `app`
  after three quick crashes, `bootloader` after one under a rollback-enabled
  bootloader) or `boot_failed`.
- `rollback.ota_state`: `new` (written by this app under the field
  bootloader), `pending_verify` (the new bootloader's word), `valid`,
  `invalid`, `aborted`, `undefined` (a factory-flashed slot).
  `bootloader_version` 2 = built with `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`.
  The `rollback` object is the record the firmware read from flash at boot
  and after its last confirm (the milestone, `fw_update_confirm()`), not a
  live read: the snapshot itself touches no flash (2026-10-21, README
  "Flash reads and PSRAM stacks").
- `cellular.policy` is the constant `wifi_only` (2026-10-19, Ali): the lists
  and the notes are read on any uplink, an image is never downloaded while
  `is_cellular` is true (the dongle's LTE carries the default route). The
  install answers 409 `cellular_blocked` then; nothing in the request
  overrides it.
- `targets.dongle` (2026-10-19, the relay; README): `verdict` is
  `not_built` (no relay wired), `not_present`, `unreachable`,
  `needs_usb_flash` (api below 7, or the dongle's `GET /api/ota/status`
  answered 404: a v1.2x release, flash it once over USB), `no_manifest`,
  `update_available`, `up_to_date`, `layout_mismatch`, `no_path` or
  `unknown_version`. `installed` is what the dongle reported at the last
  identify (`fw_version` as it says it, `dev_build` when that string is a
  git describe); `latest` / `available` / `installable` / `reason` /
  `target_version` as for `self`; `last_check` is the dongle manifest's
  (`null` before the first); `cellular_needed` says the download would cost
  mobile data (and is refused); `last_result` is the last relay job of this
  boot: `{id, from, to, state verified|failed|aborted, error, detail, time,
  push_s}`. The shared `job` carries `target:"dongle"` and `transport`
  (`usb` | `wifi`) with the states `checking`, `downloading`, `verifying`,
  `pushing` (`received` = bytes sent), `rebooting` (the dongle restarts),
  `pending_verify` (its new version is being read), `verified`, `failed`,
  `aborted`. Failure codes of a dongle job: `no_dongle`, `needs_usb_flash`,
  `dongle_busy`, `no_memory`, `sha256_mismatch`, `wrong_image`,
  `wrong_product`, `dongle_rejected`, `push_failed`, `timeout_reboot`,
  `version_mismatch_after`, plus the download codes above.

## POST /api/fw_update/check

`{}` -> 202 `{"job_id":N,"state":"checking"}`. A check already running
answers 202 with the same id; 409 `busy` while an install runs. Zero flash
writes: the result is in `last_check` and `targets` a few seconds later.

## POST /api/fw_update/install

```json
{"target":"self","version":"6.1.0","confirm_wipe":false}
```

`target` is `self` or `dongle` (2026-10-19). `version` omitted = the
computed target; for the dongle a named version may be a downgrade.
`max_stage_bytes` (dongle, the bench) caps the staging buffer. 202
`{"job_id":N,"state":"checking","target":"self","version":"6.1.0","from":"6.0.3"}`
(`from` is the dongle's reported version for the dongle); the same
(target, version) while that job runs -> 202 with that job; the running
firmware already is that version -> 200 `{"already":true,"installed":"6.1.0"}`.

| Status | `code` | When |
|---|---|---|
| 400 | `bad_request` | not JSON, an unknown target |
| 404 | `unknown_version`, `not_visible`, `yanked`, `no_release` | the version is not in the manifest, not on the channel, withdrawn; the channel has nothing |
| 409 | `busy`, `busy_upload` | another job, an upload session |
| 409 | `no_manifest` | nothing fetched yet: check first |
| 409 | `no_network` | the device is offline |
| 409 | `up_to_date` (omitted version only answers 200 `already`) | |
| 409 | `no_path` | the stepping stone is not on the channel |
| 409 | `needs_confirm_wipe` (+ `"wipes":["storage"]`) | the layout formats data: send `confirm_wipe:true` after the backup |
| 409 | `layout_serial`, `layout_unknown` | a USB flash is needed |
| 409 | `cellular_blocked` | the uplink is the dongle's mobile connection: downloads wait for WiFi (no override, 2026-10-19) |
| 409 | `insufficient_ram` | under 24 KB free internal RAM or an 8 KB largest block |
| 409 | `dongle_absent` | the dongle target on a firmware without the relay wired |
| 409 | `no_dongle` | the dongle target: no dongle known, or not reachable |
| 409 | `needs_usb_flash` | the dongle target: its firmware has no update route (api below 7, or `/api/ota/status` answered 404) |
| 409 | `no_memory` | the dongle target: the image does not fit the PSRAM staging buffer |

The orchestrator recipe (HA, a fleet tool): POST install, remember `job_id`,
poll GET until `job.state` is `rebooting` or the connection drops, wait for
`/api/info`, then GET: `last_result.id == job_id` with `state` in
`pending_verify|verified` and `/api/info.fw_version == version` = success;
`rolled_back` / `boot_failed` / `failed` carry `by` / `error`. A 404 on
`GET /api/fw_update` tells an integration to use the push route instead.

## POST /api/fw_update/abort

200 `{"aborted":true,"job_id":N}`; 409 `no_job`; 409 `too_late` once the
restart is scheduled, or once a dongle image is being pushed (the dongle's
httpd would be left waiting). The download stops at the next chunk and the
slot is left as scratch; `job.state` reads `aborted`.

## The release notes

There is no notes route since 2026-11-02 (`GET /api/fw_update/notes` is gone):
each `latest` object of the snapshot carries `notes_url`, the absolute address
of the release's notes on the release server (`base_url` + the manifest's
`notes` path; `""` when the release has none), and the Firmware update page
fetches it in the browser. The device never downloads them.
