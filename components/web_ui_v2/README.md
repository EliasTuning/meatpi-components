# web_ui_v2

The **v2** built-in web app — a self-contained, responsive single-page
application that covers every `/api` + `/ws` feature. Like v6 it is
**API-first**: just another client, no privileged paths.

## Choosing the UI at build time

`Kconfig` adds a choice (menuconfig → *WiCAN built-in web UI*):

| Option | What compiles |
|---|---|
| **v2** (default) | this component — one gzipped `index.html` |
| **v6** | `components/web_ui` — the legacy ES-module pages |
| **None** | headless: `/api` + `/ws` only, no UI blob in flash |

Only the selected UI is embedded; `main` calls both `web_ui_v2_register()`
and `web_ui_register()` but the unselected one is a no-op. API/WS routes
always win because the catch-all is installed last.

Set it with `idf.py menuconfig` or in `sdkconfig.defaults`:
```
CONFIG_WICAN_WEBUI_V2=y     # or CONFIG_WICAN_WEBUI_V6=y / CONFIG_WICAN_WEBUI_NONE=y
```

## Design — "WiCAN Pro" (2026-07-10)

The visual system and information architecture come from **meatpi's
claude.ai/design project "WiCAN Pro configuration interface"**
(`WiCAN Pro.dc.html`, imported via the design MCP) and are implemented
over the existing SPA engine. One file, zero dependencies (no framework,
no CDN — the JetBrains Mono stack falls back to system monospace). It is
gzipped at build time (`gzip_asset.py`) and embedded (~39 KB gz).

- **Shell**: 58 px header — meatpi WiCAN Pro logo · connection pill
  (pulse dot, device id, IP, AP/STA tag) · theme toggle · a header
  **Submit Changes** button that batches settings PUTs + submit (the
  old apply-bar semantics, incl. the unsaved-changes guard).
- **Nav**: DEVICE group (the designed 12 tabs) + TOOLS group (the
  deeper diagnostic pages); a warning card at the bottom surfaces
  degraded / restart-pending components. Wraps to a chip row ≤ 860 px.
- **Connection watchdog**: 3 s `/api/status` poll drives the pill,
  offline/reboot banners, and page re-render on reconnect.
- **Theme**: light-default per the design, full dark theme, follows the
  OS and a manual toggle (persisted).
- **Schema-driven settings**: forms are generated from
  `GET /api/settings/<name>/schema`, grouped into the design's cards —
  new firmware fields appear with no UI change.

## UX audit (2026-09-05) — the rules every page now follows

Screenshot-driven pass over all 23 routes on desktop and a Pixel-class
phone against a live WiCAN Pro (`tools/webui_preview` smoke + probes
green afterwards). What changed, and what to keep doing:

- **Phones render the desktop layout.** The viewport meta is
  `width=1024` (not `device-width`): a phone lays the page out at
  1024 CSS px and zooms to fit — exactly Chrome's "Desktop site" — so
  the sidebar + cards look the same everywhere and nobody has to tick
  that box (meatpi request). Pinch-zoom stays on. The ≤ 860 px
  responsive rules remain for narrow desktop windows only.
- **Field dictionary.** The device schema has no titles/help, so the
  form engine owns the wording: `LABELS` / `HELP` / `ENUMS` /
  `PLACEHOLDER` maps (a `"component.key"` entry beats a bare `key`),
  with `flabel()` as the fallback (sentence case, acronyms upper-cased,
  `_s/_ms/_min/_dbm` → "(s)" etc.). New firmware fields still appear
  automatically — add a dictionary line when the generated label reads
  badly. Enum values are shown as words (`apsta` → "Access point +
  Station") while the stored value stays the raw enum.
- **Controls by type.** Booleans are toggle switches (`switchCtl`),
  never Enable/Disable dropdowns. Bounded integers with a span ≤ 1500
  and every `*_mv` voltage are a slider + number pair (`sliderCtl`;
  volts with 0.05 V steps, stored as mV) — `NO_SLIDER` lists the
  exceptions (ports, "-1 = forever" counters). Secrets come back
  redacted (`""` = keep on PUT), so password inputs carry an
  "unchanged — type to replace" placeholder instead of looking empty.
- **Layout.** `.frow` labels top-align to the control's first line
  (tall controls keep their label at the top); help text sits under the
  control column; number inputs cap at 240 px; two-column card grids
  use `.cols2` (collapses < 700 px).
- **One save model, one button.** The header **Submit Changes** button
  is the only "apply" affordance for staged settings — the per-card
  "Submit to apply" badges were removed (meatpi). Only cards that write
  to the device immediately are marked (`liveBadge`, e.g. Parameters).
- **Sub-tabs on long pages** (`tabbedPage(container, base, tabs, sub)`):
  Settings (WiFi · Bluetooth · CAN bus · Connections · MQTT), Automate
  (Settings · Parameters · Data destinations · Home Assistant), USB
  (Status · Settings · Dongle console), System (Maintenance ·
  Certificates · Firmware update), Trouble Codes (Codes · Code databases
  · Settings), Rules & Events (Rules · Live events · Triggers & actions).
  Every pane is built once; a tab click only toggles visibility and
  rewrites the hash with `history.replaceState` (no `hashchange`, so the
  page is NOT re-run — staged edits and live pollers survive), while
  `#/<page>/<tab>` deep-links land on that tab via the router's `sub`.
- **Sidebar accordion (2026-09-06).** The sub-tabs live in the sidebar:
  a section that has them shows a chevron, and while it is the active
  section its sub-tab list drops under its button (animated
  `grid-template-rows` 0fr→1fr, honours `prefers-reduced-motion`); every
  other section stays collapsed (meatpi: "when I click Settings the
  sub-tabs drop under Settings"). `NAV[].sub` is the ONE list of tab ids
  + labels — `tabbedPage()` reads its labels from there (and warns when
  a page passes a tab id that is missing from it), so adding a tab means
  adding it to `NAV` and passing `{id, el}` from the page. Clicking a
  sub-tab of the page that is already open calls the page's registered
  `show()` (`subNav`) — an in-place pane switch, no route, staged edits
  kept; clicking the open section's own button goes to its first tab the
  same way; anything else routes to `#/<page>/<tab>`. The in-page
  `.subtabs` strip is `display:none` on the sidebar layout and only
  shows under 860 px, where the sidebar becomes a chip row that cannot
  nest. Verified with a Playwright flow on hardware (expand, in-place
  switch with a staged edit kept, guard modal, deep link, narrow window).
- **DOM hygiene.** `Element.append/prepend/replaceChildren` are wrapped
  once to drop `null`/`false` children — conditional children
  (`cond ? el : null`) used to print the word "null" on the page.
- **Automate split by concern (2026-09-06).** The old "Behaviour" tab
  mixed the master switch with per-PID-group settings. Now the
  **Settings** tab (card "Automate settings" — meatpi preferred that
  over "Polling") holds `enabled`, the pause rules and, under advanced,
  the event rate limit / console flag; each PID group's
  own switch, init chain and (standard) protocol head that group's pane
  on the **Parameters** tab, and the vehicle name + init sit above the
  profile picker. Picking a profile in the dropdown IS the load (the
  vehicle-specific PIDs are replaced live, name + init staged for
  Submit); **Fetch latest** re-reads the published list with caches
  bypassed, and the device's current car is preselected on first load
  (meatpi 2026-09-06 — no separate Load Profile button). The pause threshold is one
  "Pause polling" choice — *below the Power Saving sleep voltage* /
  *below a voltage I choose* / *never* — mapped onto the firmware's
  `pause_below_mv` (0 = no fixed threshold) + `pause_follow_sleep`; the
  custom slider runs 12.0–14.5 V in 0.1 V steps (a 12 V vehicle
  battery) instead of the schema's 0–30 V (meatpi). Every autopid form
  on the page shares ONE values object (`av`) so a single staged PUT
  carries all edits. The other voltage sliders keep their firmware
  bounds (8–16 V), which are already sensible.
- **Interactive pass (2026-09-06, meatpi: "click everything, ranges
  must make sense").** A Playwright crawler (`ux_crawl.js`, scratchpad)
  visited all 38 routes/sub-tabs, flipped every switch, drove every
  slider to both ends, cycled every select and clicked every safe
  button: 0 console errors, 0 stray "null" text, every control
  behaved. What changed: the sleep / wake / pause thresholds are
  bounded in the FIRMWARE schema for a 12 V battery (sleep 12.0–14.0 V,
  chip sleep 12.0–14.0 V, chip wake 12.0–15.0 V and forced above sleep
  by `on_validate`, pause ≤ 14.5 V), the sleep delay is 1–30 min, the
  periodic check-in ≥ 5 min and the chip's sleep hold ≤ 60 min — each
  with a schema bump + clamping migration, so stored values never
  degrade a component; all voltage sliders step 0.1 V; the periodic
  wake interval only shows once periodic wake-up is on; the DTC clear
  mode dropdown uses words; the DBC monitor-window and UDS timeout
  boxes carry bounds; IMU sensitivity/threshold rows explain their
  units. Bench note: the Logs page's sink chips toggle sinks LIVE — a
  crawler must not click them (they are in its deny list now).
- **Sleep-off safeguards (meatpi 2026-09-06, the pre-v6 reminder).**
  Sleep mode ships ON with a 5 min delay and a 12.0–14.0 V threshold
  (firmware floor 12.0 V). While sleep is off — on the device, or
  staged in the form — a permanent warning chip sits in the header
  ("Sleep mode off — the vehicle battery can drain", links to Power
  Saving); `paintSleepWarn()` runs from `renderSubmit()`, at boot and
  on every reconnect (`refreshSleep()` reads `/api/sleep`). Submitting
  settings while sleep will be off replaces the plain restart confirm
  with an acknowledgement modal whose **Submit anyway** button stays
  disabled until the "I understand…" box is ticked (`sleepAckModal()`).
  Verified end to end on hardware (stage off → warning; submit → ack →
  reboot → warning persists from device state; re-enable → plain
  confirm → warning gone).
- **Terminal readiness.** A fresh device ships with the `ws_cli`
  channel disabled and no `cli ⇄ ws_cli` bridge, so the WiCAN console
  terminal's Connect was refused silently for every new user. The page
  now reads `websocket_manager` + `bridge_manager`, disables Connect and
  shows a warning with an **Enable WiCAN console** button when the
  selected terminal (console → `/ws/cli`, ELM327 → `/ws/obd`) is not
  bridged; the button stages the channel + `br_cli`/`br_obd` bridge
  (parking any other bridge on that interface — single-consumer rule)
  for the header Submit. A refused socket prints why instead of a bare
  "[disconnected]". Verified end to end on hardware (enable → Submit →
  reboot → Connect → `help`).
- Page fixes from the pass: Settings split into WiFi Access Point ·
  WiFi Network (Station, Scan → click to pick) · Bluetooth cards;
  Connections rows are one line ([iface] ⇄ [conn] + switch); Dashboard
  empty state is a card, not a squeezed tile; Logger Pause is disabled
  while the logger is off; Files never percent-encodes `/` (the fs API
  rejects `%2F`) and offers `/data` + `/sd` at the root; Logs classify
  lines by the IDF level letter; About/pill take the device id from
  `/api/info` (`/api/status` has none); USB page ordered status →
  settings → console.
- Logger page rework (2026-09-06): the status reads the real card
  state (`/api/status` `sdcard_mounted` + `/api/fs/info`) — one chip per
  state (Off / Recording / Paused / Waiting for storage / No SD card)
  and per-stream figures instead of a flat list of dashes; settings
  split into Logger settings · Vehicle parameters · CAN frames (each
  stream discloses progressively, per-format guidance paints live, the
  retention total is checked against the card size, a "nothing
  selected" nudge and a CAN-bus-off warning); the CAN filter is a
  choice (all frames / matching IDs) with 11/29-bit width, sanitised
  hex and the matched ID range spelled out; the newest log files list
  on the page with the write-locked active files marked "in use";
  Files deep-links (`#/files/sd/logs`).
- Dashboard rework (2026-09-06): the header chip shows the real state
  (Automate off / Polling / Paused on battery / Not polling — the old
  page never noticed Automate was off because `/api/autopid` has no
  `enabled`; it reads `/api/status` `autopid_enabled` now); a strip with
  one switch per polling group (runtime `/api/autopid/group`, labelled
  temporary) and chips for PID/filter counts, the measured poll rate and
  failures (+ the sub-floor warning the API asks for); each tile has the
  class icon, a sparkline of the last ~2 minutes, the bar over the
  parameter's min/max from Automate (session autoscale only when unset,
  and it says so), the PID / filter / "External (GPS)" source and the
  value's age — stale values fade. Empty states link to Automate →
  Settings / Parameters. Vehicle-profile import normalises the
  mis-encoded degree sign (`\uFFFD` → `°`).
- Dashboard customisation (2026-09-06): Customise puts a gear on every
  tile — widget (automatic · number · bar · dial · trend · chart), range
  (the parameter's min/max from Automate, or custom), warn below / above
  (amber value and dial arc), decimals, width (1–2 columns), hide — and
  the tiles drag to reorder; the layout is ONE file on the device
  (`/data/dashboard.json`, written through `/api/fs/upload`) so every
  phone and PC sees the same dashboard, and Reset layout deletes it.
  Charts use uPlot 1.6.31 (MIT), NOT embedded: the page downloads the
  pinned build from cdn.jsdelivr.net, checks size + FNV-1a, stores it
  under `/data/cache/www` (or `/sd/cache/www` when flash is short; with
  neither it says an SD card is needed) and loads it from `/cache/www/`
  — the `/cache/*` → `/data/cache` and `/sdcache/*` → `/sd/cache` prefix
  assets `web_ui_v2.c` registers (MIME by extension, ETag/304,
  max-age 3600). Anything under cache/ is re-creatable.
- Dashboard history from the logger (user request 2026-09-06): a chart
  tile backfills its parameter from the data_logger's parameter files in
  ANY of the four formats (window per tile: 15 min / 1 h / 3 h / 6 h),
  then continues live. JSON lines and CSV stream through
  `/api/logger/export?name=<param>` (cursor walked from the newest file
  that starts before the window); binary `.wdl` and SQLite `.db` files
  are fetched whole through `/api/fs/download` (the file being written is
  read after pausing the logger's gate, resumed right after — unpaused it
  answers 409 `file in use`, which the page retries for a few seconds
  because a SQLite commit in flight delays the release) and decoded
  in the browser — the `.wdl` reader mirrors `tools/wdl_dump.py`, SQLite
  uses sql.js 1.14.2 (WebAssembly, 690 KB, MIT) installed on demand under
  `cache/www` exactly like the chart library (`DASH_RES` registry: pinned
  size + FNV-1a, internal flash first, SD card fallback, "insert an SD
  card" when neither has room). Decoded files are cached in memory; record times are shifted by the browser-vs-`/api/rtc` offset so
  an unset device clock still lines up (the note says "device clock not
  set"). Other formats, a logger that is off or `autopid_log=off` get a
  one-line reason with a link to Logger. Samples live in a page-level
  store (`DASH_HIST`, 25k per parameter) that survives navigating away.
  Probe: `probe_dash_history.mjs`.
- WiFi Network card (2026-09-06): while the station is not connected and
  the last attempts failed, a plain-language line says which network,
  why (`/api/wifi/status` `sta_attempt`: reason + failure streak) and what
  the firmware does next (other networks first, then this one again — the
  timed ban is gone), with the hint to correct the password here.
  Probe: `probe_wifi_why.mjs`.
- Size budget (2026-09-06): the build now minifies index.html —
  `gzip_asset.py` runs rjsmin/rcssmin (vendored under `tools/`,
  Apache-2.0, pure Python, so the IDF Python is enough) before gzipping:
  91.6 KB → 82.0 KB gzipped WITH the dashboard customisation and the
  logger history, under the 85.3 KB the UI shipped at before them. `tools/webui_preview/make_preview.py
  --min` builds the preview through the same function so the smoke test
  and probes (`probe_dash_custom.mjs` for the customisation) test what
  ships.
- No OBD chip card (meatpi 2026-09-07): the Advanced page lost the
  `obd_chip` settings card. Its fields (auto sleep, monitor policy, the
  chip's sleep thresholds) only confused users, and the chip UART baud is
  now a firmware constant (`OBD_CHIP_BAUD`; obd_chip settings v3 drops a
  stored value) — a user changing it would break the chip and claim
  warranty. The group stays reachable through the settings API / CLI /
  backup; the UI keeps no labels, enums or formatters for it.
- Motion defaults + Power Saving flag (meatpi 2026-09-07): the IMU stays
  on by default, but "Wake on motion" (the sensor's bump detector) ships
  off with a 125 mg threshold. When it is on, the Power Saving page shows
  a warning banner naming it, linking to Advanced, and stating what it
  really does: bump events for rules/MQTT, no wake from sleep. The
  Motion card hides the threshold while it is off and the SMD knobs while
  SMD is off; the help text no longer claims a knock wakes the device.
  Probe: `probe_power_flag.mjs`.
- Last wake-up on Status (meatpi 2026-09-07): the System card names this
  boot's cause in plain words from `/api/restart/history` — "Battery
  voltage recovered — woke from sleep", "Periodic check-in — woke from
  sleep" (new restart_tracker reason `periodic_wake`; sleep_manager used
  to file check-ins as power_wake), "Restart requested · web UI",
  "Settings applied", "Firmware update", or the chip's own reset cause
  ("Power-on", "Crash (panic) — unexpected", watchdogs, brown-out).
  Motion slots in once wake-on-motion exists. Probe:
  `probe_wake_source.mjs` (the mock's last record is
  `__mockState.lastRestart`).
- File Manager (meatpi 2026-09-07: "improve the UI and UX of the Files
  tab, rename it File Manager"): the page ships as the second on-demand
  chunk (`web/files.js` → `/ui/files.js`, `PAGES.files` stub). Landing =
  storage cards (internal flash / SD card with usage meters, "not mounted"
  when the card is absent); inside a mount: breadcrumb that also drives
  the URL (`#/files/sd/logs` deep links keep working), Up, a usage line,
  filter + sort, multi-select with Delete selected, New folder, multi-file
  Upload with a progress bar (XHR) and drag-and-drop onto the list,
  per-row Preview (text files ≤ 64 KB, JSON pretty-printed), Download,
  Copy path and Delete; the folders users meet carry a one-line
  description; the logger's active file (`/api/logger` dir/file while
  running) is marked "in use" with Delete disabled and a hint to pause
  logging; non-empty folders and in-use files get plain-language errors.
  Bug found on the way: the old page's New folder never worked — it
  posted the path as a JSON body while `/api/fs/mkdir` reads `?path=`
  like every other fs route (bench-verified 2026-09-07).
  Probe: `probe_files.mjs` (the mock lists `__mockState.files` / `dirs`
  dynamically, honours `sdMounted` / `loggerRunning`, and shims
  XMLHttpRequest).
- WiFi settings (meatpi 2026-09-07: "the WiFi mode should not be in the
  AP settings; no open security"): the mode is a tile selector (Access
  point + Station · Station only · Access point only · WiFi off, with a
  warning for off; no tile is labelled recommended, meatpi) above the two cards; each card carries a live chip
  (AP: clients + address; station: connected + IP / not connected / off)
  and folds to a one-line note when the mode does not use it; "Open (no
  password)" is gone from AP security (firmware enum v7 with an
  open → auto migration; the page also filters it out of an older
  schema). Probe: `probe_wifi_mode.mjs`.
- Radio Arbitration + AP auto-off (meatpi 2026-09-07: "these should be in
  wifi settings. And we should recommend to shutdown AP if station is
  connected"): the interface_manager card (Enabled · pause the station while
  a phone is connected over Bluetooth · pause the access point while a phone
  is connected over Bluetooth) left the Advanced page for a fourth card in
  Settings, WiFi, with a note that it only acts while Bluetooth is on.
  `wifi_manager.ap_auto_disable` (drop the AP once the station has an IP,
  back on station loss) left the AP card's advanced fold, is labelled "Turn
  the access point off while the station is connected" with a recommending
  help line, and a banner under the mode tiles ("Recommended: turn the access
  point off while the station is connected", button "Turn it on" = stage
  `ap_auto_disable:true`) shows whenever Access point + Station is selected
  without it. Probe: `probe_wifi_mode.mjs`.
- Factory AP password (meatpi 2026-09-07: "we should not allow submitting
  the password if it is still the default"): while `/api/wifi/status`
  reports `ap_default_password`, the header shows "Factory AP password:
  change it" (links to Settings → WiFi), a warning sits under the AP
  password field, and Submit stops with an explanation whenever the
  staged WiFi settings would keep it (blank = keep, or `@meatpi#` typed).
  The firmware refuses such a write after boot anyway (wifi_manager
  `on_validate`). Probe: `probe_ap_password.mjs` (mock flag
  `__mockState.apDefaultPassword`, PUT counter `__mockState.puts`).
- CAN Monitor (meatpi 2026-09-07: "the monitor tab seems broken: the start
  button is already pressed but nothing shows; bring it as close as possible
  to our mockup"): a fresh WiCAN Pro has the native CAN bus, the `/ws/can`
  channel and the slcan connection all off, so the old page sat "receiving"
  over an empty table. The page is now an on-demand chunk (`web/monitor.js`)
  laid out like the PCAN-style mockup: Connection (bus + WebSocket chips, a
  wiring check that names what is missing and an "Enable CAN monitor"
  button staging `can_manager.enabled` + the `ws_can` channel + a `br_can`
  slcan bridge, Pause/Resume, Reconnect, bit rate and listen-only staged
  into can_manager), Message filter (ID range + data/remote/standard/
  extended switches), Send message (ID, data, extended, remote, cycle,
  Send / Cyclic / Add to list, a persisted transmit list), Trace tools
  (buffer 200/1000/5000, auto-scroll, Clear, Save CSV); the table shows
  Time · Dir · ID · Type · DLC · Data · ASCII (Grouped: bytes with a hot
  highlight, ASCII, cycle, count); the status bar shows RX · TX · Errors
  (from `/api/can` bus_errors, TEC/REC etc. in the tooltip) · Rate · Bus
  load · Connected/Paused · Buffer · Auto-scroll. SUPERSEDED the same day
  by the design below.
- CAN Monitor, second pass (Ali, 2026-09-07: "i found the design i wanted,
  implement this", the Claude Design export "WiCAN PRO Monitor"): the page
  became the design's three-tab PCAN-View style analyzer (see the Pages
  list): receive list with decode panel, transmit list with context menu and
  keyboard shortcuts, edit dialog with byte boxes, trace, settings cards,
  status bar, connect dialog; its own light/dark palette from the design
  (`.pmv` tokens, prefixed `--p*` so the app's tokens stay untouched);
  shipped transmit rows are manual (nothing goes on a vehicle bus by
  itself); the page OPENS DISCONNECTED (Ali: "it should be disconnected
  by default": no socket until Connect… in the page header is confirmed,
  then the stream runs and reconnects until Disconnect); CAN FD
  controls render disabled (the TWAI controller has no FD);
  prefs in localStorage `wican.canmon.v1`. Probe: `probe_monitor.mjs` (mock
  `__mockState.canEnabled`, `wsCanPeriod`, `wsSent`, `wsCanRefuse`,
  `dbcs`, `dbcSignals`).
- Logger robustness surfaces (2026-09-07, data_logger/ROBUSTNESS.md): the
  Logger status shows a warning banner while `*.corrupt` files are set
  aside (what happened, `tools/db_recover.py`, link to the File Manager)
  and a chip "N records recovered after the last reset"; the File Manager
  labels `.corrupt` files; the flush-interval help explains the trade-off.
  Mock: `__mockState.loggerCorrupt` / `loggerSalvaged`.


### PID lists on Automate > Parameters (2026-09-16)

The three PID tables (Standard / Vehicle Specific / Custom) were a fixed-width spreadsheet of inline inputs: a hard 890-960 px grid that clipped the delete button even at 1440 px and hid half the columns behind an unsignposted scroll at the 1024 px phone layout, parameter sub-rows with no headers that did not line up with the PID columns, every single-value PID shown twice, group/init/cycle repeated on every row, thirteen primary Test buttons, and no live values. `pidTable()` now renders a list: one row per PID = enable switch (the firmware's `enabled`, default true), name + request (+ RX ID on std/custom), the live value(s) the poller holds, a ghost Test and delete (the Vehicle Specific list has the Your car picker right under its enable switch, ahead of the name + init it fills). Expanding a row (the `▸ n parameters` caret, `Expand all` / `Collapse all` in the toolbar) shows the details panel: group / cycle / PID init, then the parameter table WITH headers: per-parameter switch, name, expression (red border while empty), unit, live value, delete, `Add parameter`. Live values: `GET /api/autopid` every 2.5 s while the page is open, written into the cells in place (`liveCell()`), so typing is never interrupted; the toolbar counts `N PIDs · M parameters`, filters by name / request / parameter name, and shows an `Unsaved edits` chip until Apply; a rejected Apply prints the firmware's reason under the list as well as in the toast. Layout is CSS grid with `--pidcols` per list (no min-width), flex-wrapping under 860 px; phones at 1024 CSS px fit without horizontal scroll (verified with Playwright against the bench DUT at 1440 and 1024). Kept on purpose: std names/commands read-only, the live-apply model (Apply Configuration + the Submit-staged vehicle name/init), the `▸` caret text the probes key on. `probe_automate.mjs` covers the rows; the profile import is bench-proven by `tools/testbench/obd/autopid_profile_bench.py`.

## On-demand page chunks (2026-09-07)

The main page must not grow (meatpi: "we cannot increase the size of the
web UI"), so a heavy, rarely opened page can ship as its own chunk:
`web/scripts.js` is minified (rjsmin) and gzipped by `gzip_asset.py` at
build time, embedded next to `index.html.gz` and served at
`/ui/scripts.js` (`web_ui_v2.c` asset table, `application/javascript`,
`Content-Encoding: gzip`). `index.html` keeps a four-line stub
(`PAGES.scripts`) that `loadScript()`s the chunk the first time the page
opens and then calls `PAGES.__scripts`; the chunk is a classic script, so
it shares the page's global scope (`h`, `page`, `tabbedPage`,
`settingsForm`, `api`, `UI_RES`, …), injects its own CSS and registers its
library in `UI_RES`. Sizes at the split: index.html.gz 84.6 KB (84.4 KB
before the Scripts work; the stub, nav entry, icons and dictionary lines
are the difference), scripts.js.gz 8.5 KB. `make_preview.py` inlines every
chunk after the app script so the jsdom probes run it (the stub sees
`PAGES.__scripts` already defined and never fetches). To add another
chunk: a `web/<name>.js` that sets `PAGES.__<name>`, a stub in
`index.html`, one more custom command + embed in `CMakeLists.txt`, one
more asset row in `web_ui_v2.c`, and its name in `make_preview.py`'s
chunk list. Chunks today: `scripts.js` (Scripts), `files.js` (File
Manager) and `monitor.js` (CAN Monitor, 2026-09-07).

## Pages

DEVICE: Status (stat cards + network/system tables) · Settings (WiFi/AP
· Station & Bluetooth w/ scan · CAN · MQTT) · Automate (autopid
polling switch + pause rules · PID groups with their own
switch/init/protocol · Home Assistant webhook · destinations-as-event-rules · PID
table/scan/vehicle-profile/raw-config) · Power Saving · Logger (per-stream status · gate ·
newest files · settings split per stream) · Dashboard (state chips · group switches + poll stats · sparkline
tiles over the parameter's own min/max, with the PID and the value's age ·
Customise: per-tile widget / range / warnings / width / hide, drag order,
layout file on the device, uPlot charts cached under cache/www, chart history
backfilled from the logger's JSON-lines stream) ·
**CAN Monitor** (2026-09-07, built after Ali's Claude Design "WiCAN PRO
Monitor": sub-tabs Monitor · Trace · Settings; Monitor = ID filter, Pause,
Clear, a RECEIVE list grouped by CAN-ID (type chips STD/EXT/RTR, DLC, bytes
with the changed ones lit, ASCII, cycle, count; sortable, drag-resizable
columns; click a row for the decode panel: DBC signals from the device's
loaded .dbc files decoded with the firmware's bit rules, else the raw
frame) and a TRANSMIT list (on/off, ID, type, DLC, data, cycle, count,
trigger on an RX ID, comment; Send/edit/delete; context menu with
cut/copy/paste/clear + CAN ID and data byte formats; Space/Insert/Enter/
Delete/Ctrl+X/C/V/Shift+Esc); Trace = newest-first buffer with record/
stop, size, CSV, bus errors as error rows; Settings = DEVICE, CAN
INTERFACE (bit rate, mode, the wiring check + one-click enable; CAN FD
shown as unavailable), DECODING (DBC files, upload), SETTINGS FILE
(save/load JSON); status bar connected · bus · load · rx/s · frames · err ·
TEC/REC; page-header connection chip + Connect/Disconnect. Speaks **slcan
over `/ws/can`**; chunk `web/monitor.js`) · Terminal (console `/ws/cli` or
ELM327 `/ws/obd`) · Advanced (IMU only: radio arbitration moved to Settings,
WiFi on 2026-09-07 — no OBD chip card
since 2026-09-07)
· System (reboot · backup/restore · restart history · factory reset ·
**Certificates** (cert_manager sets: list w/ part flags, per-part PEM
upload via raw `/api/certs/upload?set&type` — NOT the api() JSON
wrapper — delete w/ confirm; key material is write-only by design; the
MQTT and Home Assistant `cert_set` fields become dropdowns fed from
`/api/certs` via `certSetPicker()`, which MUST share the settingsForm's
staged values object) · OTA with progress) · VPN (settings + keygen +
debug) · **USB** (connector role/uplink status · **ESPNetLink Status**
= legacy-usb_host-tab parity: LTE + GPS panels + full-JSON `<details>`,
polled from the dongle's own `lte -j` / `gps -p -j` console over
`/api/usb/acm/cmd` at the legacy cadences [GPS 5 s, LTE 15 s]; falls
back to the cached AGNSS position when there's no live fix · ESPNetLink
Console for manual commands, serialized against the poll via a shared
`nlBusy` latch so they never 409 each other) · About.
TOOLS: Trouble Codes (scan/describe/clear + databases) · DBC Signals ·
Rules & Events · **Scripts** (2026-09-07: Editor · Examples · Reference ·
Settings — the Editor tab is a full-width "Stored scripts" table (name,
size, Open · Run · Download · Delete per row, the open one highlighted;
meatpi: a stacked side list was not intuitive) above an editor card whose
header names the open file and carries the Saved / Unsaved / Running
chip; scripts are files under `/data/scripts` created, opened, saved,
renamed, run, checked, downloaded and deleted in the browser; Run sends the
editor's text inline (saves and runs by name past the 8 KB inline cap),
Check compiles only; output lines that name `string:<line>:` are
jump-to-line links and a matching hint from the firmware's `errors` table
shows under the output; CodeMirror 5 (highlighting via a simple-mode
Berry grammar, line numbers, bracket matching, autocomplete of the
bindings + globals + keywords, Ctrl-S / Ctrl-Enter / Ctrl-Space /
Ctrl-/) is an on-demand library in the same `UI_RES` registry as uPlot
and sql.js — nine files under `cache/www`, a plain textarea until it is
installed; the Examples gallery and the whole Reference (bindings with
Insert, globals, a Berry primer, the rule recipe, error hints, limits)
are rendered from `GET /api/scripts/reference` + `/examples`, so the page
never drifts from the bindings; CodeMirror runs with its own `wican`
theme so the tokens take the page's colour variables in both light and
dark mode (its stock palette is light-only); the scripting settings
(enable switch, runtime budget, ECU-flashing gate) have their own tab and
go through the header Submit like every setting) · UDS Tool · J2534 ·
**File Manager** (2026-09-07: storage cards, breadcrumb + deep links,
filter/sort, multi-select delete, drag-and-drop upload with progress, text
preview, in-use marking; chunk `web/files.js`) · Logs · System Monitor
(tasks/CPU/heap/temp) · All Settings
(every component).

## Connections card (Settings page — the bridge builder)

Pure settings composition, ZERO firmware surface: each row is
[interface ▾] ⇄ [connection ▾] with contextual fields (TCP/UDP → port,
WebSocket → path, CAN → protocol raw/slcan/gvret/realdash) + enabled +
Add (max 4). Compiles into `bridge_manager.bridges` AND maintains the
backing `socket_manager.servers` (reuse by proto+port, else allocate a
free/disabled slot, name `tcp<port>`) / `websocket_manager.channels`
(reuse by path, name `ws_<path>`). Client-side guards: single-consumer rule, port and
`/ws/` path validation, slot caps, "CAN bus disabled" hint. Bridges it
can't model (hand-made relay pairings) show read-only with a pointer to
All Settings. Everything applies through the header Submit
(reboot-to-apply).

## Preview + tests WITHOUT a device — `tools/webui_preview/` (2026-07-13)

The committed harness mocks the whole `/api` + `/ws` surface in-page:

```
cd tools/webui_preview
python make_preview.py     # extracts all 34 components' settings field
                           # tables from the C sources into mocks, and
                           # injects mock_api.js into index.html
start preview.html         # clickable in any browser, zero device
npm i jsdom                # once
node smoke.mjs             # every route, assert 0 JS errors + non-empty view
node probe_automate.mjs    # interaction probe: Automate + Rules editor flows
node probe_adv.mjs         # advanced-gating visibility probe
```

Last run (2026-07-13): smoke 23 routes 0 errors; automate probe 16/16
(card order, essentials-only gating, scan→results-modal→dedup-merge,
rule-editor modal). The preview is also publishable as a claude.ai
artifact for review. Against a REAL device instead:
`cd components/web_ui_v2/web && python -m http.server 8000` then
`http://localhost:8000/?api=http://<dut-ip>`.
