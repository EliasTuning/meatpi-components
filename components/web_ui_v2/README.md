# web_ui_v2

The **v2** built-in web app: a self-contained, responsive single-page
application that covers every `/api` + `/ws` feature. Like v6 it is
**API-first**: just another client, no privileged paths.

## Choosing the UI at build time

`Kconfig` adds a choice (menuconfig → *WiCAN built-in web UI*):

| Option | What compiles |
|---|---|
| **v2** (default) | this component: one gzipped `index.html` |
| **v6** | `components/web_ui`: the legacy ES-module pages |
| **None** | headless: `/api` + `/ws` only, no UI blob in flash |

Only the selected UI is embedded; `main` calls both `web_ui_v2_register()`
and `web_ui_register()` but the unselected one is a no-op. API/WS routes
always win because the catch-all is installed last.

Set it with `idf.py menuconfig` or in `sdkconfig.defaults`:
```
CONFIG_WICAN_WEBUI_V2=y     # or CONFIG_WICAN_WEBUI_V6=y / CONFIG_WICAN_WEBUI_NONE=y
```

## Design: "WiCAN Pro" (2026-07-10)

The visual system and information architecture come from **meatpi's
claude.ai/design project "WiCAN Pro configuration interface"**
(`WiCAN Pro.dc.html`, imported via the design MCP) and are implemented
over the existing SPA engine. One file, zero dependencies (no framework,
no CDN: the JetBrains Mono stack falls back to system monospace). It is
gzipped at build time (`gzip_asset.py`) and embedded (~39 KB gz).

- **Shell**: 58 px header, meatpi WiCAN Pro logo · connection pill
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
  `GET /api/settings/<name>/schema`, grouped into the design's cards,
  new firmware fields appear with no UI change.

## UX audit (2026-09-05): the rules every page now follows

Screenshot-driven pass over all 23 routes on desktop and a Pixel-class
phone against a live WiCAN Pro (`tools/webui_preview` smoke + probes
green afterwards). What changed, and what to keep doing:

- **Phones render the desktop layout.** The viewport meta is
  `width=1024` (not `device-width`): a phone lays the page out at
  1024 CSS px and zooms to fit (exactly Chrome's "Desktop site") so
  the sidebar + cards look the same everywhere and nobody has to tick
  that box (meatpi request). Pinch-zoom stays on. The ≤ 860 px
  responsive rules remain for narrow desktop windows only.
- **Field dictionary.** The device schema has no titles/help, so the
  form engine owns the wording: `LABELS` / `HELP` / `ENUMS` /
  `PLACEHOLDER` maps (a `"component.key"` entry beats a bare `key`),
  with `flabel()` as the fallback (sentence case, acronyms upper-cased,
  `_s/_ms/_min/_dbm` → "(s)" etc.). New firmware fields still appear
  automatically: add a dictionary line when the generated label reads
  badly. Enum values are shown as words (`apsta` → "Access point +
  Station") while the stored value stays the raw enum.
- **Controls by type.** Booleans are toggle switches (`switchCtl`),
  never Enable/Disable dropdowns. Bounded integers with a span ≤ 1500
  and every `*_mv` voltage are a slider + number pair (`sliderCtl`;
  volts with 0.05 V steps, stored as mV): `NO_SLIDER` lists the
  exceptions (ports, "-1 = forever" counters). Secrets come back
  redacted (`""` = keep on PUT), so password inputs carry an
  "unchanged: type to replace" placeholder instead of looking empty.
- **Layout.** `.frow` labels top-align to the control's first line
  (tall controls keep their label at the top); help text sits under the
  control column; number inputs cap at 240 px; two-column card grids
  use `.cols2` (collapses < 700 px).
- **One save model, one button.** The header **Submit Changes** button
  is the only "apply" affordance for staged settings: the per-card
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
  page is NOT re-run, staged edits and live pollers survive), while
  `#/<page>/<tab>` deep-links land on that tab via the router's `sub`.
- **Sidebar accordion (2026-09-06).** The sub-tabs live in the sidebar:
  a section that has them shows a chevron, and while it is the active
  section its sub-tab list drops under its button (animated
  `grid-template-rows` 0fr→1fr, honours `prefers-reduced-motion`); every
  other section stays collapsed (meatpi: "when I click Settings the
  sub-tabs drop under Settings"). `NAV[].sub` is the ONE list of tab ids
  + labels: `tabbedPage()` reads its labels from there (and warns when
  a page passes a tab id that is missing from it), so adding a tab means
  adding it to `NAV` and passing `{id, el}` from the page. Clicking a
  sub-tab of the page that is already open calls the page's registered
  `show()` (`subNav`): an in-place pane switch, no route, staged edits
  kept; clicking the open section's own button goes to its first tab the
  same way; anything else routes to `#/<page>/<tab>`. The in-page
  `.subtabs` strip is `display:none` on the sidebar layout and only
  shows under 860 px, where the sidebar becomes a chip row that cannot
  nest. Verified with a Playwright flow on hardware (expand, in-place
  switch with a staged edit kept, guard modal, deep link, narrow window).
- **DOM hygiene.** `Element.append/prepend/replaceChildren` are wrapped
- **Rules & Events: the rule builder (2026-09-17, the Rules Builder
  canvas).** The rules list shows every rule as a sentence ("When WiFi
  connects to HomeAP → poll the default group every 10 s, back to the
  configured rate when that stops being true") with the technical form
  (name · event → action · undo · cooldown) underneath, a switch, the
  runtime badge from `GET /api/events/rules` (fired N×, last fired, active;
  refreshed every 5 s while the pane is open), edit / duplicate / delete.
  Add Rule (and the Templates menu: slow polling at home, extra PIDs while
  charging, alert on a value, LED on a new DTC) opens the builder: Name,
  **When** (a plain-language trigger picker over `/api/events/sources`
  with its own fields: saved networks for `wifi.sta`, parameter names from
  the autopid config for `autopid.param`, status bits, MQTT topic, a
  timer period that creates the timer named after the rule), **Only if**
  (up to 4 rows of field / operator / value; fields are the trigger's own
  keys plus every live value the registry offers, `autopid.` expanded to
  the parameter names and marked LIVE; stored as `when[].key` or
  `when[].value:"${...}"`), **Then** (an action picker over
  `/api/events/actions` with friendly forms: group + rate for
  `autopid.group`, colour + mode for the LED, topic/payload with the
  template hint; registered actions the vocabulary does not know get a
  generic form from their `params_schema`), the **Undo** switch (only for
  `undoable` actions: a while-rule that reverses when the conditions stop
  holding), a preview sentence, and **Edit as JSON** (round-trips with the
  form, unknown keys kept). Trigger state fields (`connected`, `edge`,
  `state`, `set`) are written into `when`, identities (`param`, `pid`,
  `topic`, `bit`, `timer`) into `match`, so an undo rule sees the opposite
  event. Add/Save validates (name `[A-Za-z0-9_]{1,15}`, unique, ≤ 4
  conditions, ≤ 4 timers) before the modal closes and stages the
  `event_manager` settings for Submit. `probe_rules.mjs` covers it.
  once to drop `null`/`false` children: conditional children
  (`cond ? el : null`) used to print the word "null" on the page.
- **Automate split by concern (2026-09-06).** The old "Behaviour" tab
  mixed the master switch with per-PID-group settings. Now the
  **Settings** tab (card "Automate settings": meatpi preferred that
  over "Polling") holds `enabled`, the pause rules and, under advanced,
  the event rate limit / console flag; each PID group's
  own switch, init chain and (standard) protocol head that group's pane
  on the **Parameters** tab. The Vehicle Specific pane heads with ONE
  **Vehicle profile** row (the `vehicle` setting, still editable) plus a
  **Choose profile** button (meatpi 2026-09-17, replacing the "Your car"
  dropdown that PUT the config the moment it changed): a dialog over the
  latest published `vehicle_profiles.json` (fetched when it opens,
  **Fetch latest** re-reads it) with the makes on the left and the chosen
  make's models on the right ("Option A", same day; names split at the
  first colon of the `Make: Model` convention, first word otherwise; no
  PID/parameter counts, Ali), one search over both that auto-selects a
  single match, Up/Down through the models, Left/Right through the
  makes, Enter / Esc, and the device's current car preselected)
  whose "Use this profile" only STAGES the pick on the page: the
  vehicle-specific PIDs are replaced in the table (`Unsaved edits` chip
  + an info banner with **Discard**), name + init + enable are staged
  for Submit. A profile's passive CAN `filters` (the Leaf ZE1 profile,
  2026-10-21) join the table's filters tagged `profile: true`: the next
  import replaces them, hand-made filters stay, Discard puts the previous
  ones back, the banner and the toast count them (ported into this file
  2026-10-27: the edit had sat in a stale clone, so builds up to
  6.0.0-beta.4 imported a profile's PIDs alone). Apply Configuration
  sends the PIDs AND saves the card's
  staged autopid settings (`PUT /api/settings/autopid`), then offers the
  restart they need (settings are reboot-to-apply, standard 4.2) in one
  modal: *Save, restart later* / *Save and restart now* (the sleep-off
  acknowledgement rides in it; dismissing saves nothing and the pane says
  so). A refresh after Apply keeps the fields (it used to empty them:
  they were staged for Submit only, Ali 2026-09-17); "later" leaves the
  sidebar's Restart pending notice plus a warn banner on the Parameters
  card with **Restart now**. Nothing reaches the device on its own.
  Names longer than the schema's 63 chars
  are cut to fit. Same day: the card's autopid settings/schema load
  retries once and a failure shows a crit banner with **Retry** on the
  Parameters tab (it used to drop every group row silently with the
  error on the hidden Settings tab; reproduced by aborting one request
  with Playwright). The pause threshold is one
  "Pause polling" choice (*below the Power Saving sleep voltage* /
  *below a voltage I choose* / *never*) mapped onto the firmware's
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
  periodic check-in ≥ 5 min and the chip's sleep hold ≤ 60 min: each
  with a schema bump + clamping migration, so stored values never
  degrade a component; all voltage sliders step 0.1 V; the periodic
  wake interval only shows once periodic wake-up is on; the DTC clear
  mode dropdown uses words; the DBC monitor-window and UDS timeout
  boxes carry bounds; IMU sensitivity/threshold rows explain their
  units. Bench note: the Logs page's sink chips toggle sinks LIVE, a
  crawler must not click them (they are in its deny list now).
- **Sleep-off safeguards (meatpi 2026-09-06, the pre-v6 reminder).**
  Sleep mode ships ON with a 5 min delay and a 12.0–14.0 V threshold
  (firmware floor 12.0 V). While sleep is off (on the device, or
  staged in the form) a permanent warning chip sits in the header
  ("Sleep mode off: the vehicle battery can drain", links to Power
  Saving); `paintSleepWarn()` runs from `renderSubmit()`, at boot and
  on every reconnect (`refreshSleep()` reads `/api/sleep`). Submitting
  settings while sleep will be off replaces the plain restart confirm
  with an acknowledgement modal whose **Submit anyway** button stays
  disabled until the "I understand…" box is ticked (`sleepAckModal()`).
  Verified end to end on hardware (stage off → warning; submit → ack →
  reboot → warning persists from device state; re-enable → plain
  confirm → warning gone).
- **Sleep countdown bar (meatpi 2026-10-06).** A default device on an
  11.6 V supply went to sleep after 2 minutes in the middle of a Quick
  Setup, with "Sleep after 5 min" on the page: the critical battery
  floor (under 11.9 V for 2 min then; 5 min since the same day, Ali)
  cuts a longer sleep delay short, and nothing on the page said a sleep
  was coming at all. `/api/sleep` now
  carries `pending` (`delay` = the sleep delay runs, `critical` = the
  floor runs) and `sleep_in_s`; `#sleepbar`, the first child of
  `main.content` (sticky, so it stays while a page scrolls, and it is
  there in Quick Setup's focused layout), counts it down on every page:
  "Going to sleep in 1:27. The battery reads 11.6 V, under the 11.9 V
  critical floor: ... It wakes above 13.2 V." for the floor (warning
  kind), "... below the sleep voltage (13.1 V)." for the delay (plain
  kind), with a link to Power Saving. `refreshSleep()` stores the read
  in `sleepSoon`, `paintSleepSoon()` ticks once a second between reads
  and on every connection change, and `ping()` reads `/api/sleep` again
  only while the status bit `wake_voltage_ok` is false or a countdown
  shows (no extra request on a healthy battery). When the device drops
  off within 8 s of the countdown's end the offline pill says "WiCAN
  went to sleep (battery under 11.9 V). It wakes above 13.2 V." instead
  of "reconnecting" (`sleepSoonGone()`); an earlier drop-off keeps the
  plain text. A reading that would print the same one decimal as the
  threshold beside it shows two (`voltVs()`: "13.08 V, below the sleep
  voltage (13.1 V)"). A firmware without `pending` shows no bar. Power
  Saving names the floor as a rule of its own (it was a clause of the
  sleep-off warning; the voltage and the 5 minutes come from
  `critical_v` / `critical_s` of the route) and the Sleep after help
  says the floor is the latest it sleeps. Cost: 0.9 KB gzipped. Probe: `probe_sleep_soon.mjs` (23 checks;
  the mock counts `__mockState.sleepPending = {cause, in_s}` down and
  stops answering at its end, `asleep = false` wakes it).
- **Keep awake (Ali, 2026-10-06: "if the user is configuring the device
  and it is about to sleep, prompt the user to extend; available after the
  device is configured too").** The countdown bar carries a `Keep awake
  10 min` button (`.sb-hold`) while a countdown runs and the device still
  allows a hold (`holds_left` on `/api/sleep`); a press posts
  `/api/sleep/hold {"minutes":10}` (`holdSleep()`), applies the answer and
  toasts. One minute before the entry `promptSleepHold()` opens the page's
  own dialog once per deadline, on a visible tab only and never over
  another dialog: the rule about to fire, "you are in the middle of Quick
  Setup" when the wizard is open, the holds left, `Let it sleep` (closes,
  nothing else) and `Keep awake 10 minutes`; its title counts the seconds.
  "Once per deadline": `sleepSoon.prompted` keeps the deadline asked about,
  a deadline within 30 s of it is the same one (a read moves it by a second
  or so), and a deadline that moves out by more than 30 s (a hold, from here
  or from the console) resets it. After a press the bar reads "Kept awake on
  your request (2 of 3 holds left)" and the button "10 min more"; with the
  three used the button is gone and the bar says so, and the page does not
  ask. The dialog closes by itself when the device drops off or the
  countdown clears. Power Saving explains the rule in one note. Probe:
  `probe_sleep_soon.mjs` (41 checks: the button, a press, the dialog at
  0:58, Let it sleep, the next deadline asked again, the third press and
  the cap, a hidden tab not asked and asked once visible; the mock's
  `/api/sleep/hold` counts `__mockState.holds`).
- **Terminal readiness.** A fresh device ships with the `ws_cli`
  channel disabled and no `cli ⇄ ws_cli` bridge, so the WiCAN console
  terminal's Connect was refused silently for every new user. The page
  now reads `websocket_manager` + `bridge_manager`, disables Connect and
  shows a warning with an **Enable WiCAN console** button when the
  selected terminal (console → `/ws/cli`, ELM327 → `/ws/obd`) is not
  bridged; the button stages the channel + `br_cli`/`br_obd` bridge
  (parking any other bridge on that interface: single-consumer rule)
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
- **USB GPS receiver (issue #978, 2026-10-11).** A plain receiver on the
  connector (u-blox `1546`, or anything that speaks NMEA) gets a "GPS
  receiver" card on USB > Status (the device, the sentences read, the fix
  rows, where the fix goes; the off state points at the switch), the
  connector card's last row reads "USB serial" (receiver / dongle console /
  not bound), and the LTE & GPS card is shown for the dongle only. The
  component's two switches sit on two tabs from ONE shared values object
  (`settingsForm(..., {values, schema})`): `gps` ("USB GPS receiver", on by
  default) in its own card on the Settings tab, `console` ("ESPNetLink
  console over USB") on the Dongle console tab. The Status page's System
  card gets a GPS row while a fix exists or a receiver is being read
  (`gpsRow()`). Quick Setup's USB step reads the receiver live and stages
  nothing. Probe: `tools/webui_preview/probe_usb_gps.mjs`.
- Logger page rework (2026-09-06): the status reads the real card
  state (`/api/status` `sdcard_mounted` + `/api/fs/info`): one chip per
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
  (Automate off / Polling / Paused on battery / Not polling, the old
  page never noticed Automate was off because `/api/autopid` has no
  `enabled`; it reads `/api/status` `autopid_enabled` now); a strip with
  one switch per polling group (runtime `/api/autopid/group`, labelled
  temporary) and chips for PID/filter counts, the measured poll rate and
  failures (+ the sub-floor warning the API asks for); each tile has the
  class icon, a sparkline of the last ~2 minutes, the bar over the
  parameter's min/max from Automate (session autoscale only when unset,
  and it says so), the PID / filter / "External (GPS)" source and the
  value's age: stale values fade. Empty states link to Automate →
  Settings / Parameters. Vehicle-profile import normalises the
  mis-encoded degree sign (`\uFFFD` → `°`).
- Dashboard customisation (2026-09-06): Customise puts a gear on every
  tile, widget (automatic · number · bar · dial · trend · chart), range
  (the parameter's min/max from Automate, or custom), warn below / above
  (amber value and dial arc), decimals, width (1–2 columns), hide, and
  the tiles drag to reorder; the layout is ONE file on the device
  (`/data/dashboard.json`, written through `/api/fs/upload`) so every
  phone and PC sees the same dashboard, and Reset layout deletes it.
  Charts use uPlot 1.6.31 (MIT), NOT embedded: the page downloads the
  pinned build from cdn.jsdelivr.net, checks size + FNV-1a, stores it
  under `/data/cache/www` (or `/sd/cache/www` when flash is short; with
  neither it says an SD card is needed) and loads it from `/cache/www/`:
  the `/cache/*` → `/data/cache` and `/sdcache/*` → `/sd/cache` prefix
  assets `web_ui_v2.c` registers (MIME by extension, ETag/304,
  max-age 3600). Anything under cache/ is re-creatable.
- Dashboard history from the logger (user request 2026-09-06): a chart
  tile backfills its parameter from the data_logger's parameter files in
  ANY of the four formats (window per tile: 15 min / 1 h / 3 h / 6 h),
  then continues live. JSON lines and CSV stream through
  `/api/logger/export?name=<param>` (cursor walked from the newest file
  that starts before the window); binary `.wdl` and SQLite `.db` files
  are fetched whole through `/api/fs/download` (the file being written is
  read after pausing the logger's gate, resumed right after, unpaused it
  answers 409 `file in use`, which the page retries for a few seconds
  because a SQLite commit in flight delays the release) and decoded
  in the browser: the `.wdl` reader mirrors `tools/wdl_dump.py`, SQLite
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
  the firmware does next (other networks first, then this one again, the
  timed ban is gone), with the hint to correct the password here.
  Probe: `probe_wifi_why.mjs`.
- Size budget (2026-09-06): the build now minifies index.html,
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
  stored value): a user changing it would break the chip and claim
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
  boot's cause in plain words from `/api/restart/history`: "Battery
  voltage recovered, woke from sleep", "Periodic check-in, woke from
  sleep" (new restart_tracker reason `periodic_wake`; sleep_manager used
  to file check-ins as power_wake), "Restart requested · web UI",
  "Settings applied", "Firmware update", or the chip's own reset cause
  ("Power-on", "Crash (panic), unexpected", watchdogs, brown-out).
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
  Bug found on the way: the old page's New folder never worked, it
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

The three PID tables (Standard / Vehicle Specific / Custom) were a fixed-width spreadsheet of inline inputs: a hard 890-960 px grid that clipped the delete button even at 1440 px and hid half the columns behind an unsignposted scroll at the 1024 px phone layout, parameter sub-rows with no headers that did not line up with the PID columns, every single-value PID shown twice, group/init/cycle repeated on every row, thirteen primary Test buttons, and no live values. `pidTable()` now renders a list: one row per PID = enable switch (the firmware's `enabled`, default true), name + request (+ RX ID on std/custom), the live value(s) the poller holds, a ghost Test and delete (the Vehicle Specific list heads with its enable switch, the Vehicle profile row with the Choose profile dialog, and the init chain). Expanding a row (the `▸ n parameters` caret, `Expand all` / `Collapse all` in the toolbar) shows the details panel: group / cycle / PID init (the cycle field is empty with the group's rate as its placeholder while the PID inherits it, `period_ms` 0, so an imported profile never reads as "cycle 0"; 2026-09-17), then the parameter table WITH headers: per-parameter switch, name, expression (red border while empty), unit, live value, delete, `Add parameter`. Live values: `GET /api/autopid` every 2.5 s while the page is open, written into the cells in place (`liveCell()`), so typing is never interrupted; the toolbar counts `N PIDs · M parameters`, filters by name / request / parameter name, and shows an `Unsaved edits` chip until Apply; a rejected Apply prints the firmware's reason under the list as well as in the toast. Layout is CSS grid with `--pidcols` per list (no min-width), flex-wrapping under 860 px; phones at 1024 CSS px fit without horizontal scroll (verified with Playwright against the bench DUT at 1440 and 1024). Kept on purpose: std names/commands read-only, the live-apply model (Apply Configuration + the Submit-staged vehicle name/init), the `▸` caret text the probes key on. The Test button fires ONE shot per PID (`type` + `expressions[]`, no longer one request per parameter) and the modal shows the firmware's transcript (`> sent` / `< received` for the type init, PID init, ATCRA, request, ATCRA off), the payload and every decoded parameter (meatpi 2026-09-16: "show what is being sent and what is received"). `probe_automate.mjs` covers the rows; the profile import is bench-proven by `tools/testbench/obd/autopid_profile_bench.py`.

### CAN link states and the bus guard (2026-10-03)

The native CAN node listens before it talks and `/api/can` says where it is,
so three places changed wording (no layout change): the Status tile
(`canRateText` / `canStateText` in index.html: `Auto` until a bit rate is
proven, `Listening for the bit rate`, `Listening · no traffic yet`, `Bus runs
at another bit rate · listen-only` in the warn colour, then `Normal · active`
/ `Listen-only · active` from `listen_only`, not from the `silent` setting),
the CAN Monitor (`canRate` / `canNote` / `canWhyNoTx` in monitor.js: the bit
rate select gains `Automatic`, the status bar and Device card show the link
state, Send explains WHY nothing goes out) and the `can_manager.baud` field
(label `Bit rate`, `Automatic (from the bus traffic)`, a help line).
Automate: when the firmware's bus guard parks the poller
(`stats.paused_bus`) the dashboard chip reads `Paused: nothing is sent to
this bus` and a warn banner above the tiles prints the firmware's own
sentence (`bus_guard.reason`) with a link to Automate settings; the chip
jobs' HTTP 409 carries the same sentence, which `api()` already shows as
the error. The `std_protocol` help says what Automatic does and that a fixed
protocol pauses instead of transmitting onto another bit rate.

### J1939 vehicles on the pages (2026-10-03, TASK_j1939_wwh.md phase 5)

- **Quick Setup, vehicle step** (`setup.js`): a fourth detection phase,
  `network` ("Listening to the vehicle network"); a J1939-only result reads
  "J1939 vehicle" with the network row (`SAE J1939, 250 kbit/s`, chip
  "Heard"), "identified by its controllers" without a VIN, no profile step
  (profiles carry OBD requests), Continue enabled without a profile choice;
  a result with `j1939_listening:false` announces "One more restart for the
  J1939 listener" and the Reading-the-car Finish stages `can_manager`
  (`enabled`, `silent`, `baud` = the measured bitrate) and `j1939`
  (`enabled`) beside `autopid` in the one restart; an EU truck (OBD dialect +
  `j1939`) gets a "J1939 network as well" note and chip. The store list and
  the Done screen say "SAE J1939".
- **Automate > Parameters** (`index.html`): a row whose command is `PGN:…`
  shows a "J1939" tag where the RX ID would be, no init field (the expanded
  row explains the group instead), Test decodes it from the listener's store;
  "Add PGN" on the Custom tab adds `PGN:F004` / EngineSpeed; a J1939 listener
  line over the standard table (state, bitrate, groups, sources, VIN) when
  PGN rows exist or the listener is up; the notes explain the grammar and
  little-endian words.
- **Trouble Codes**: path chip "J1939, heard (DM1)", the four lamp chips
  (MIL, Stop, Warning, Protect) when the report carries `lamps`, DM1 items
  as `active` with `SA n` in the ECU column and a Count column, SPN-FMI
  explained, and no clear section on a J1939-only vehicle ("WiCAN only
  listens on this vehicle").
- Mock: presets `detect:"j1939"` (+ `j1939Listening`), `dtc:"j1939"`,
  `autopidCfg`, `GET /api/j1939`, `window.__mockSettings`; probe
  `tools/webui_preview/probe_j1939.mjs` (`J1939 PROBE PASS`, 40 checks, plain
  and minified).

### OBD dialects on the pages (2026-10-03)

A vehicle that speaks OBD over UDS (ISO 27145 / SAE J1979-2, the vehicle
store's `dialect: "uds"`) needs no page of its own; three places learned
about it (TASK_j1939_wwh.md phase 3):

- **Trouble Codes** (`PAGES.dtc`): the table is built from the report's
  `items` (one row per code, category and ECU; an `ECU` column appears when
  the firmware names it), a line under it says which ECUs ask for the lamp
  when more than one answered, a chip names the path (`OBD on UDS (ISO
  27145)`), and the clear section is worded for the path the firmware
  reports in `path`: mode 04 on an OBD-II car, "This clears ALL emission
  codes of every ECU" (service 14, group FFFF33) on a `wwh` path, the
  per-code clear on `uds`. Older firmware (no `items`, no `path`) renders as
  before.
- **Automate > Parameters**: "Add selected" in the scan results keeps a
  row's `init` (and `rxheader`). A scanned row of such a vehicle carries the
  address of the ECU that owns the value; dropped, the row would be asked of
  every ECU at once. The results list and the Standard table show that ECU
  beside the request (`22F40C · ECU 58`; `rowEcu()` reads it from the init).
- **Quick Setup** (`setup.js`): the detection steps say what is asked
  (`0100`, then `22 F400`), the result and the store's list name the dialect
  beside the protocol (`CAN 29-bit 500 kbit/s, OBD on UDS (ISO 27145)`); an
  OBD-II car reads as before.

Preview: `mock_api.js` serves the real `/api/autopid/dtc` shape (presets
`window.__mockPreset.dtc = "obd" | "wwh"`) and an ISO 27145 van for the
detection (`detect: "wwh"`). Probe: `probe_wwh.mjs` (38 checks).

### Pause VPN per network (2026-10-11, issue #976)

The flags live with the networks under Settings > WiFi > Station: the schema-driven
station form renders `sta_vpn_pause` as a switch labelled `Pause VPN` right under the
password (`LABELS` / `HELP`, the advanced rows sort below the essentials), and
`fallbackNetworks()` draws a `pause VPN` tick beside `trusted` on every backup block
(deleting a block resets the flag). The VPN page sets nothing: `status()` paints the
badge `Paused on <network>` as an info chip while `/api/vpn` says `paused`, the debug
block carries a `Network` row, and one note at the end of VPN Settings
(`data-key="vpn_pause_note"`) names the marked networks (read from
`/api/settings/wifi_manager`) with a link to the WiFi tab; it hides while the VPN is
disabled. The Status page's VPN row goes through `vpnWord()` (Connected / Paused on X /
Waiting for network / Connecting / Disabled). Probe: `probe_vpn_pause.mjs` (mock presets
`vpn` and `wifi`). Mockup Ali decided on: the artifact linked from
`internal/TASK_vpn_pause.md`.

### Routes through the tunnel (2026-10-11, issue #975)

The VPN Settings card shows `routes` as one field, "Routes through the
tunnel", right under the tunnel address (not advanced; `LABELS` / `HELP` /
`PLACEHOLDER`): a comma list of CIDRs, the tunnel's own network included.
`importWgConf()` maps EVERY IPv4 AllowedIPs entry of a wg-quick file into it
and turns `0.0.0.0/0` into Route all traffic (it used to keep the first entry
as allowed_ip + mask). The debug block carries a `Routes` row from
`/api/vpn`. Probe: `probe_vpn_routes.mjs`.

### Configuration backup (2026-10-11, issue #979)

System > Maintenance carries three cards from the on-demand chunk
`web/backup.js` (`PAGES.__backup.cards()`, loaded by `PAGES.system` into
the Maintenance pane after the Maintenance card and the diagnostics (the
Maintenance card leads the page since 2026-10-28, Ali); the old Download Config
and Upload Config buttons and their note are gone). Over
`config_backup/HTTP_API.md`:

- **Configuration backup**: the five parts that may leave the device as a
  list of ticks (`.bk-row[data-part]`, a count and a size each from
  `/api/backup/status`, the certificates row never tickable and saying
  where they are kept), an Everything master tick, Remove sensitive data
  and Encrypt the file switches (`.frow[data-key="bk_redact"|"bk_encrypt"]`),
  and the file line (`.bk-file`: the name as it lands in Downloads, a
  plain text / encrypted / passwords removed chip, the size) with the
  Download backup link at the right (`a.bk-download`, an `href` to
  `/api/backup?include=…&redact=1&encrypt=1` rebuilt on every change; no
  `href` and `.disabled` while nothing is ticked or the key was never
  downloaded, with the key nudge banner carrying the key link).
- **Copies on the device**: the internal flash and SD card rows
  (`.bk-place[data-where]`, refreshed time, parts, size, a Restore button
  each; a card from another WiCAN is named and restores through its key
  file), Refresh the copies now at the right, the `config_backup.auto_copy`
  settings row (a real setting: Submit, restart) and the Backup key row
  (`.frow[data-key="bk_key"]`: the key link, downloaded / never downloaded,
  the nudge until downloaded). The key link is an `<a download>`; the
  device marks the key downloaded when it serves it, so the cards re-read
  the state 1.5 s after a click.
- **Restore from a file**: Choose backup file at the right; the file goes
  to `POST /api/backup/inspect` as a raw body and the manifest opens the
  preview modal (`restorePreview(m)`, 640 px): the facts, the warnings
  (`key_missing` with a Choose key file picker that posts the file's text
  to `unlock`, `bad_key`, `other_device`, `redacted`, `newer`), for another
  device's backup the ticks Keep this WiCAN's own names (on) and Keep this
  WiCAN's VPN settings and keys (off), the six parts with a tick per part
  present, the errors, and Restore and restart (`.bk-go`, disabled while
  locked or refused; posts `include`, `keep_identity`, `keep_vpn_keys`,
  then `expectReboot`). Cancel drops the staged backup.

Rules kept: nothing applies live (the auto switch stages; a restore
restarts), action buttons at the right edge of their row (Ali, 2026-10-11),
no long dash. Mock: `/api/backup/*` in `mock_api.js` with
`__mockPreset.backup` (`keyDownloaded`, `sd`, `sdDeviceId`, `copies`,
`inspectAs`: `plain_this | enc_other | redacted | newer | v1`). Probe:
`probe_backup.mjs` (`BACKUP PROBE PASS`, 44 checks, plain and `--min`).

### The SD card format (2026-10-25)

A card in the slot WiCAN cannot read (exFAT, the factory format of every
card over 32 GB; NTFS; a blank card; a damaged boot sector) used to read
"Not mounted" and "Insert an SD card" everywhere. The status poll's `sd`
summary (`dev_status_manager/HTTP_API.md`) now tells the pages what is in
the slot, and `GET /api/sdcard` + `POST /api/sdcard/format`
(`external_storage/HTTP_API.md`) let the user format it from the page:

- **The bar** (`#sdbar`, the ESPNetLink bar's twin under the header on
  every page but Quick Setup; `paintSdBar()` from the status poll and the
  router): the lead names the problem ("SD card inserted, but WiCAN cannot
  read it." / "but it has no filesystem." / "but it does not answer."), the
  sentence what the card holds with its size and what a format does, then
  **Format the card…** (the dialog) and **Not now** (per card and state, in
  this browser: `localStorage wican-sd-seen` = `<serial>:<state>`). A card
  that does not answer paints the bar red with OK only: no format can help
  it.
- **The dialog** (`sdFormatDialog()`, shared by every entry point): ask
  (the card, what was found, what the result is, the erase warning, **Yes,
  format it** as the danger action), the words (`#sd-phrase`, Continue off
  until `format sdcard` is typed, case and spacing ignored; the page posts
  the canonical phrase), the progress (the device answers 202 at once; the
  page polls `GET /api/sdcard` once a second, no backdrop close), the
  outcome (SD card ready: mounted at /sd, FAT32, the free space, the time
  it took, Open File Manager; or The format failed with the error and Try
  again). A card pulled out mid-way closes the dialog with a toast.
- **The words elsewhere**: the Status page's SD card row (`sdRowValue()`:
  "Inserted, exFAT: not readable" + Format…; "Inserted, not answering" + the
  error), the File Manager's SD card tile (`files.js`: the chip, the card
  and what it holds, a Format… button at the right; a dead card greyed
  out), the Logger's chips ("Not recording" + "SD card inserted, not
  readable" + Format the card…) and its empty file list, the backup copies
  row (`backup.js`: "not readable" + Format…).

Rules kept: nothing is formatted without the typed words, a mounted card is
never offered (the route refuses it too), action buttons at the right edge,
no long dash. Mock: `__mockPreset.sd` = `mounted` (default) | `exfat` |
`exfat_failfmt` | `blank` | `unknown` | `failed` | `absent`,
`__mockPreset.sdFormatMs`; `__mockSd()` for the probes. Probe:
`probe_sdcard.mjs` (`SD CARD PROBE PASS`, 44 checks, plain and `--min`).
Design record: `test-reports/logs/sdformat_20261025/` (the override over a
copy of the preview, the shots, the review page).

### OBD chip and CAN self-test cards (2026-10-28, issue #1023)

System > Maintenance opens with the Maintenance card (Reboot, Restart
History, Run Quick Setup again, Factory Reset: it leads the page since
2026-10-28, Ali; the backup cards used to sit above it), then two cards from
the on-demand chunk `web/diag.js` (`PAGES.__diag.cards()`, loaded by
`PAGES.system` into `#diag-cards` right after the Maintenance card, the
backup cards after them; `internal/TASK_issue_1023_phase2.md`).
Over `GET /api/obd_chip`, `POST /api/obd_chip/update`, `POST/GET
/api/obd_chip/probe`, `GET /api/obd_chip/report`, `POST/GET
/api/can/selftest` (`components/HTTP_API.md`):

- **OBD chip**: the state row (ready at N baud or not up, the chip's
  firmware as its version alone, `firmware V2.3.24`, green at the
  packaged version and amber below it: the chip's own words, `MIC3624
  V2.3.24`, stay in the API's `fw.chip` and in the configuration report,
  Ali 2026-11-01; the packaged image as `device carries V2.3.24`; a
  `last update: <error>` chip only after a failed run), refreshed every
  3 s; a warn banner while the chip is down, with what the bring-up
  received at each speed (`bringup.wire`) and a pointer at the wire tools;
  or, while the chip's firmware is older than the packaged one
  (`fw.outdated`: a chip left on V2.3.22 is accepted at start-up and
  flagged here, Ali 2026-10-28), the banner "A newer chip firmware is
  available" naming both versions with the **Update to V2.3.24** button
  at its right edge (`btn pri`), behind `confirmModal` (Cancel / Continue)
  and then `POST /api/obd_chip/update` with an empty body: the device
  writes the chip only when its image is newer, there is no forced
  rewrite any more (Ali, 2026-11-01: the Rewrite button, its danger
  styling, the help paragraph about it and `force:true` are gone, and the
  button exists only while there is something to update to); the update
  progress bar while `fw.update` runs; "Read the chip configuration" at
  the right edge of its help line (the report route into a read-only
  block with Select all, the text a bug report needs: identity, voltage,
  protocol, the programmable parameters, the sleep settings; 409 = the
  chip is down, shown as a banner); the wire tools folded at the bottom
  (Speed, Send word, Listen seconds; Probe, Scan all speeds, Detect
  speed, Listen; every byte the chip sent as text and hex in a console
  box kept in localStorage `wican-diag-wire`; the chip's own log lines
  from the ring under it). Every text-plus-action row is the
  `actionRow` grid (`minmax(0,1fr) auto`): the text wraps, the button
  never drops under it.
- **CAN self-test**: one help line, the verdict chip, one Run button
  that POSTs and polls, and the result as the same copyable block as the
  chip configuration: the console's `can_selftest` text (the verdict, the
  bus, the three steps with their timings and details, the exchange lines,
  the time taken), stamped with the run's time. The exchange: the chip
  asks `010C` to 7DF, the WiCAN's own CAN controller answers as ECU 7EF
  with 1726 rpm, so one run proves both transceivers, both directions and
  the wiring, with or without a vehicle (`live:true` says a vehicle was
  answering too). A refusal (the chip down, the bus off) is shown as not
  run with its reason, no block. Ali, 2026-10-28: the first cut's tiles,
  step cards and failure table were too much.

Probe: `probe_diag.mjs` (plain and `--min`; the mock presets a chip up,
a chip down with wire notes, a chip left on V2.3.22, and a self-test pass).

### The OBD chip firmware prompt, the destinations table and Active on (2026-11-08, `internal/TASK_chip_prompt_destinations.md`)

- **The OBD chip firmware prompt.** On chip firmware V2.3.22 the warm start
  the bring-up sends kills the chip's automatic protocol search until a
  power cycle (`internal/OBD_CHIP_ATWS_AUTO_SEARCH_2026-11-08.md`), so the
  page asks for the update to the device's image wherever a user lands:
  one shared helper, `chipFw` in index.html (`prompt` = the one-button
  popup, `run` = the Maintenance card's job with its progress in a popup
  that a tap beside it does not close, polling `GET /api/obd_chip` once a
  second and waiting for the chip's fresh bring-up, `done` = the Done
  popup, `banner` = the same offer as a banner). Quick Setup's vehicle step
  (setup.js, `W.chip`) opens the popup once per visit while
  `fw.outdated`, keeps the banner `#qs-chip-banner` and disables Detect
  with the chip `Update the chip first`; the Status page (Ali: a device set
  up before never re-runs the wizard) shows the banner `#chip-fw-banner`
  on every refresh while the chip is outdated and opens the popup once per
  browser session (`sessionStorage`, `wican-chip-prompted-<device id>`).
  The popup rules (`.qs-pop*`, `.qs-spin`) live in index.html since both
  pages use them. Probes: `probe_chip_prompt.mjs` (six scenes); bench:
  `.	est.ps1 chipprompt`.
- **Automate > Data destinations.** One status chip per row in a fixed
  column with a fixed vocabulary (`Not applied yet`, `Destinations
  paused`, `Paused`, `Waiting for <names>`, `Failing · retry in N s`,
  `Waiting for network` / `the broker`, `Sending · N`, `Waiting for the
  first send`), the row's facts as a muted line under its topic or URL
  (`.dest-sub`), Options as a named button, the options as a panel
  (`.dest-panel`: Active on first, the type's rows, the Delivery counters
  last), the Links row with a banner while a link is down, a restart notice
  while a saved table is not running yet, Test disabled on a row that is
  not applied. Probe: `probe_destinations.mjs`.
- **Active on.** `networkPicker()` (beside `certSetPicker`) is a dropdown
  that reads like a select and opens a list with check boxes: All networks
  first, the station networks under Settings > WiFi (`wifiNetworkNames()`:
  the main one and the backups, the connected one marked from
  `/api/wifi/status` `ssid`), a stored name that is not configured any more
  struck through; ticking a network clears All, unticking the last puts
  All back; the value is the `networks` setting (names separated by commas,
  "" = all). `networksRowPicker()` swaps the generated row of the Home
  Assistant card (`settingsForm` + `advancedExcept` naming `networks`),
  whose stats line reads `Waiting for <names>` while `on_network` is
  false. The sleep countdown bar reads "Kept awake while the OBD chip
  firmware is updated" for `activity_why: chip_update`.

### The Bluetooth memory profile and the VPN hold note (2026-10-31, issue #1024)

Settings > Bluetooth is built by `bleForm()` instead of a bare
`settingsForm("ble_manager")`: the `ble_ram_profile` row (label "Memory
profile", words Full (BLE 5 options) / Lean (recommended) / Custom, the
help text with what Lean frees and what Full keeps) sits with the
essentials in the schema's order, right after `advertising`; the three
controller knobs (`ble_max_act`, `ble_acl_tx_bufs`, `ble_scan_cache`) are
advanced rows that the `visible` filter shows only under Custom, the WiFi
card's pattern for its buffer knobs. Under Lean the
firmware accepts legacy advertising on 1M only, so the `advertising` and
`phy` selects are greyed and a note under the advertising row
(`data-key="ble_lean_note"`) says so; picking Lean while a BLE 5 word is
staged puts `legacy` / `1m` back into the staged values (`rows.load()`)
and stages the group, so Submit never meets the firmware's refusal. The
VPN page shows `/api/vpn` `reason` (why a Tailscale tunnel is held in
`waiting`: the internal-RAM floor and the levers the device has) as a
warn note above the settings card (`data-key="vpn_hold_note"`, hidden
while the field is empty) and a `Relay` row (`derp`) in the debug block.
Probe: `probe_ble_ram.mjs` (the mock presets `window.__mockPreset.ble`
for the Bluetooth values and `.vpn` for the tunnel state; 23 checks,
`BLE RAM PROBE PASS`).

## The new-car bar (2026-10-26)

Ali set a device up on the simulator (two standard PIDs chosen), took it to
his car, and the Dashboard was empty with no word: the car answered as a new
one, became current with its rows stored off, and his two rows stayed with
the simulator's entry. The identity rule stays (two cars of one make can
share an ECU set); the page says so now. The status poll's `vehicle` summary
(`dev_status_manager/HTTP_API.md`) drives it:

- **The bar** (`#vehbar`, under the header on every page but Quick Setup;
  `paintVehBar()` from the status poll and the router): up while the current
  car's setup never finished (`pending`) and the store holds another car.
  The lead "WiCAN is reading a car it was not set up for.", the sentence
  naming the car ("(no VIN)" when it has none) and what to do, then **Set
  up this car** (`#/setup/vehicle`, where Forget and "Same car as..." live)
  and **Not now** (per car key in this browser: `localStorage
  wican-veh-seen`).
- **The Dashboard** carries the same word as a banner above the tiles
  (`#dash-vehnote`, painted by the same function), since that is where the
  empty tiles are; Not now on the bar leaves it.
- Probe: `probe_newcar.mjs` (the fork from the Dashboard, the wizard without
  the bar, one pending car alone, a set-up car current). Bench:
  `tools/testbench/obd/autopid_newcar_bench.py`.

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
Manager) and `monitor.js` (CAN Monitor, 2026-09-07), `setup.js` (Quick Setup, 2026-10-01)
and `backup.js` (the configuration backup cards of System > Maintenance,
2026-10-11; a chunk that is not a page: the System page loads it and
appends its cards), `fwupdate.js` (the Updates card of System > Firmware
update, 2026-10-18, the same pattern), `diag.js` (the OBD chip and CAN
self-test cards of System > Maintenance, 2026-10-28, the same pattern).

## Firmware updates (2026-10-18, `internal/TASK_fw_update.md`)

Downloads go over WiFi only (Ali, 2026-10-19): on the dongle's mobile
connection the card lists the updates under a warning banner and both
Update buttons stay disabled with the reason as their tooltip; there is no
cellular setting and no dialog that allows a mobile download.

System > Firmware update opens with the **Updates** card from
`web/fwupdate.js` over `GET /api/fw_update` (fw_update/HTTP_API.md): the
legend tools carry "checked N ago" and **Check now** (`POST
/api/fw_update/check`); one row per product (the ESPNetLink row, since
2026-10-19, is the relay's: the dongle's reported version with a USB or
WiFi chip, what its own release list offers, **Release notes** for
`target=dongle`, **Update dongle** behind a dialog that says how the
image travels and that internet through the dongle pauses for about a
minute, `Needs a USB flash` with a docs link for a dongle firmware
without an update route, `No ESPNetLink dongle is connected` when none
is known; its job words name the dongle and the transport, Cancel
disappears once the push started, the outcome reads `Dongle updated to
X (sent in N s)` or a danger banner with the cause and Try again) with
the installed version (its build as
the tooltip, a `Development build` chip when the string is a git describe,
`withdrawn` when the manifest yanked it), a second line "6.1.0 available,
2026-10-20, <summary>" or the reason (Up to date, the stepping stone the
channel does not carry, Needs a USB flash), and at the right edge **Release
notes** (the modal renders the device-proxied markdown with `mdLite`:
headings, lists, bold, inline code, https links; everything escaped) and
**Update** (`POST /api/fw_update/install {target:"self", version}`), which
asks first when the layout verdict is `wipe` (the dialog names the erased
partitions, points at the backup, "Install and erase") and when the uplink
is the dongle's mobile connection (`allow_cellular`). A running job shows
its bar and phase under the rows with **Cancel** (`POST /api/fw_update/abort`),
polled every 2 s; `rebooting` hands over to the page's reconnect screen.
`last_result` paints the outcome after the restart: verifying (10 min or a
planned restart), verified, rolled back (danger, with Try again) or did not
start. The `fw_update` settings form sits under the rows (staged; the
global Submit changes applies it with the restart); `base_url` and
`cert_set` are advanced. The old upload card is **Manual update**, below.
The About page shows the build (`/api/info.fw_build`) and the partition
under the version. Mock scenes: `__mockPreset.fw` = `up_to_date |
available | step | wipe | serial | cellular | job | rolled_back |
no_manifest | yanked`; probe `probe_fw_update.mjs` (`FW UPDATE PROBE PASS`).

## Pages

DEVICE: **Quick Setup** (2026-10-01, chunk `web/setup.js`, `TASK_quick_setup.md` in the
app repo): the first-run wizard. Opens by itself while the access point still has the
factory password (`route()` sends an empty hash to `#/setup`; "Skip setup for now" is a
sessionStorage flag), later from the first sidebar entry or System > Maintenance; the
sidebar hides while it runs (`#app.setup-focus`). Ten screens on a step rail in two
halves: safety rules (three acknowledgements), use case (three tiles: Home Assistant, own
MQTT broker, and "Just connect WiCAN to my WiFi" since 2026-10-07 (Ali; it was a small
radio under the tiles before): chosen, the details step is skipped and leaves the rail
(`hide` on the step, the numbers follow), Review has no Home Assistant row, Checks and Done
no Home Assistant card; OBD apps / ABRP / logger greyed as later work), details (the HACS
install steps for the WiCAN integration, or the broker form), AP password (8 to 63,
never `@meatpi#`; a device with its own password may keep it), home WiFi (scan via
`/api/wifi/scan`, one row per SSID with the strongest signal, `auth_mode`, OPEN networks
cannot be chosen, blank password keeps the stored one for the same SSID; since 2026-10-06
Continue is **Test and continue**: `POST /api/wifi/try` joins the network with the typed
credentials before anything is saved, the page polls `GET /api/wifi/try` once a second with
a 3 s abort per poll (the AP blinks while the radio changes channel, and a page over the
station loses the device for the trial; the page's offline-to-online re-route rebuilds the
step, which picks a running test up from `W.testing`), a pass goes straight to Review with
"Password accepted · <ip>" on the Home WiFi row, a failure stays with its card (password:
the field marked, reason N; not_found; no_ip; refused; timeout; lost contact), Test again,
and a quiet Continue anyway ("Test failed · continuing anyway" on Review, an amber circle);
a pass is remembered for those very credentials, a blank password that keeps the stored one
needs no test, a device in access point only mode skips the test with a note; since
2026-10-08 the POST carries the scan row's `channel` (0 for a hand-typed name) so the
station scans that one channel and the access point's clients see the announced switch
within a second instead of three seconds of silence; **the phone's mobile data** (Ali,
2026-10-08, "ask the user to shut down his 5G/4G before they continue": when the page rides
the access point, `onAp()`, the settings' `ap_ip`): the first screen asks for mobile data
off before the three rules (the two pauses ahead, Settings > Mobile data or Cellular, a
computer has nothing to turn off), and Test and continue opens the page's popup once per
visit (the phone icon, "Make sure mobile data is off on this phone", the why with the
network's and the access point's names, where the switch is, one Continue that starts the
test; a tap beside it closes it without testing; Ali asked for the popup after seeing a tick
on his phone); the Trying card says the phone may drop off the access point for a few
seconds and rejoin by itself; **after it joins** (Ali, 2026-10-07): Station only, recommended and the default on a fresh device, or
Access point + Station, which reveals "turn the access point off while WiCAN is on X",
ticked by default (`wifi_manager.mode` sta / apsta, `ap_auto_disable`); a configured device
already running AP + Station with a network keeps that choice as the default of a re-run;
the choice is free because a station that cannot join is never a lockout: a 5 s press of
the button is button_manager's config mode, the configured access point up for 10 minutes,
and the wizard says so wherever it used to promise the access point), **USB devices**
(2026-10-07, Ali: "quick setup for usb devices such as espnetlink, usb to ethernet and gps";
mock https://claude.ai/code/artifact/14acea5f-2625-4728-8ce3-8e64bb4a26bd): the host side of
the connector is on by default, so the step reads what is plugged in live, before the
restart: `/api/usb`, `/api/espnetlink`, `/api/usb/acm` and `/api/gps` every 3 s
(`usbLive()` / `usbKind()`: an adapter by the Ethernet driver with a known id, the dongle by
the link task's state or 303a:4007, a receiver by the enumerated `device` (u-blox 1546, or a
GPS/GNSS product name: `/api/usb` reports the enumerated device for any class since the same
night, and `/api/usb/acm` says whether it is read (`reading`) and as what (`mode` nmea for a
receiver streaming NMEA, parsed by usb_acm_cli)), else unknown / none);
three information tiles with the found one lit "Found", one card (`usbSummary()`, shared with
Checks and Done) and the one question the device has, default = the device's setting:
ESPNetLink **USB Ethernet (recommended, `usb_rndis`)** with the note that a GPS that never
gets a fix this way wants WiFi modem, or WiFi modem (a dongle already paired keeps the mode
it was paired with), plus the SIM's APN asked on its own (2026-10-28, `apnSection()`: "Your
SIM's APN" with where the provider publishes it, a live verdict under the field from the
dongle's health, refreshed with the connector poll: Online with the APN or on the dongle's
default, Not online yet with the APN named as the usual reason, Waiting for the PIN; the
username and password folded as rarely needed; the SIM PIN row above it when the SIM asks;
until then "Carrier settings (optional)" folded the APN away and a Simbase SIM stayed on the
dongle's default APN; the `espnetlink.apn` / `apn_user` / `apn_password` / `sim_pin` settings
the pairing pass writes to the dongle in every mode); under the dongle card the basics as rows
in the same box (`nlRows()`, `.qs-nlcard`):
SIM (found / not found with what to do / PIN locked, dongle api 8; a dongle without the key
counts as found when attached), network and type, signal in dBm with a word (good above -65,
fair to -80, weak below with the antenna hint), internet with the carrier address, GPS (fix
with satellites, or the sky hint plus the switch-to-WiFi-modem hint on USB Ethernet), APN;
no SIM turns the card to a warning and leaves out the rows that need one. Adapter: internet
over the cable only when WiFi is down (the default) or always (`prefer_usb_route`). GPS
receiver: nothing to ask; a fresh device says it is read after the restart (the save turns
`usb_acm_cli.enabled` on). A connector in device mode offers to switch it back to host at the
restart. Nothing plugged in is a plain Continue. The save carries only what changed
(`espnetlink` enabled / mode / carrier, `usb_host_manager` route preference or host role,
`usb_acm_cli` enabled); Review gets a USB connector row only when there is something to say
(`usbReview()`); Checks and Done read the routes again, so a page that arrived by the link
shows the same card (the pairing's own extra restart is named on it)), Review and
restart (a checklist since 2026-10-07, Ali's sketch after "the location of the green pills
looks off or random": `.qs-rv`, a circle per row (the rail's own: green check = settled by
this save, empty = comes after the restart, amber = a warning), the title in a column of
its own, the value with its short attributes beside it and the verdict or note under it,
joined by middle dots, no pills; the mode on the rows and in the steps: "off after the
restart, hold the button 5 s to bring it up when needed" / "off while WiCAN is on X, back
whenever X is lost"; `store.commit()`: PUT wifi_manager mode sta or apsta (+
`ap_auto_disable` as ticked) + station + `sta_trusted` + AP password, optionally
mqtt_manager + a data_destinations `~/autopid` row, one submit); then, from the address
link `http://<address>/#/setup/checks` on the home network (no mDNS name anywhere in the
wizard since 2026-10-07, Ali: "it's simpler to just use IP"; the address is the connection
test's, usually the same after the restart (same MAC, same lease), else the live station
address when the device is already on that network (a re-run), else the card says the
address is not known yet, that this page shows it as soon as WiCAN joins while the phone
stays on the access point, and that the router's device list shows WiCAN as `wican_<id>`,
its DHCP host name (`routerName()`)): Reconnect (the link, Open WiCAN and Copy link; the
live card watches TWO places since 2026-10-06: the page's own origin, for a phone or PC
that stays on the access point (join status, the station address), and the address, with
a no-CORS `fetch` of `/api/info` every 3 s once the restart has had 12 s, for the phone
that moved to the home WiFi by itself (the AP password changed, or the access point went
off once WiCAN joined), which leaves the page's origin out of reach for good; when the
address answers the card says so and offers `Continue on <ssid>` as a button to the link
(never a jump on a timer, Ali's call: this screen is the user's only map and the back
button cannot return to it, and an opaque answer cannot prove it is WiCAN), and a success
popup comes up once per visit (Ali, 2026-10-07: "popup when the device is connected to the
desired network and ask the user to press"; `.qs-pop`: a large green check, "WiCAN
successfully connected to X", the address and the link, ONE button `Continue on X` with the
open-in-new mark, the only way on; a tap beside it closes it and the card keeps the same
button); the popup also comes when the page's origin stays reachable and the device
reports the join after the restart's time (then it says to join X first when the phone is
still on the access point); until then
the card says what to do meanwhile, and after 40 s points at the way back with the
wrong-password hint: the access point for AP + Station, the router's device list and the
5 s button for Station only; the page pill reads "WiCAN's access point is out of reach
from here" on that screen instead of "Device offline"), Checks (WiFi with the address and
the router's name for it / the access point: "off: Station only" with the button, "off
while WiCAN is on X" for the auto choice, else secured or factory password / Home
Assistant via `GET /api/webhook` / MQTT via `bits.mqtt_connected`, polled every 3 s, Fix
links back into the earlier steps; the mode read from `/api/settings/wifi_manager` after
the restart's reload), Vehicle (second pass, 2026-10-01 evening: DETECT
first, profile last. "Detect my vehicle" = `POST /api/autopid/vehicles/detect` (falls back to
`POST /api/autopid/std_scan` on older firmware), the phases protocol / vin / pids from
`GET /api/autopid/std_scan`, then the result card from `/std_scan/result` + the store
`GET /api/autopid/vehicles` (VIN or "identified by its ECUs", detected protocol, PID
count, a name field defaulting to the VIN's manufacturer + last 4; the facts as muted text
beside the values, no pills, 2026-10-07), a "Welcome back" card for a known car;
**2. Standard PIDs** (2026-10-07, Ali: the scan found what the car answers, the user
chooses what WiCAN reads): Choose PIDs opens the page's standard-PID picker
(`pickStdRows()` in index.html, the one Automate > Parameters opens on a scan: a scrollable
list, tick | request | name | unit, Select all, Clear, a count, Add selected), nothing
ticked for a new car, a car SET UP before comes with its current rows ticked
(`W.stdDefault` = the config's std rows after the scan, since a re-scan keeps a known car's
own tables; `W.stdPick` once the picker was used), none is allowed and Continue does not
wait for it. A car the store has but whose setup never finished (`pending_profile`: the
device stored its rows at the detection, nobody chose them) starts with none like a new car
(2026-10-09, after Ali's "none chosen, later all enabled": a page reload between the
detection and Finish, then Detect again, had pre-ticked every stored row); the card's
Standard PIDs row says "N chosen" / "none chosen yet"; a J1939-only
vehicle has no requests and no section 2; then the profile as section 3: suggestions
filtered by the VIN's manufacturer code (the `WMI`
table in setup.js), the shared `vehicleProfilePicker()` for the rest, "keep it without a
profile", a protocol-mismatch warning when the profile's init names another protocol than
the detected one, and **Test profile**: one `POST /api/autopid/test` per profile PID
(init chain, type specific, the shifted expressions) shown in a modal, one row per
parameter with value / unit / request, a summary chip, Use this profile. The store list
("Vehicles this WiCAN knows") with Current / Profile pending chips and Forget
(`DELETE /api/autopid/vehicles/<key>`). Finish = `PUT /api/autopid/vehicles/<key>` {name,
profile, specific_init} (empty profile = the chosen standard PIDs only), `PUT
/api/autopid/config` with the standard rows cut to the chosen ones (the firmware stored
every row the scan found under a new car; a chosen row the config lacks is added from the
scan, one row per request, so a request the profile already carries is not added twice; a
car switch snapshots the result into the car's file, so the choice comes back with the car;
since 2026-10-09 a chosen row loses a stored `enabled: false` (the firmware stores a new
car's rows off, every one: nothing reads until ticked), and a Finish without a detection this
session with nothing chosen cuts the standard rows of a car whose setup never finished,
while a car set up before keeps its rows), the profile's rows via `profileToPids()` and, since 2026-10-27, its passive CAN `filters` (tagged `profile`, in place of the last profile's; hand-made filters stay; Test profile names them, nothing to request for them), custom rows kept, staged
autopid {enabled, std_protocol "0", vehicle, specific_init} through `store.commit()`),
Battery and sleep (2026-10-01 night, Ali:
the Power Saving pair measured on the car. `GET /api/battery` once a second into a 90 s SVG
trace; the charging voltage is captured by itself from six stable readings at or above
13.3 V (a smart alternator: a 0.4 V climb then 12 s stable, or "Use the current reading"),
key-off is noticed as two readings 0.5 V under it and the rest is taken once ten readings
sit within 0.04 V (60 s cap); the pure `pwrRecommend()` makes sleep = resting + 0.3 V and
wake = sleep + 0.2 V capped at charging minus 0.4 V, two range sliders keep a 0.1 V band,
warnings for a wake above charging / a sleep under resting / small margins; under 9 V = a
desk on USB, the step explains and keeps the current pair; the skip is a real button since
2026-10-06 (Ali), `Skip, keep the defaults (13.1 / 13.2 V)` beside Continue. Finish stages
`sleep_manager` {enabled, sleep_mv, wake_mv} (wake_mv only when the schema has it:
sleep_manager v3). Reading the car's Finish de-duplicates parameter names across the config
it PUTs (`_2`, `_3`, a toast): the firmware refuses a repeat, and a file written by the old
SAE table could carry one (the loader repairs those since the same day; this is the belt).
Every battery voltage shows ONE decimal (Ali). The Reading the car step quotes the
measured pair; Done lists it), Reading the car (2026-10-01 night: the
autopid polling rules in plain words, prefilled from the device: poll rate 1/2/5/10/custom s
= the default group's `period_ms` in the live config with the rows on the old default
inheriting it (a fresh device gets the recommended 5 s, a device already polling keeps
its rate), `min_event_interval_ms` as "report a change at most every N s", when the car is
off = pause with Power Saving (`pause_follow_sleep`, the sleep voltage quoted from
`/api/settings/sleep_manager`) / below a chosen voltage (`pause_below_mv`, 12 to 14.5 V) /
never (battery warning), `pause_mode` as "while paused, also stop listening", the Standard
PIDs switch reading the vehicle step's choice ("The N of the M the scan found that you
chose"; off and greyed with none chosen; on once a pick was made this session even when the
device has the standard set off, 2026-10-09) and the vehicle-specific one (greyed without a
profile); the Custom PIDs switch left the wizard on 2026-10-07 (Ali) and `custom_enabled`
stays as the device has it; trouble codes (`dtc_enabled` + `dtc_scan_period_min`); Finish
lives here now and stages it all),
Done (addresses, states, the reading rules, the sleep-off note, where to go next). Wizard state lives in the
chunk; the second half rebuilds from device state. Probes: `probe_setup.mjs`, `probe_std_choice.mjs` (2026-10-09: the standard PIDs stay the user's choice through a reload, a re-detection, a skipped detection and a Finish from the URL).
· Status (stat cards + network/system tables; a Quick Setup pointer while AutoPID is off) · Settings (WiFi/AP
· Station & Bluetooth w/ scan · CAN · MQTT) · Automate (autopid
polling switch + pause rules · PID groups with their own
switch/init/protocol · Home Assistant webhook · Data destinations (the `data_destinations` table: MQTT / HTTP / HTTPS + cert set / ABRP rows with auth, live counters from `/api/destinations`, Test per row, one-time import of the older timer-rule destinations; 2026-09-19) · PID
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
WiFi on 2026-09-07, no OBD chip card
since 2026-09-07)
· System (reboot · backup/restore · restart history · factory reset ·
**Certificates** (cert_manager sets: list w/ part flags, per-part PEM
upload via raw `/api/certs/upload?set&type`, NOT the api() JSON
wrapper: delete w/ confirm; key material is write-only by design; the
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
Settings, the Editor tab is a full-width "Stored scripts" table (name,
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
and sql.js: nine files under `cache/www`, a plain textarea until it is
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

## Connections card (Settings page: the bridge builder)

Pure settings composition, ZERO firmware surface: each row is
[interface ▾] ⇄ [connection ▾] with contextual fields (TCP/UDP → port,
WebSocket → path, CAN → protocol raw/slcan/gvret/realdash) + enabled +
Add (max 4). Compiles into `bridge_manager.bridges` AND maintains the
backing `socket_manager.servers` (reuse by proto+port, else allocate a
free/disabled slot, name `tcp<port>`) / `websocket_manager.channels`
(reuse by path, name `ws_<path>`). Client-side guards: single-consumer rule, port and
`/ws/` path validation, slot caps, "CAN bus disabled" hint. Bridges it
can't model (hand-made relay pairings) show as a "custom bridge" line
with a pointer to All Settings and a Remove button. Everything applies
through the header Submit (reboot-to-apply).

2026-10-29 (the field report in `internal/unable-to-save-config`: two
rows to UDP servers that no longer existed locked a device out of every
settings page and could not be removed):

- the card stages NOTHING until the user edits a row (the shortcut from
  the Socket servers card counts as an edit): the compiled list differs
  from the stored one in order and in the a/b side for a device set up
  elsewhere, and a stage on open carried a list the user never touched
  into every later Submit, from any page;
- a row whose connection side is a jack the device does not have: a name
  of the card's own form (`tcp3300`, `udp3333`) is read back into an
  editable row with a banner ("udp3333 does not exist on this device ...
  Submit re-creates it as UDP 3333, or remove the connection") and the
  row's trash button; any other name stays a custom bridge line with the
  device's `reason` from `/api/bridges` when it has one, and Remove;
- a bridge the device reports as not running (`/api/bridges` `reason`)
  says so under its row;
- `store.apply()`: a refused PUT names its settings group in the toast
  ("Apply failed: Bridges: ...").

The firmware side of the same report: `bridge_manager` no longer refuses
a bridge whose endpoint is not registered yet (a server and its bridge
travel in one submit; a stranded row degrades alone with a W line and
the reason). Bench: `.\test.ps1 connections` (the page leg in Chrome:
`tools/testbench/system/connections_ui_check.mjs`).

## Preview + tests WITHOUT a device: `tools/webui_preview/` (2026-07-13)

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
node probe_destinations.mjs # Automate > Data destinations: rows, options per type, Test, import
node probe_adv.mjs         # advanced-gating visibility probe
```

Last run (2026-07-13): smoke 23 routes 0 errors; automate probe 16/16
(card order, essentials-only gating, scan→results-modal→dedup-merge,
rule-editor modal). The preview is also publishable as a claude.ai
artifact for review. Against a REAL device instead:
`cd components/web_ui_v2/web && python -m http.server 8000` then
`http://localhost:8000/?api=http://<dut-ip>`.
