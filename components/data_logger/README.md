# data_logger

Generic record logger (successor of legacy `obd_logger`): producers
push timestamped records; this component owns storage on the SD card,
batching, file rotation, retention, and the storage-engine choice.
**Two streams** since 2026-07-09 (TASK addendum): numeric params and
raw CAN frames, each with its own format, file series, rotation and
retention, their rates and tooling differ wildly (autopid ≤ ~19
samples/s vs a 500 kbit/s bus peaking ~4,000 fps).

## Model

```c
dl_param_t rpm;
data_logger_register_param("autopid", "rpm", &rpm);   /* once, cheap */
data_logger_write(rpm, 850.0);                        /* never blocks */
```

Records land in PSRAM rings (drop-oldest + counter); one writer task,
the only file toucher, PSRAM stack (SD-only IO), drains both rings in
batches. A yanked card parks the writer while the rings keep
absorbing; producers never stall. Params and frames have SEPARATE
rings so a CAN flood can never evict param records (frame ring sized
by `ring_len`).

Three files share that work (split 2026-10-13, the standard's 700-line
rule): `data_logger.c` is the core (the registry, the rings, the
lifecycle, the public API), `data_logger_writer.c` the writer (the task,
the two streams and their engine binding, the batches, the gate, the
clock hooks) and `data_logger_store.c` the card side (open or resume a
stream's file, rotate, retention, the set-aside of a corrupt file, the
faults). The writer decides when, the store decides how; the core never
touches a file.

## Producers

- **autopid → params**: the `autopid_log` setting (`off | changed |
  all`). main wires `autopid_set_value_sink(data_logger_autopid_sink)`;
  autopid feeds every accepted sample (with a changed flag), the
  setting picks none / on-change / every-sample. Params appear as
  `autopid.<name>`. For SELECTIVE per-param logging use the rule path
  instead (`on autopid.param match {param} → logger.write`).
- **CAN → frames**: `can_log` subscribes into can_manager's RX fan-out
  (needs can_manager `enabled`; `silent` mode recommended for passive
  logging). `can_filter`/`can_mask`/`can_ext` (hex strings) select
  ids; empty filter = ALL frames. One small drain task moves the
  subscription queue into the frame ring.
- **rules**: the `logger.write {source?, name, value}` event action,
  any event's values, user-selected (see Events below).

## Storage engines (`format` / `can_format` settings)

Each stream binds its own engine instance (any combination, two files
open at once). Params: `sqlite | csv | binary | jsonl`. CAN:
`binary | csv | sqlite | mf4 | blf | candump | asc | jsonl`:

| format | file | streams | why |
|---|---|---|---|
| `sqlite` | `.db` (`params`+`records` / `frames` tables) | both | legacy-compatible `.db` tooling; ~700 rows/s tuned: CAN only for filtered/slow streams |
| `csv` | `.csv` (`ts_ms,source.name,value` / `ts_ms,id,ext,rtr,dlc,data`) | both | spreadsheet/pandas friendly |
| `binary` | `.wdl` packed + inline dictionary | both | THE busy-bus option (~170× sqlite); convert offline: `tools/wdl_dump.py --to csv\|candump\|trc\|text` |
| `mf4` | `.mf4` MDF 4.10 (ASAM) | CAN | the bus-logging standard: asammdf / CANoe / MATLAB / INCA (CANedge-style). CAN_DataFrame channel group; readable to the last commit (in-place DT/CG patching) |
| `blf` | `.blf` Vector BLF | CAN | Vector-shop native, python-can readable; uncompressed LOG_CONTAINERs v1 (zlib = future); header re-patched per commit |
| `candump` | `.log` SocketCAN text | CAN | can-utils replay / SavvyCAN import; absolute epoch stamps |
| `asc` | `.asc` Vector CANalyzer text | CAN | CANalyzer/CANoe text tooling |
| `jsonl` | `.jsonl` one JSON object per line | both | universal (jq / pandas / anything) |

mf4/blf/asc are **single-use** (relative timestamps / patched headers):
every boot/rotation starts a fresh file, no cross-boot append. PEAK
TRC stays offline (`wdl_dump.py --to trc`) unless users ask.

Files live in `/sd/logs`, rotate at `max_file_mb` / `can_max_file_mb`,
and the oldest are deleted beyond `max_files` / `can_max_files`:
retention is per stream (prefix-scoped) and spans engine switches.
Zero-padded epoch names make lexical order = age order.
Browse/download/delete via the existing `/api/fs` routes. The ACTIVE
file of each stream is write-locked (`CONFIG_FATFS_FS_LOCK`,
2026-07-09): downloading or deleting it fails with an error instead of
corrupting the card (unlink-while-open = orphaned FAT chains), pause
via `/api/logger/gate` (closes + flushes both files) or wait for
rotation, then fetch. Rotated files are always free.

## Settings (`data_logger`, v2, reboot-to-apply)

`enabled` (false), `format` (sqlite|csv|binary|jsonl), `max_file_mb`
(1 to 32, 4), `max_files` (1 to 500, 100), `batch_rows` (16 to 1024, 256),
`flush_ms` (100 to 60000, **5000** since 2026-09-07), `autopid_log` (off|changed|all, off),
`can_log` (false), `can_format`
(binary|csv|sqlite|mf4|blf|candump|asc|jsonl, binary), `can_filter`
(hex id, "" = all), `can_mask` (hex, "7FF"), `can_ext` (false),
`can_max_file_mb` (1 to 128, 8), `can_max_files` (1 to 500, 50), `ring_len`
(512 to 8192, 2048), `cli` (true). v1→v2 migration is a pass-through;
format-enum ADDITIONS are schema-compatible (no version bump).

## Events (event_manager)

Sources: `logger.rotated {file}` (both streams, the prefix tells them
apart), `logger.error {count}`.
Actions:
- `logger.enable` / `logger.disable`: rule-driven gating ("log only
  while driving"). Paused keeps filling the rings, so an enable rule
  also lands the newest pre-trigger records.
  A gate change notifies the writer task (2026-09-06), so the files close
  or reopen right away instead of after the `flush_ms` nap: the export
  relies on that to read the active file.
- `logger.write {source?, name, value}`: rule-selected values into
  the params stream, e.g. `match autopid.param -> logger.write
  {"source":"autopid","name":"${param}","value":"${value}"}`.

## Surfaces

- `GET /api/logger`: status JSON: param-stream fields at the top
  level (enabled/running/paused/storage_ok, file, rows, files,
  written/dropped/errors/rotations) + a `can{}` block (enabled, file,
  file_rows, files, queued, frames_written, frames_dropped,
  rotations) + a `clock{}` block (valid, source, steps_this_boot,
  rotations_by_clock, journal_bytes, journal: the "Time" section).
- `GET /api/logger/export?stream=params&since=<cursor>&limit=N[&name=<param>]`
  (`data_logger_export.c`): incremental pull of the params stream (jsonl
  and, since 2026-09-06, csv: 400 for the other engines, whose files the
  UI fetches whole through `/api/fs/download`): a `{"_file":"<name>"}`
  line ahead of each file's rows, the rows verbatim
  (`{"ts":<epoch ms>,"param":"<source>.<name>","value":n}`, or
  `ts_ms,source.name,value` for csv), then a FINAL
  `{"_cursor":"<epoch>:<offset>","more":bool}` meta line. `since` takes
  that cursor back (exact: a retired epoch resumes at the next newer file
  from its start) or, since 2026-11-06, a plain epoch (seconds or
  milliseconds: "the rows from that moment", for a client that only
  remembers the time of its last import), which starts at the newest file
  opened at or before it (the pure `dl_export_start()` in
  `data_logger_cursor.c`, host-tested); nothing or 0 starts at the oldest
  file. `name` (2026-09-06) filters to one parameter (the value after the
  last `.` or the full `source.name`); the cursor walks every line either
  way, bounded per request by 512 KB read / 64 KB emitted / `limit` lines
  (cap 2000). Every read window re-seeks to the cursor (the old loop
  advanced the file position by the whole window but the cursor by the
  kept part, so a second window in one response started mid-record). A
  client that goes away mid-response stops the card reads. The web
  dashboard's chart tiles backfill from it, and so does the Home Assistant
  integration (below). A page ends at a line boundary, always: a line that does not fit the
  page's byte budget opens the next page (until 2026-11-06 a read window
  whose first line did not fit went out raw, about one page in sixteen,
  its cut tail row glued to the cursor line, and a client stopped paging
  there; the walk is the pure `dl_export_walk()`, host-tested).
- **Store and forward (2026-11-06).** A car away from home logs its
  parameters here (`autopid_log` on, `format` jsonl or csv); once it is
  back, the Home Assistant integration (`meatpi-multi-device` branch)
  pulls the rows through the export route from the cursor it kept and
  imports them into each sensor's long-term statistics at the recorded
  hours. The device's part: `GET /api/logger` reports `format`,
  `autopid_log` and an `export{reads, rows, bytes, age_s (-1 = never),
  cursor}` block (what a consumer has read), the `logger` command prints
  the same line, the web UI's Automate > Home Assistant card has a Store
  and forward switch that stages the three keys, and every export read
  that carried rows is reported to the composition root
  (`data_logger_set_reader_cb()`, `data_logger_reader_cb_t`), which hands
  it to `sleep_manager_activity()`: a car at home has its engine off and
  the sleep countdown running, so the read pushes the countdown out a
  minute at a time (ten minutes at most), and the backfill finishes before
  the entry instead of waiting for the next drive. With a VPN on
  (`vpn_manager.enabled`) the card warns that the integration reaches the
  device away from home too and pulls the log over mobile data as well
  (Ali, 2026-11-06: the pull stays as it is, no network rule, a warning).
  Bench:
  `tools/testbench/ha/ha_history_backfill_bench.py` (`.\test.ps1
  habackfill`, `HA BACKFILL PASS`) against the bench Home Assistant.
- `GET /api/logger/gpx?since=<cursor|epoch>&until=<epoch>&limit=<points>`
  (2026-11-06, `data_logger_gpx.c`): **the GPS track.** The fix is published
  as the AutoPID parameters `gps_latitude`, `gps_longitude`, `gps_altitude`,
  `gps_speed`, `gps_heading` and `gps_satellites`, so Store and forward
  records it; this route gathers those rows by stamp (one fix = one point,
  within 0.2 s: the fields of one fix share a stamp to the millisecond,
  and a 1 Hz track must not merge neighbours) into a static PSRAM table
  of up to 4000 points (sorted;
  rows out of order find their point) and writes a GPX 1.1 document: a
  `<trkpt>` per fix with `<ele>`, `<time>` and `<extensions>` (speed in
  m/s, course, satellites), a new `<trkseg>` after ten minutes without a
  fix, and a trailing comment `<!-- cursor="<file-epoch>:<offset>"
  more="true|false" points="N" -->` to continue from (`limit` default 2000,
  max 4000; 3 MB read per request). jsonl or csv only (400); one request
  at a time (409). It counts as an export read (the counters, the reader
  hook). Home Assistant's own history cannot hold a position (no past
  state, no statistics for a location): the time series lives in this
  file. The Logger page's GPS track card downloads it for a window; the
  HA integration's `wican.save_gpx` service saves it under `www/wican/`.
- `POST /api/logger/gate` `{"enabled":bool}`: runtime (non-persisted)
  gate for BOTH streams; the HTTP twin of the `logger.enable`/
  `logger.disable` event actions. Resets on reboot.
- CLI `logger`: both streams' status, the clock line and the export line;
  `logger test <rows> [<source> <name> [<age_s>|@<epoch_s> [<step_ms>]]]`
  queues synthetic records (`i * 0.5`; the default `test.value` stamped
  now, or any `source.name` starting `age_s` ago, or at the absolute
  `@<epoch_s>`, at `step_ms` apart with explicit stamps: the
  store-and-forward bench's known series in past hours, two series on
  the same stamps for the GPS track),
  `logger frametest <n>` synthetic frames (engine throughput without a bus).

## Time (2026-10-13, issue #984)

Every stamp is **UTC epoch milliseconds from the system clock** at the
moment the record was taken (`gettimeofday`; file names carry the epoch
of their open). The system clock is rtc_manager's business (restored from
the RX8130 at boot, SNTP on every connect, a manual set); the logger's
business is what happens to rows when that clock is set or moves:

- **Every record carries the monotonic time it was taken at** beside its
  wall stamp (`mono_ms`, in what used to be the record's padding: 32
  bytes as before). Before every batch the writer catches up with
  rtc_manager's clock steps by generation and **re-bases the rows queued
  before a step** (adds the step's delta), so rows produced before an
  SNTP sync and written after it carry corrected stamps. A record with
  an explicit stamp (`data_logger_write_at`) or one salvaged from before
  a reset is never re-based (`DL_REC_ABS`).
- **The relative formats (asc, mf4, blf) take their time from the
  monotonic stamps**: a wall step cannot make them negative or run
  backwards. mf4 and blf re-patch their header start at every commit and
  at the close from the wall clock as it is then, so a file whose clock
  was set or stepped while it was open carries the corrected start; asc
  keeps the date it was opened with (the journal has the rest).
- **Rotation on the clock**: the first valid clock of a boot and any step
  of a minute or more (`DL_CLOCK_ROTATE_MS`) close both files and open
  new ones named by the corrected clock (a backward step keeps the names
  monotonic: `newest + 1`, the order on the card stays the order of
  writing). Smaller steps are journaled and marked, not rotated. A boot
  without a valid clock never resumes a file whose rows were stamped by a
  real clock: it opens its own (named `newest + 1`, the rows at 1970 +
  uptime), and the first valid clock rotates again.
- **The clock journal `/sd/logs/clock.jsonl`**, one JSON object per line,
  written only when something happens (standard 11), bounded to the
  newest 32 KB past 64 KB: `boot` (the restart tracker's boot count, the
  wall and the uptime at the writer's first lap, whether the clock was
  valid), `valid` (the first valid clock of the boot) and `step` (every
  later set) with `ts_before`, `ts_after`, `delta_ms`, `source` and the
  files that were open, `rotate` (from, to, why). The device's own record
  of what its clock did: `tools/log_clock.py --show` prints it,
  `--journal clock.jsonl --rebase <file>` re-bases a jsonl, csv, candump
  or .wdl file written before a step from it (no guessing per boot).
- **In-band markers** where the format has room: jsonl `{"ts","event":
  "clock_step","delta_ms","source"}`, params csv `ts,logger.clock_step,
  delta`, sqlite table `clock_steps(ts, delta_ms, source)`, `.wdl` record
  `0x04` (only as the last record before a close: an older reader stops
  at it as at a torn tail). candump, asc, mf4, blf and the CAN csv rely
  on the journal alone. `/api/logger/export` never emits a marker as a
  point and names each file's rows with a `{"_file":"<name>"}` line.
- `GET /api/logger` `clock {valid, source, steps_this_boot,
  rotations_by_clock, journal_bytes, journal}`; `logger` prints the same
  line; the web UI's Logger card says how the clock was set and what
  moved, the Files table labels files the journal lists ("clock not set
  until ..."), the dashboard history re-bases points per file with the
  journal.
- Not changed: stamps before the first valid clock stay 1970 + uptime
  (the journal and the UI say so); the logger never withholds rows
  waiting for a clock (a car-only unit without internet still logs). The
  unsynced rows of a device that never syncs cannot be re-based (no step
  ever): the journal's `boot` lines segment them per boot.

## Robustness (power cuts, crashes, corrupt files)

See `ROBUSTNESS.md` (2026-09-07): the fail-case matrix and what each
case costs. In short: sqlite commits are atomic (the port syncs again,
`journal_mode=PERSIST` + `synchronous=FULL`); text and `.wdl` files get
their torn tail cut on resume; a corrupt file is set aside as
`<name>.corrupt` (fault `logger_file_corrupt`, best-effort salvage into a
fresh file of the old name, `tools/db_recover.py` for the thorough pass)
and logging goes on in a new file; the registry and both rings live in a
PSRAM `.noinit` envelope so a crash or watchdog loses nothing queued
(`salvaged` in `/api/logger`); `data_logger_stop()` is synchronous and
an `esp_restart` shutdown hook calls it, so sleep entry and every
planned restart close the files first.

## Testing

- Host: `host_test/` (42 tests: file-name/prefix rules, the export's start, line and window rules, hex filter
  parse, `.wdl` + csv frame-encoder golden vectors, the recovery
  helpers (torn text/.wdl tails, set-aside names, salvage sanity, CRC),
  and the clock rules (the 32-byte record layout, the re-base incl. the
  modulo-32 stamp compare, the monotonic relative time under a backward
  wall step, the rotation rule, the journal lines and markers byte-exact,
  the mf4 start offsets)).
- Live: `tools/testbench/data_logger_bench.py` (`test.ps1` stage
  `live datalog`), the full format matrix with CONTENT validation
  (python sqlite3 / csv / wdl_dump byte-exact vs PCAN-sent frames),
  the autopid sink leg, the id filter leg, rotation/retention, the
  runtime gate, and the paced CAN-rate benchmark. CAN legs self-skip
  without a PCAN.
- The clock: `tools/testbench/system/log_clock_bench.py` (`.\test.ps1
  logclock`, `LOG CLOCK PASS`): an SNTP actor on the Pi steps the DUT's
  clock forwards and backwards while the logger writes (the journal, the
  markers, the re-based rows, the mf4 times), the unsynced boot after
  `rtc --forget` and a PSU power cycle, the re-base tool. TESTING.md.

## Why the sqlite pragmas look like that

Legacy toggled `journal_mode` per store call and got the DELETE-journal
worst case; setting the pragmas once + batching was the 5.6× fix
(`BENCHMARKS.md`: legacy 130 → tuned 724 rows/s; littlefs-on-SD
rejected, 7 to 13× slower everywhere). Until 2026-09-07 the port also
compiled out xSync and ran `journal_mode=MEMORY`: fast, but a power cut
mid-commit corrupted the file. Now: sync on, `journal_mode=PERSIST`,
`synchronous=FULL` (ROBUSTNESS.md case 1); the extra fsyncs are paid
for by the 5 s default batch.

## Footprint

PSRAM: ~250 KB static (param ring 12 KB + frame ring 192 KB at the
8192 cap + writer 10 KB + drain 3 KB stacks + registries + CAN queue)
plus ALL sqlite heap (SQLITE_CONFIG_MALLOC → PSRAM). Internal: 4 KB
DMA-capable stdio buffer PER OPEN append-engine file (lazy heap,
allocated at open, freed at close; must be DMA-capable or sdmmc
bounce-buffer allocs fail under pressure) + FreeRTOS objects. The
writer/drain PSRAM stacks are safe because they only touch the SD
card via SDMMC, never internal flash (§2 corollary doesn't apply).
