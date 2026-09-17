# event_manager

The device automation engine (design: [TASK_event_manager.md](TASK_event_manager.md)):
components PUBLISH typed events and REGISTER named actions and pull
values; user RULES (settings, UI-edited) wire them together. The
manager knows no component by name — the bridge_manager ownership
inversion. Each participating component owns a `<comp>_events.c`
(the `<comp>_cli.c` shape).

```
SOURCES ──publish──▶ queue ──dispatcher──▶ match/when/cooldown ──▶ ACTIONS
                                │                                    ▲
                          RULES (settings)              ${…} templates + pull VALUES
```

## API (`include/event_manager.h`)

- `event_manager_init()` / `_start()` / `_stop()` — settings + log descriptor +
  the built-in timer source and `log.note`; start creates the dispatcher and
  worker tasks and arms the timers (`ESP_ERR_INVALID_STATE` when unconfigured).
- `event_manager_declare_source(decl)` / `_register_action(action)` /
  `_register_value(name, read)` — the three registries (static writes, any
  time after the component's own init; a name ending in `.` is a prefix).
- `event_manager_publish(ev)` — copy into the queue, never blocks
  (drop-oldest when full).
- `event_manager_rule_match_values(selector, key, out, max)` — a hot source's
  pre-filter (mqtt topics).
- `event_manager_stats(out)` / `event_manager_capacity(...)` — counters and
  registry occupancy (sources, actions) for the health surface.
- `event_manager_register_http()` — the `/api/events/*` routes.
- `em_action_t.undoable` (2026-09-17): the action reverses itself when run
  with `"undo":true` in `with` (a while-rule ending); `blocking` = worker lane.

## Dependencies

`PRIV_REQUIRES settings_manager log_manager esp_timer http_server_manager`;
`REQUIRES espressif__cjson` (the public header exposes cJSON). Init after
log_manager and settings_manager, before any component that declares or
publishes (declares are static writes, so order among publishers is free);
start after the settings boot pass. Publishers/registrants: autopid,
battery_monitor, imu_manager, dev_status_manager, mqtt_manager, rtc_manager,
led_manager, http_client_manager, obd_chip, uds_manager, script_engine,
data_logger, wifi_manager (2026-09-17). Nothing below this component
depends on it.

## Memory footprint (estimated 2026-09-17, sizes from the structs)

- PSRAM static: event queue 32 x 432 B = 13.8 KB, job queue 8 x ~450 B =
  3.6 KB, per-rule runtime state 16 x ~710 B = 11.4 KB (grew with `last_ev`
  for the live re-check), registries (32 sources, 24 actions, 24 values)
  ~2 KB, event ring 32 entries ~14 KB, the 2 KB render buffer, the settings
  probe table 16 x ~600 B = 9.6 KB.
- Task stacks (PSRAM): dispatcher 8 KB, 2 workers x 4 KB.
- Internal RAM: queue/task control blocks only (~1 KB). Heap: the rendered
  `with` cJSON per fired action (transient).

## Rules (settings `event_manager`, reboot-to-apply)

```json
{ "enabled": true,
  "timers": [{"name": "beat", "period_s": 15}],
  "rules": [
    {"name": "high_rpm", "on": "autopid.param", "match": {"param": "rpm"},
     "when": [{"key": "value", "op": ">", "val": 3000}],
     "do": "mqtt.publish",
     "with": {"topic": "~/alerts", "payload": "{\"rpm\":${value}}"},
     "cooldown_ms": 60000}
  ]}
```

`on` = `source.event` (must exist in the registry); `match` = typed
equality pre-filter; `when` = AND-ed conditions
(`== != > >= < <= changed contains`; OR = write a second rule). A
condition names EITHER a field of the trigger (`key`) OR a **live
value** (`"value":"${autopid.SOC}"`, any registered pull value,
rendered when the rule fires: numbers compare as numbers, `true`/
`false` as booleans, the rest as strings; an unresolved value never
holds) — so "CHARGING is 1 and SOC is above 20" is one rule
(2026-09-17, the Rules Builder). `"undo":true` makes a **while**
rule: the action runs when the conditions come true and runs again
with `"undo":true` in `with` when they stop holding — on the next
trigger event, or on the dispatcher's 1 s re-check of the live-value
conditions (the trigger may stay silent while SOC drifts). Only
actions declared `undoable` accept it (validation rejects the rest):
`autopid.group` restores the group's configured enable + rate,
`led.indicate` clears. Two while-rules on the same group: the last
undo restores the CONFIGURED state, not the other rule's override.
For an undo rule put the trigger's state (`connected`, `edge`) in
`when`, not `match` — a `match` mismatch is silently skipped, so the
opposite event would never reach the undo. Per-rule counters (`fired`,
`last_fired_age_s`, `active`) come from `GET /api/events/rules`.
`do`+`with` = one action, `${key}` templates resolve event values,
builtins (`ts`/`source`/`name`) and pull values (`${autopid.data}`,
`${autopid.<param>}`); `cooldown_ms` rate-limits. `changed` is
per-boot. Validation is strict at PUT time (registries populated);
timers publish `timer.tick {timer}` on esp_timer (64-bit µs — no
32-bit tick types anywhere here).

## v1 sources / actions / values

| Owner | Events | Actions / values |
|---|---|---|
| event_manager | `timer.tick {timer}` | `log.note {message}` |
| mqtt_manager | `mqtt.rx {topic, payload≤47ch}` — topics AUTO-derived from enabled rules' `match.topic` at start (hot-source pre-filter) | `mqtt.publish {topic, payload, qos, retain}` (async path; `~/x` = prefix-relative) |
| autopid | `autopid.param {param,value,unit,group}` (ON CHANGE + `min_event_interval_ms`), `autopid.pid_failed {pid,streak}`, `autopid.scan_done {found}` | action `autopid.group {group,enabled[,period_ms]}` (§5b context switching — EPHEMERAL, reboot restores config defaults; **undoable**: `undo:true` = `autopid_group_restore()`); values `${autopid.data}` (legacy JSON snapshot) + `${autopid.<param>}` |
| wifi_manager | `wifi.sta {connected, ssid}` on every STA link-up (got IP) and real drop (2026-09-17; failed attempts stay quiet) | values `${wifi.ssid}` ("" when down), `${wifi.connected}` (`true`/`false`) |
| battery_monitor | `battery.threshold {edge, volts}` (12.0 V down / 12.5 V up, 5 s hold; initial state delivered at boot) | value `${battery.voltage}` |
| imu_manager | `imu.motion {state}` (active\|stationary), `imu.bump {axes}` | — |
| dev_status_manager | `status.bit {bit, set}` on EVERY bit change (sta_connected, ble_connected, mqtt_connected, motion, time_synced, …) — context rules for free | — |
| rtc_manager | — | values `${time.iso}` (ISO8601 UTC) + `${time.epoch}` |
| led_manager | — | `led.indicate {r,g,b,mode}` (**undoable**: undo = clear) / `led.clear` (the ALERT slot only) |
| http_client_manager | — | `http.post {url, body, content_type?}` (no retries; errors counted; blocking → worker pool) |
| obd_chip | `obd.response {cmd, response≤47ch}` (published by the action) | `obd.request {cmd}` — chain a rule on obd.response to deliver the answer (blocking → worker pool) |
| uds_manager | `uds.response {ok, nrc, len, data≤15B, req}` (published by the action) | `uds.request {tx, rx, req, ext?}` — outcome-only; full payloads via a script's `uds()` binding (blocking → worker pool) |

**Default rules** (schema default; they REPLACED `main_events.c`):
`imu.bump` / `imu.motion` / `battery.threshold` → `mqtt.publish
~/events` with the legacy-glue payload shapes (`{"event":"bump",…}`,
`motion_active|stationary`, `battery_below|above` + `${time.iso}`).

Still planned (TASK §5): ble.client
sources (status.bit covers the connected-bit cases today), the
`time.at` calendar source, a dedicated target app.

## Behavior notes

- Publish NEVER blocks: queue depth 32, full = drop-oldest + counter.
- Two action lanes, chosen per-action by `em_action_t.blocking`:
  - **inline** (`blocking=false`, the default) — runs on the dispatcher
    task (8 KB PSRAM). Fast, non-network actions: `log.note`,
    `mqtt.publish` (already async), `led.*`, `autopid.group`.
  - **worker pool** (`blocking=true`) — a SLOW network/bus round-trip
    (`http.post`, `obd.request`, `uds.request`) is handed to a bounded
    pool (`EM_WORKERS=2` tasks, 4 KB PSRAM each) via a depth-8 job queue,
    so one 8 s HTTP+TLS post can't stall tick processing. The queue is
    drop-OLDEST when full (`stats.blocking_dropped` counts the drops);
    the rendered `with` cJSON ownership transfers dispatcher→worker.
    `run()` on a blocking action MUST NOT touch flash (PSRAM-stack task,
    §2 corollary) — all v1 blocking actions are network/bus only.
- No flash access, no retries on any lane — errors are counted
  (`stats.action_errors`), not retried. Boot-window publishes before
  the broker connects count there by design.
- Legacy destination parity is RULES, not code: the cycle-MQTT
  publish = `on timer.tick → mqtt.publish {payload:"${autopid.data}"}`
  (bench-verified).

## HTTP (see `components/HTTP_API.md`)

`GET /api/events/sources|actions|values` — the UI's rule-editor
vocabulary (zero hardcoded dropdowns; actions carry `undoable`).
`GET /api/events/log` — stats + the last 32 events with which rules fired
(the rule-debugging view). `GET /api/events/rules` — per-rule runtime
(`fired`, `last_fired_age_s`, `active`, `undo`, `enabled`).

## Files

| File | Role |
|---|---|
| `event_manager.c` | queue, dispatcher, rules+settings, timers, lifecycle |
| `event_manager_registry.c` | source/action/value registries, kv constructors, the log ring, JSON builders |
| `event_manager_rules.c` | PURE parse (incl. live-value conditions, `undo`) + the undo-vs-action check (host-tested) |
| `event_manager_eval.c` | PURE evaluation: match, when over trigger fields + live values, the while-rule step + live re-check, cooldown (host-tested) |
| `event_manager_template.c` | PURE `${key}` substitution (host-tested) |
| `event_manager_http.c` | the five routes |

## Tests

- Host (`host_test/`, `.\test.ps1 host event_manager`): parse happy/reject,
  script sugar, match/ops, contains, `changed`, cooldown, templates, live-value
  conditions (parse + evaluation + kinds), the while-rule step and re-check,
  the undo validation, and 12 SCENARIOS replaying rule combinations through
  `em_rule_decide()` (the engine's whole per-event decision): a WiFi
  while-rule over connect/repeat/drop/other-network, live-value drift undone
  by the re-check (and no re-arm without a trigger), a plain and a while
  rule on one trigger, `match` isolating other parameters, the state-in-
  match trap, cooldown gating the arming but never the undo, two while-rules
  arming/undoing independently, a disabled rule, a timer with a live epoch
  deadline, `changed` + cooldown, mixed key/live/string conditions, and the
  action template reading live values — 28 tests.
- Bench (`tools/testbench/rules_bench.py`, composed firmware): the registry
  surfaces, a `wifi.sta` while-rule firing at boot and overriding a group's
  rate, a timer rule with a live `${time.epoch}` condition undone by the 1 s
  re-check; restores the device's rules.
