# dev_status_manager

## Summary

Core component owning the ONE device-status event group: the single source
of truth for live device state. Feature components publish by setting/clearing
well-known bits (`DEV_STATUS_BIT_*`: STA/ETH/MQTT/BLE connectivity, AP state,
SD mounted, time synced, modes, wake-voltage, …); consumers (LED logic, sleep
manager, status transports, UI) read snapshots or block on bit combinations.
Rewrite of the legacy `dev_status` against Coding Standard rev 2.2: proper
lifecycle, TAG = directory name, bit changes logged at DEBUG (not INFO spam),
pure helpers split out for host testing. Bits are ephemeral runtime state:
they reset on reboot and are **not** settings.

## API

| Function | One-liner |
|---|---|
| `dev_status_manager_init()` | Create the event group; capture running partition + app descriptor. |
| `dev_status_manager_start()` / `_stop()` | Lifecycle uniformity (§3); passive. |
| `dev_status_manager_set/clear/clear_all(bits)` | Publish state changes (masked to the 24 usable bits). |
| `dev_status_manager_get/is_set/all_set/any_set(...)` | Snapshot queries. |
| `dev_status_manager_wait_all/wait_any(bits, timeout)` | Block until a combination is reached (bits not consumed). |
| `dev_status_manager_bit_name(bit)` | "sta_connected", … for logs/JSON. |
| `dev_status_manager_format_uptime(buf,len)` | "HH:MM:SS" / "Nd HH:MM:SS". |
| `dev_status_manager_app_version()` / `_partition_label()` | Running-image identity for status transports. |
| `dev_status_manager_set_app_build(s)` / `_app_build()` | 2026-10-18: the build's git describe beside the release number (`fw_version` is `version.txt` since the constant project name; the commit lives here). Set by main from the `WICAN_FW_BUILD` define; `/api/info.fw_build`, the About page, the HA push. |
| `dev_status_manager_device_id()` | THE device identity: 12 hex chars of the SoftAP MAC (legacy `hw_config_get_device_id`), BLE name (`WiCAN_<id>`), AP SSID and STA hostname all derive from it; no component reads the MAC for naming itself. |
| `dev_status_manager_memory(*out)` | Both heaps (internal + PSRAM): total/free/min_free, and **largest_block** (the fragmentation signal, Architecture §12b) for the INTERNAL heap only. Poll-friendly: nothing in it keeps interrupts off for longer than ~0.15 ms (see "Interrupts and the status routes" below). `psram.largest_block` is 0 = not measured. |
| `dev_status_manager_memory_deep(*out)` | The same with the PSRAM heap's `largest_block`: a walk of ~6000 blocks with interrupts off, 3 to 4 ms. For the boot report (`WICAN MEM`), the console (`system -m`) and `GET /api/status?deep=1`: something a person asked for, never a timer, never a polled route. |
| `dev_status_manager_task_stats(out,cap,*n,*total_us)` | Task monitor snapshot (2026-07-08): per-task name/state/core/prio/stack high-water/cumulative runtime, busiest first + per-core scheduler time. CPU% = deltas between two snapshots ÷ (total-delta × cores). Needs `CONFIG_FREERTOS_USE_TRACE_FACILITY` (+ `_GENERATE_RUN_TIME_STATS`, u64 counter: both in sdkconfig.defaults). Poll-friendly since 2026-10-03: the task lists are walked with this core's scheduler suspended (`xTaskGetNext`, interrupts on, microseconds) and the unused stack bytes are counted afterwards in task context; the kernel's own `uxTaskGetSystemState()` did that inside its critical section, 6 ms with interrupts off for 47 tasks. A task that ends between the two passes may show a meaningless `stack_hw` once. |
| RAM guard (2026-09-22, `TASK_internal_ram.md`) | a 10 s esp_timer started by `dev_status_manager_start()` latches the fault `internal_ram_low` (once per boot, W log with free/min/largest) when the internal heap's free size drops under `DEV_STATUS_RAM_LOW_FREE` (8 KB) or its lifetime minimum under `DEV_STATUS_RAM_LOW_MIN` (4 KB): below ~5 KB the default event loop stops taking the web server's events, BLE does not restart after an AP station leaves and the BT controller drops PDUs silently, all without an E line of their own. `dev_status_task_t.stack_ext` says whether a task's stack lives in PSRAM (`system -t` prints it). |
| `dev_status_manager_temperature(*c)` | Die temperature, °C (ESP32-S3 internal sensor, lazy install). |
| `dev_status_manager_set_usb_provider(fn)` / `dev_status_manager_usb(*out)` / `dev_status_manager_usb_kind_str(k)` | The USB connector summary (2026-10-13): what is on the connector (`espnetlink` / `gps` / `eth` / `other` / `none`) and its one-line state for the web UI's header pill and ESPNetLink bar, served as `usb` on `GET /api/status`. The composition root registers the one provider (it knows the USB components; this manager must not); the status route asks it per request; the kind spelling is pure (host-tested). |
| `dev_status_manager_set_vehicle_provider(fn)` / `dev_status_manager_vehicle(*out)` | The current vehicle summary (2026-10-26): the car the live tables belong to (`name`, `key`, `vin`), whether its Quick Setup finished (`pending`) and how many cars the store holds (`cars`), served as `vehicle` on `GET /api/status` (absent without a current car) for the web UI's notice bar and the Dashboard, which say so when a car whose setup never finished is current beside a set-up one (a device set up on one car and plugged into another). The composition root registers the provider from autopid (this manager must not know it); types in `include/dev_status_vehicle.h`. |

`DEV_STATUS_NETWORK_CONNECTED_MASK` = STA | ETH ("any upstream path").

## Interrupts and the status routes (2026-10-03)

Found by `can_manager`'s new `rx_overrun` counter and the J1939 conservation
bench (TASK_j1939_wwh.md phase 4): a CAN controller's receive FIFO holds
four frames, so anything that keeps interrupts off for longer than four
frame times (2.8 ms at 250 kbit/s and 80 % load, 1.1 ms at 500 kbit/s line
rate) loses frames. Two things did, in routes the web UI polls every few
seconds:

| Call | Interrupts off | Frames lost at 250k, 80 % load, 5 requests/s |
|---|---|---|
| `heap_caps_get_largest_free_block()` on the PSRAM heap (6100 blocks, slow memory; `multi_heap_get_info_impl` walks the pool inside the heap's lock) | 3.1 to 4.0 ms | 74 in 30 s (`GET /api/status`) |
| `heap_caps_get_largest_free_block()` on the internal heap (560 blocks) | 0.14 ms | none |
| `uxTaskGetSystemState()` (47 tasks; the kernel counts every stack's unused bytes inside `xKernelLock`) | 5.9 to 6.1 ms | 478 in 30 s (`GET /api/status/tasks`) |

Hence the routine / deep split of the memory snapshot and the two-pass task
snapshot above. After the change: 0 frames lost over 260 000 frames with
every status route polled five times a second (the bench's `while_polled`
leg guards this from now on, together with a `while_flash` leg: flash writes
cost no frame either, the TWAI interrupt is cache-safe).

Rule of thumb for any status surface: no heap walk (`heap_caps_get_info`,
`heap_caps_get_largest_free_block`, `heap_caps_print_heap_info`,
`heap_caps_check_integrity*`), no `uxTaskGetSystemState` / `vTaskList` /
`vTaskGetRunTimeStats`, nothing else that holds a critical section for
longer than a frame, in a path a page or a timer can reach.

## Dependencies

- `app_update` (running partition/app descriptor), `esp_timer`,
  `esp_driver_tsens` (die temperature): private.
- Init order: early in `main` (after `restart_tracker_init`), before any
  feature component that publishes bits. No settings descriptor.
- Consumers must not invent private connectivity flags: publish here
  (Architecture §2). `wifi_manager` wiring is a follow-up (CHECKLIST).

## Settings (`"dev_status_manager"`, version 1)

Minimal descriptor, one knob: `cli` (bool, default true), register the
`version` / `status` console commands with cmdline_manager on the
settings boot apply (reboot-to-apply). Registered via
`dev_status_manager_register_settings()`, wired by main right after
settings_manager_init because this component inits before it (the
log_manager_register_settings pattern, 2026-07-05). Everything else is
runtime state only.

## HTTP API (requirement: full endpoint reference: `HTTP_API.md` in this directory; conventions: `components/HTTP_API.md`)

`GET /api/status` → all named bits + uptime + version/partition (+ boot
counters from restart_tracker). Implemented by the `api_http` glue: this
component must NOT depend on the HTTP server (layering). No mutating routes:
bits are owned by their publishers.

## Memory footprint

| Where | What | Size |
|---|---|---|
| Internal `.bss` | static event group + app descriptor copy | ~320 B |
| PSRAM / heap / task stacks | none (passive, caller context); task_stats scratch is transient PSRAM; the CLI's fault-table snapshot (1.3 KB) is PSRAM `.bss` since 2026-09-22 | 0 |
| Internal | one esp_timer (the RAM guard) | ~50 B |
| Internal (FIRMWARE-WIDE, task monitoring) | the two runtime-stats Kconfig flags: 828 B static + 8 B/task u64 TCB counters (~330 B @ 41 tasks), **~1.1 KB measured A/B 2026-07-08** (+ ~1 µs esp_timer read per context switch). Not this component's `.bss`, but it owns the feature so the cost is booked here. | ~1.1 KB |
| Internal heap | temperature-sensor driver handle (lazy, first call) | ~100 B |

## Tests

- **Host (`host_test/`, 5 tests):** bit-name mapping (incl. unknown/multi-bit),
  uptime formatting (H:M:S, days, truncation, error paths).
- **On-target (`test_apps/`):** set/clear/mask queries, cross-task waiter
  wakeup, wait timeout, identity helpers, clear-all. Main partition table per
  rev 2.1.

## CLI

`dev_status_manager_register_cli()` (main, CLI builds) registers the `version` and `status` commands with cmdline_manager (`dev_status_manager_cli.c`).

## Device fault codes + health surface (2026-07-19)

The automotive-DTC idea applied to the firmware (meatpi): structural
problems are LATCHED and survive reboots/power cycles until MANUALLY
cleared, a fault that fired once in the field is still visible on the
bench weeks later.

- Store: `dev_status_faults.c`, NVS namespace `dfc` (read and written through `flash_jobs_run()` since 2026-10-31: a fault may be raised or cleared from any task, the HTTP server's PSRAM stack included), 16 slots of
  `{code, detail, count, first/last unix time}`. Wear-disciplined: one
  NVS write on first occurrence of a code + at most one count-update per
  code per boot (recurrences count in RAM). This feature must never
  become the wear bug it guards against.
- Raise: `dev_status_manager_fault_raise(code, detail)` from any task.
- Clear one: `dev_status_manager_fault_clear(code)` (2026-10-10): the boot
  that follows a partition table migration drops `boot_errors`, latched
  by the boots that could not persist (main.c); `ESP_ERR_NOT_FOUND` and
  no NVS write when the code is not latched.
  The composition root raises `boot_errors` (any ESP_LOGE during boot:
  the generic net over every silent degradation), `registry_headroom`
  (a bounded table within 2 of full), `boot_flash_budget` (>64 erases
  during boot: the settings-manager-rewrote-everything class),
  `flash_churn` (erases rising 10 consecutive 60 s windows at runtime) and
  `flash_jobs_refused` (2026-11-01: a flash job of a PSRAM-stack caller
  refused for want of internal RAM for the flash worker; once per boot,
  with the count, from the same 60 s watch).
  Components below dev_status in the graph raise nothing themselves
  (dependency cycles); their ESP_LOGE is caught by the boot_errors net.
- Read/clear: `faults` / `faults -c` CLI, `GET /api/faults`,
  `POST /api/faults/clear`. `/api/status.health` carries the live
  counters (log E/W, flash ops via CONFIG_SPI_FLASH_ENABLE_COUNTERS,
  registry caps, fault count).
- Enforcement: `tools/testbench/system_bench.py` step 6 asserts zero log
  errors, flash budgets (boot + idle-quiet), registry headroom ≥2, task
  stack headroom ≥256 B, and ZERO latched faults.
- `dev_status_manager_flash()`: flash op counters since boot (zeros +
  ESP_ERR_NOT_SUPPORTED when the Kconfig is off).
