# fw_update: firmware updates pulled from the meatpi-devices manifests

**Summary.** Every MeatPi device was updated by PUSH until 2026-10-18: a
browser, the Home Assistant integration or a bench script uploaded a bin to
`POST /api/ota/upload`, nothing checked for updates, nothing verified more
than IDF's image checks, there was no rollback. This service component reads
the release list of its device type (`<base_url><device_type>/firmware/manifest.json`
on github.com/meatpiHQ/meatpi-devices, schema 1: `internal/TASK_fw_update.md`
section 3), picks the newest release its channel offers (stable, beta or alfa;
stepping stones and yanked releases honoured, the layout verdict by the
partition table's MD5), tells the user (the web UI's Updates card, the HA
push's `fw_update_available`, the console) and, on request, downloads the
image through ota_manager's pipe (size, sha256 and project name checked
before anything is activated), restarts and verifies the new image. Nothing
installs by itself in v1 (Ali's decision): the user, HA or a fleet tool asks.
Since 2026-10-19 it also updates the ESPNetLink dongle through the WiCAN
(the relay, below), and firmware images are downloaded over WiFi or
Ethernet only, never over the dongle's LTE (Ali): the lists are read on any
uplink so the row can say an update exists, the download refuses with
`cellular_blocked` until the device is on a WiFi network. Sits with the
services: it depends on ota_manager, http_client_manager, partition_migrate,
restart_tracker, config_backup, sleep_manager and the core; the uplink word,
the product's image names and the dongle operations come from the
composition root (`main_glue_wire_fw_update`).

## The ESPNetLink dongle through the relay (2026-10-19)

`fw_update_dongle.c` / `fw_update_dongle_job.c`, generic against
`fw_update_dongle_ops_t` (the endpoint, a hold on the link's own traffic,
the restart notice, an identify on demand), which main wires from
espnetlink_link's `espnetlink_link_endpoint()`, `_hold()`,
`_expect_dongle_reboot()` and `_identify_now()`. The scheduled check also
reads `<base_url>espnetlink/firmware/manifest.json` with the WiCAN's channel
when a dongle is known, selects against the dongle's reported `fw_version`
(the tolerant grammar: `v1.22-61-gcc81591` is 1.22.0, a dev build) and
probes `GET /api/ota/status` on the dongle once per dongle boot: 404 (the
v1.2x releases, the July api-6 builds) = `needs_usb_flash`. `POST
/api/fw_update/install {"target":"dongle"}` (a named version may be a
downgrade) runs on the component's task: the manifest again, the image into
a PSRAM staging buffer of exactly its size (`fwu_fetch_image_to_ram`, three
attempts; the cap is the largest free PSRAM block minus 512 KB, at most 4 MB,
or the bench's `max_stage_bytes`: `no_memory`), the SHA-256 against the
manifest (`sha256_mismatch`), the image head (`fwu_image_head_parse`: an
ESP32-S3 application image, `wrong_image`), the project name against the
manifest's (`wrong_product`), the version against the release's
`fw_version` (`wrong_image`), then the dongle idle (its pairing machine at
rest, up to 20 s: `no_dongle`), the probe again (`needs_usb_flash`,
`dongle_busy`), the link held, and the push: `http_client_manager_upload()`
in 8 KB pieces to `http://<host>/api/ota/upload?sha256=<hex>&source=wican`
with a 30 s bound per write and for the answer (the dongle's digest pass),
`pushing` with the percentage; 200 `{"ok":true}` = `rebooting`: the link's
hold off, the drop waited for (20 s), the return (90 s over USB, 150 s on
its WiFi: `timeout_reboot`), then `pending_verify` until the identify pass
reports the expected version (45 s, an identify every 5 s:
`version_mismatch_after`), `verified`. The staging keeps the dongle's single
httpd worker out of every network stall: nothing reaches it before the whole
image passed its checks here; an abort during the push answers `too_late`.
The WiCAN never restarts for a dongle job; the row's `last_result` lives in
RAM for this boot. Snapshot: `targets.dongle` (HTTP_API.md), verdicts
`not_built | not_present | unreachable | needs_usb_flash | no_manifest |
update_available | up_to_date | layout_mismatch | no_path | unknown_version`.

**The rollback, two layers.** (a) Every unit, now: a job record in NVS
(`fw_update/job`, 112 B, CRC-32) is written at REBOOTING and once more with
the outcome. `fw_update_boot_check()` runs on the boot task right after
`restart_tracker_init()` and before the crash park: a boot that the
crash-loop brake would park (three quick crashes of the new image) marks
the running slot INVALID (`esp_ota_mark_app_invalid_rollback`, which the old
and the new bootloader honour alike) and restarts with the planned reason
`ota_rollback`, so the previous image gets three fresh tries; a boot that
finds the other slot running records `rolled_back` (by the bootloader) or
`boot_failed`; an operated run (a planned restart that is not the OTA
itself, a rollback, a park retry, safe mode or the partition rewrite)
records `verified`. At runtime the milestone is the brake's own 600 s settle
(`restart_tracker_settle()`), or `fwupdate confirm`. Cost: an unrelated
crash loop inside the first ten minutes rolls a good update back; the UI
says so and offers it again. A layout-changing job (`FWU_FLAG_LAYOUT_CHANGED`)
is verified at its first boot and never rolled back: the old image under the
new table would rewrite and format again. (b) `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`
and `CONFIG_BOOTLOADER_PROJECT_VER=2` in the WiCAN PRO's sdkconfig from now
on: a bootloader built with it boots a new image `PENDING_VERIFY` and falls
back at the next reset unless the app confirms (`ota_manager_confirm_running()`
at the milestone, before every planned restart through restart_tracker's
pre-restart hook, and before a new OTA session); a bootloader built without
it ignores the `NEW` state, so mixing is safe both ways. Field units keep
their bootloader (it is never written over the air): (a) is their
protection; (b) reaches units flashed over USB from now on, where one crash
before the confirm already falls back. `GET /api/fw_update` says which
bootloader runs (`rollback.bootloader_version` 2 = with the option).

## API (`include/fw_update.h`)

| Call | Behavior |
|---|---|
| `fw_update_init()` | Log + settings descriptors, the job record read. No network. |
| `fw_update_start()` | The task; the first check 90 s after the network is up, then every `check_interval_h` with +-10 % jitter; the pre-restart confirm hook. Refuses (`ESP_ERR_INVALID_STATE`) when the settings did not apply (standard 4.3 step 5). |
| `fw_update_stop()` | Aborts a job, parks the task (first in the sleep prepare sequence). |
| `fw_update_boot_check()` | BOOT PATH, before `main_park_check()`: the decision table above (`fw_update_record.h`, host-tested). Needs NVS, esp_ota and the restart tracker only. |
| `fw_update_check_now(&id)` | A manifest check on the task; a running check answers its id, an install `ESP_ERR_INVALID_STATE`. |
| `fw_update_install(target, version, flags, &id, why, &already)` | Validates against the last view synchronously (busy, no manifest, no network, unknown / not visible / yanked version, the layout verdict, `FW_UPDATE_F_CONFIRM_WIPE`, the cellular policy with `FW_UPDATE_F_ALLOW_CELLULAR`, 24 KB free internal RAM), then the task re-fetches the manifest (a yank must beat a cached view), downloads through the pipe (three attempts for a transport failure), writes the record and restarts 1.5 s later through `restart_tracker_restart(OTA_APPLY, OTA, id)`. Idempotent on (target, version). `already` = the running firmware is that version. The dongle target answers `dongle_absent` until the relay is built (D1). |
| `fw_update_abort()` | `ESP_ERR_NOT_FOUND` without a job, `ESP_ERR_INVALID_STATE` once the restart is scheduled. |
| `fw_update_status(*out)` | The snapshot (`fw_update_status_t`, ~1 KB: keep it off task stacks): settings, uplink, the last check, this device against the manifest, the job, the last recorded job, the slots. RAM only, so a PSRAM-stack task may call it (the HA poster does): the slot fields are the record the boot task and the confirm helper read from flash, see "Flash reads and PSRAM stacks" below (2026-10-21). |
| `fw_update_confirm()` | The milestone by hand (the console, the bench). |
| `fw_update_forget()` | Erases the record (the bench). Caller on an internal stack. |
| `fw_update_set_event_cb(cb)` | A snapshot after every check and job move (one consumer). |
| `fw_update_set_uplink_provider(fn)` | The uplink word (`wifi`, `espnetlink`, `espnetlink_usb`, `eth`, `none`) and whether it costs mobile data: main's. A mobile uplink refuses every image download (`cellular_blocked`). |
| `fw_update_set_dongle_ops(ops)` | 2026-10-19: the dongle relay's four operations (endpoint, hold, expect_reboot, identify), wired by main from espnetlink_link. Without it `targets.dongle.verdict` is `not_built`. |
| `fw_update_register_http()` / `_register_cli()` | The five routes (HTTP_API.md); the `fwupdate` command, registered by `on_apply` behind `cli`. |

## Files

| File | Role |
|---|---|
| `fw_update.c` | lifecycle, the task (12 KB PSRAM stack: the TLS handshakes run on it, flash never does), the command queue, the one state object (`fwu_state_t`, PSRAM, under the engine lock), the schedule, the sleep hold, the milestone, the pre-restart confirm |
| `fw_update_job.c` | the job runners on the task: the check (this device's manifest, then the dongle's), the install of this device (2026-10-19 split; the notes job went on 2026-11-02) |
| `fw_update_api.c` | the public API with its synchronous refusals (2026-10-19 split) |
| `fw_update_dongle.c`, `fw_update_dongle_job.c`, `fw_update_dongle.h` | the relay (2026-10-19): the dongle's view, check, verdicts, refusals and snapshot; the job (stage, verify, push, the restart, the version read back) |
| `fw_update_fetch_ram.c` | a release's image into a PSRAM buffer of exactly its size (the relay's download) |
| `fw_update_version.c`, `fw_update_core.c`, `fw_update_record.c` | PURE, host-tested: the version grammar (strict and tolerant) and order, channel visibility, the bounded manifest parse, the selection with stepping and yanked, the layout verdict, URL composition; the record codec and the boot decision table |
| `fw_update_fetch.c` | the manifest with its ETag, the image into the pipe (`http_client_manager_stream`) |
| `fw_update_boot.c` | the record store, the internal-stack helper (`fwu_run_internal`), the slots read (`fwu_slots_read_fn`: otadata and the bootloader description, internal stacks only), the boot check |
| `fw_update_status.c` | the snapshot: RAM only, the slots copied from the record above under the engine lock |
| `fw_update_settings.c`, `fw_update_http.c`, `fw_update_cli.c` | the standard's set |

## Dependencies

`PRIV_REQUIRES`: app_update, esp_app_format, esp_bootloader_format,
nvs_flash, esp_timer, esp_http_server, console, esp_hw_support (esp_random),
log_manager, settings_manager, cmdline_manager, http_server_manager,
http_client_manager, ota_manager, partition_migrate, restart_tracker,
dev_status_manager, config_backup, sleep_manager, external_storage;
`espressif/cjson` through `idf_component.yml`. Init order: after
ota_manager_init (the pipe) and config_backup_init; `fw_update_boot_check`
right after `restart_tracker_init`; start after ota_manager_start.

## Settings (`fw_update`, version 1, reboot-to-apply)

| Key | Default | Meaning |
|---|---|---|
| `enabled` | `true` | the scheduled check (90 s after the network, then the interval); off = manual check and install still work, the HA keys read false / "" |
| `channel` | `stable` | `stable`, `beta` or `alfa`: visible(stable) = {stable}, visible(beta) = {stable, beta}, visible(alfa) = all three |
| `check_interval_h` | `24` | 1..168 |
| `base_url` | `https://raw.githubusercontent.com/meatpiHQ/meatpi-devices/main/` | composes the manifest and notes URLs (`image.url` is absolute); `http://` accepted only away from the default (a bench root); must end with `/` |
| `cert_set` | `""` | a cert_manager set for a private release server; empty = the built-in bundle |
| `cli` | `true` | the `fwupdate` command |

`on_validate`: the URL rules, `cert_set` in `[a-z0-9_-]`. There is no
cellular policy knob since 2026-10-19 (Ali): the lists are read on any
uplink, an image is never downloaded over the dongle's LTE
(`cellular.policy` reads `wifi_only` on the snapshot).

## The release notes are the page's own fetch (2026-11-02, Ali)

The device never downloads release notes: they are of no use to it, only to
the person reading the Firmware update page. The snapshot carries each
`latest` entry's `notes_url` (`base_url` + the manifest's `notes` path, `""`
when the release has none) and the page fetches that URL itself, straight
from GitHub (raw.githubusercontent.com answers cross-origin requests), so a
browser without internet shows "could not be loaded" while the device's own
check and install are untouched. Until that day the device proxied the notes
(`GET /api/fw_update/notes`, a 16 KB PSRAM buffer, a 25 s wait on the task)
and a notes file over 16 KB was refused outright; the beta.7 notes had grown
past it. Gone with it: `fw_update_notes()`, the `FWU_CMD_NOTES` command, the
notes fetch and the route.

## Flash-write discipline (standard section 11)

A check writes nothing: the view, the ETag and the selection live in PSRAM
and are rebuilt after a boot. A job writes the NVS record at REBOOTING (one
blob write, from the internal-stack helper) and the outcome once (VERIFIED
at the milestone or on the next boot, ROLLED_BACK / BOOT_FAILED on the next
boot): two writes per update that reached the restart, none for a job that
failed before it (the failure stays in `job` until the next job). The otadata
confirm is one more write when the running image was `NEW` or
`PENDING_VERIFY`. Steady state: `WICAN FLASH writes=0 erases=0`.

## Flash reads and PSRAM stacks (2026-10-21)

The standard's section 2 corollary covers flash writes; a flash READ that
goes through the SPI1 driver (`esp_partition_read`, the otadata map behind
`esp_ota_get_state_partition`, `esp_ota_get_bootloader_description`, NVS)
freezes the cache the same way, and IDF aborts the firmware when the
calling task's stack is in PSRAM (`esp_cache_freeze_caches_disable_interrupts`,
`s_task_stack_is_sane_when_cache_frozen`). 6.0.0-beta.1 shipped the status
builder reading otadata, and the HA webhook poster (a PSRAM stack) builds
its push from the status: every device with Home Assistant webhooks on
crashed at its first push, three times, and parked (the field report of
2026-10-21, reproduced on the bench with `ha_webhook_gate_bench.py`). Rule
in this component since: **`fw_update_status()` reads no flash.** The slots
are read by `fwu_slots_read_fn` on an internal stack only, at init on the
boot task and after every confirm on the helper (`fwu_confirm_fn`, the
milestone and `fw_update_confirm()`), into `fwu_state_t.slots`; the builder
copies that record under the engine lock. The record can lag by one event
at most: an `ota_manager` session that confirmed the running image before
the milestone (an upload in the first ten minutes after an update) shows
`pending_verify` until the milestone's own confirm refreshes it.

## Memory footprint (estimated until the bench measures them)

| Item | Where | Size |
|---|---|---|
| the task stack | PSRAM static | 12288 B (headroom target >= 2 KB; `.\test.ps1 stackaudit`, the bench's `fw_update` task row) |
| the manifest view (`fwu_state_t`) | PSRAM static | about 75 KB (64 releases x 1.1 KB, the selection, the job, the record) |
| the dongle's view (`fwd_state_t`) | PSRAM static | about 73 KB (2026-10-19) |
| the dongle image staged for the push | PSRAM heap, transient | the image's size (about 1.9 MB for the dongle), freed after the push; refused when the largest free block minus 512 KB is smaller |
| the selection, the job, the record, the check | PSRAM static | about 1.5 KB |
| the manifest body during a check | PSRAM heap, transient | <= 64 KB |
| the command queue storage | PSRAM static | 4 x 56 B |
| the internal-stack flash helper | internal heap, transient | 4 KB during a record write or a confirm |
| ota_manager's pipe writer | internal heap, session-scoped | 5 KB during a download (ota_manager/README.md) |
| internal `.bss` | internal | the mutex, the queue and TCB control blocks, a few flags |

Pre-flight before a download: `dev_status_manager_memory()` internal free
>= 24 KB and the largest block >= 8 KB, else `insufficient_ram`.

## Tests

- `host_test/`: 18 Unity tests (its README; 2026-10-19: + the image head
  parser). `.\test.ps1 host fw_update`, or `bash
  tools/testbench/host_gcc_wsl.sh fw_update` under WSL with ASan.
- `tools/testbench/system/fw_update_bench.py` (`.\test.ps1 fwupdate`,
  TESTING.md): the legs f0 to f12 against a release server on rpi001.
- `tools/testbench/wifi/espnetlink_ota_bench.py` (`.\test.ps1 espnlota`,
  TESTING.md, 2026-10-19): the dongle through the relay, legs o0 to o8.
- `tools/webui_preview/probe_fw_update.mjs`: the Updates card over the mock,
  both rows (53 checks).
- `tools/testbench/ha/ha_webhook_gate_bench.py` and
  `ha_webhook_sta_ip_bench.py` (TESTING.md): the HA push built on a PSRAM
  stack from `fw_update_status()`, the canary for a flash read in the
  snapshot (2026-10-21: FAIL with the DUT parked before the fix, PASS
  after). They run on every release build, with `.\test.ps1 fwupdate`
  (legs f3 and f10 read the slot fields after a restart and after the
  milestone, which is the record's refresh).
