# flash_jobs: the internal-stack worker for flash work

**Summary.** Every access to the SPI flash (a LittleFS or NVS read or
write, an OTA write, a partition erase, an `esp_partition_mmap`) switches
the flash cache off for the SPI transaction, and a task whose stack lives
in PSRAM cannot run with the cache off: IDF asserts it
(`spi_flash/cache_utils.c`, `esp_task_stack_is_sane_cache_disabled`) and
the device reboots with a backtrace. The component standard's rule (section
2): such work runs on a task with an internal-RAM stack. Until 2026-10-31
the tree had one such task per need (the filesystem's writer, the OTA
pipe's writer, the script loader, the pairing store, the fw_update helper,
the factory reset task) and the HTTP server task kept an 8 KB internal
stack because its handlers write settings. This component is the shared
answer: `flash_jobs_run(fn, arg)` runs `fn(arg)` on an internal stack
whoever the caller is, and the HTTP server task moved to a PSRAM stack the
same day (`internal/TASK_httpd_psram.md`).

**How it runs a job.** In line when the calling task's stack is internal
(`esp_ptr_internal` on a local: the main task at boot, the console tasks,
the worker itself inside a job), otherwise on the worker task with the
caller blocked until the job returns (a mutex serializes callers, so one
job is published at a time and its argument, wherever it lives, stays
valid). The worker is created at the first marshalled job (`xTaskCreate`,
a 4 KB stack from the internal heap, priority at least 6 and one above its
caller so a job never waits behind the task waiting for it) and leaves
after 5 s without a job (`vTaskDelete(NULL)`; the idle task returns the
stack), so a device that is not writing pays nothing at idle. The lifetime
decisions are pure (`flash_jobs_core.c`, host-tested): a job published a
moment after the worker's wait ran out is still run (the worker checks for
a pending job before it leaves), and a caller that arrives after the
worker decided to leave creates the next one. A job may call
`flash_jobs_run()` itself (in line: the worker's stack is internal).

**What a job may do.** Anything that touches the flash. Its argument may
live in PSRAM (the caller's stack included): the flash driver copies
through an internal bounce buffer when a source or destination is not in
internal RAM. What a job must not do: wait for its caller, or for the lock
of a caller that is itself blocked on a job.

**Who routes through it** (2026-10-31): settings_manager (every mount,
load, save, existence check and wipe of a settings file), filesystem (every
`/data` operation of a PSRAM-stack caller: `fs_run_internal`, the stream
helpers, the async pipe's chunk writes), restart_tracker (the stored crash
report), dev_status_manager (the latched faults), ota_manager (the update
session: begin, every write, end, abort, the confirm of a pending image),
config_backup (the backup key's NVS record). Since 2026-11-01: the
Tailscale client's NVS work (the device keys at init, the peer cache)
through microlink's `flash_run` hook, which vpn_manager sets to
`flash_jobs_run`, so the `vpn_state` and `ml_wg_mgr` tasks live on PSRAM
stacks; the script engine's file read (`script_engine_run_file`, was the
one-shot `se_load` task); fw_update's otadata and record payloads
(`fwu_run_internal`, was the one-shot `fwu_flash` task); the ESPNetLink
pairing store runs on its caller's stack now (settings_manager marshals
the flash part; was the one-shot `espnl_store` task) and the autopid
vehicle writer's stack moved to PSRAM (the filesystem marshals; was 6 KB of
internal `.bss`). Not through it, on purpose: the factory reset's own 8 KB
internal task (`nvs_flash_init`'s page scan is deeper than the worker's
stack), the OTA pipe's session writer (an asynchronous queue of chunks:
the download overlaps the flash writes, which a blocking job would not),
safe mode's server (the recovery path), the NimBLE bond store (the host
task keeps its 4 KB internal stack: `internal/TASK_ble_tailscale_margin.md`
lever 3, gated by `blesec` on a re-cabled rig).

**Failure modes.** A marshalled call when the internal heap cannot give the
worker its stack: `ESP_ERR_NO_MEM`, the job did not run, one W line per
10 s, `refusals` counted. The caller says what it could not do (a settings
save answers "change could not be saved"); main's 60 s watch latches the
`flash_jobs_refused` fault once per boot when the count is not zero (the
automotive-DTC layer: a device at its internal-RAM cliff keeps saying so
until `faults -c`). A call from a PSRAM stack before `flash_jobs_init()`:
`ESP_ERR_INVALID_STATE` (main inits it right after the log manager, before
anything that could call from PSRAM).

**Where the counters show.** `system -m` (the console), `GET /api/status`
`health.flash_jobs` (`inline`, `marshalled`, `spawns`, `exits`, `refusals`,
`stack_hw_min`, `worker_alive`), and the heap probe prints them per row so a
reading taken while a worker is alive (its 4 KB and TCB for the 5 s after
the last job) is read as such.

## API (`include/flash_jobs.h`)

| Function | What |
|---|---|
| `flash_jobs_init()` | the log descriptor and the FreeRTOS objects; from main, before the filesystem and the settings manager |
| `flash_jobs_run(fn, arg)` | run `fn(arg)` on an internal stack and return its result |
| `flash_jobs_on_internal_stack()` | true when the calling task's stack is in internal RAM |
| `flash_jobs_stats(out)` | the counters: inline runs, marshalled runs, spawns, exits, refusals, the lowest worker watermark seen, whether a worker is alive (`system -m` prints them) |

## Dependencies

`esp_hw_support` (`esp_ptr_internal`), `esp_timer` (the clock of the
refusal log rate limit), `log_manager`. No settings, no CLI, no routes.

## Memory footprint

Internal: three static FreeRTOS objects (the mutex and two binary
semaphores, about 300 B), the state (48 B), a spinlock. While a worker is
alive: its 4 KB stack and TCB from the internal heap, for the 5 s after the
last job. PSRAM: none. The worker's watermark is logged at DEBUG at every
exit (`worker idle, leaving: stack_hw=`) and kept as `stack_hw_min`. The
stack was 6 KB on 2026-10-31; the night's whole bench battery (settings
saves, uploads, an OTA session, crash reports, the backup copies' sweeps)
used 2.5 KB of it at most (`stack_hw_min` 3576), so it is 4 KB since
2026-11-01, which also shrinks the post-boot dip the backup copies' first
sweep sets (the first PSRAM-stack caller of a boot creates the worker:
`tools/testbench/system/boot_dip_probe.py` named it, 5.7 KB at 27 s with the
6 KB worker). An internal stack's overflow is caught by FreeRTOS
(`CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY`), never silent.

## Boot order

`main_boot_init("flash_jobs_init", ...)` right after `log_manager_init`:
restart_tracker's boot NVS read, the settings mount and the filesystem
mount all run in line on the main task's internal stack, so the order only
matters for the first PSRAM-stack caller, which comes much later.

## Tests

- `host_test/`: Unity tests of the pure core (`flash_jobs_core.c`): where a
  call runs, the worker's creation and exit, the publish-at-the-timeout
  race, the counters, the refusal log rate limit (`.\test.ps1 host
  flash_jobs`).
- Bench: every stage that writes settings, uploads a file or an image, or
  reads the crash report over HTTP exercises the worker from the HTTP
  server's PSRAM stack; IDF's assertion turns any missed path into an
  abort with a backtrace. The record is in `TESTING.md` and
  `internal/TASK_httpd_psram.md`.
