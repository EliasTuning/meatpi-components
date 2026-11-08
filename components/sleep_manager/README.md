# sleep_manager

Low-power manager (rewrite of legacy `sleep_mode.c`,
`TASK_sleep_manager.md`). Battery voltage (read from
**battery_monitor**, this component owns no ADC) drives a pure
ladder: `NORMAL → LOW_VOLTAGE` (below `sleep_mv`, countdown of
`sleep_delay_min`) `→ SLEEPING → WAKE_PENDING` (recovered ≥
`wake_mv`, stable for `wake_delay_ms`) `→ reboot` via restart_tracker POWER_WAKE
(PERIODIC_WAKE for a check-in, 2026-09-07: the Status page shows which):
wake-by-reboot, not resume-in-place (every manager restarts clean).

SLEEPING = repeated **light sleep** with a 2 s timer wake; voltage is
still sampled between naps and each nap verifies the OBD chip stayed
asleep (≤6 re-sleeps, then an INTERNAL_RECOVERY reboot; a re-sleep is the
reset pulse, the chip's 1.5 s boot, then `obd_chip_sleep(true)`: the
`STSLEEP0` word and the pin, since 2026-11-07). Optional
periodic check-in reboot (`periodic_wakeup` + `wakeup_interval_min`),
gated below 11.9 V so a dying battery is never drained for telemetry.

Boot-loop guard (legacy parity): ≥3 unexpected resets while the
battery reads under 12.1 V → sleep instead of another crash lap.

**Critical floor (Ali, 2026-10-01; 5 min since 2026-10-06):** a battery
that reads under **11.90 V for 300 s** (`SM_CRITICAL_V`,
`SM_CRITICAL_DELAY_MS`, fixed) puts the device to sleep **whatever the
`enabled` setting says** and without waiting out a longer sleep delay;
the normal wake rules apply (wake voltage + hold, no periodic check-in
under the floor). A device booting on such a battery still comes up
normally (15 s grace) and stays reachable for the 5 min. The pure
tracker `sm_critical_eval()` (hysteresis 0.05 V) is host-tested; the
sleep bench's `critical_floor` leg and the matrix scenario of the same
name prove it with sleep disabled at 11.75 V. The delay was 120 s until
2026-10-06: that put a default device (5 min sleep delay) on an 11.6 V
supply to sleep 2 min in, in the middle of a Quick Setup, and Ali made
it 5 min, the default sleep delay. A default device under the floor now
sleeps after 5 min either way (the two deadlines tie); the floor still
acts with sleep disabled and ahead of a sleep delay longer than 5 min.

**The hold (Ali, 2026-10-06: "if the user is configuring the device and it
is about to sleep, prompt the user to extend").** `sleep_manager_hold(minutes)`
moves BOTH deadlines, the ladder's and the floor's, out to now + minutes where
that is later (pure `sm_hold_apply()`, host-tested: a hold shorter than what
is left changes nothing, only what counts moves, nothing to hold on a healthy
battery or asleep, the clock wrap). The states stay as they are, so a battery
that recovers above the wake voltage still ends the countdown. Three per
boot (`SM_HOLD_MAX`), 1 to 30 min each; a runtime knob, never saved, a
restart drops it. The policy and the tracker belong to the state task and the
request lands on the httpd task, so both are edited under one mutex
(`s_lock`, held for the evaluation of a pass, never across the entry sequence
or a nap); the status reads the hold under it too. One I line per press
(`held awake 10 min on request (1 of 3 this boot): sleeps in 600 s`). The web
UI: a Keep awake 10 min button on the countdown bar, the page's own dialog
one minute before the entry (once per deadline, visible tab only), the bar
saying "Kept awake on your request (2 of 3 holds left)" after, "10 min more"
as the button, and no button once the three are used. Decisions, Ali's:
ask at 60 s, 10 min per press, the floor may be held too (the dialog says
the battery is low), 3 per boot, Let it sleep only closes, the button on the
bar and in the dialog. Bench: the sleep bench presses three times with 30 s
of the minute left and holds the device to a minute after the last press,
the fourth refused; one press under the floor with sleep disabled.

**The countdown (2026-10-06).** The floor cuts a longer sleep delay
short, by design, and when it was 2 min a default device on an 11.6 V
supply was asleep 2 min after its sleep task armed, with "Sleep after
5 min" on the page and nothing saying why. The device now says what is
coming: the status carries
`pending` (which rule will put it to sleep first: `delay`, the sleep
delay running while the battery is under `sleep_mv`, or `critical`, the
floor) and `sleep_in_s` (the seconds to that entry, rounded up), on
`GET /api/sleep` and in the `sleep` command, and the web UI counts it
down on every page. The pure `sm_pending_eval()` picks the earlier of
the two deadlines (the floor wins a tie: the task evaluates it first;
the ladder counts only while sleep is enabled; nothing counts once
asleep) and is host-tested. The state task publishes cause and DEADLINE
as ONE word once per pass (`sm_pending_pack()`: two bits of cause, the
deadline's low 30 bits of ms), so a reader on another task never sees
half an update, and `sm_pending_unpack()` counts the seconds against the
clock at the read: the answer is exact however old the word is (the
first cut published seconds, up to a pass stale, and a page that ticks
by itself showed it as a stutter). Bench: the sleep bench holds the
countdown against the device's own entry line in every leg (within
2.5 s; measured 0.4 s), checks its pace on the console, and has a leg
for the reported case (default settings, 11.6 V: `low_voltage` with the
countdown naming the floor).

## Entry sequence

`sleep.entering {volts}` event (rules get one last MQTT/HTTP shot,
BEFORE radios stop) → wait for autopid idle (≤20 s, only if enabled) →
**main's prepare callback** (`main/main_sleep.c`: autopid,
data_logger, mqtt, vpn, mdns, wifi, ble, external_storage, the
composition root owns the order, so this component depends on none of
them) → CAN transceiver standby (GPIO38) → obd_chip_sleep(true) (since
2026-11-07 the `STSLEEP0` word to the chip first, what 4.x sent at every
sleep, then the sleep pin; the chip's README "The sleep word") → USB
power rail held low (GPIO10, open-drain + hold). Pins via Kconfig
(`WICAN_SLEEP_*_GPIO`, defaults = WiCAN Pro).

**The board-level half on its own (2026-10-05).** The pin writes of the
entry (CAN transceiver to standby, the OBD chip's sleep pin, the USB rail off
and held) are `sleep_manager_board_down()`: public, and usable with nothing
initialised. The sleep entry calls it; so does the firmware's crash park
(`main_park.c`), which runs instead of the whole composition after three
crashes in a row and naps in light sleep on the boot task. One copy of the
sequence: a pin added to the sleep entry is added to the park. A restart
after either gives the pins back the same way (below). Bench, 13.5 V, with
the ESP32 napping: 134 mA with nothing touched, 47 mA after
`obd_chip_park()` and this call, 44 mA in sleep mode proper (which also shut
the SD card and the radios down in order).

**What a wake must give back.** A wake is a reboot, and a pad hold outlives
a reset. `sleep_manager_init()` therefore releases the rail's hold
(`usb_rail_release()`: hold off, pin reset; released, the board's pull-up
has the rail on, as after a power-on) before `usb_host_manager` starts and
drives the pin. Until 2026-10-05 nothing released it (the OBD sleep pin's
hold is released in `obd_pin_wake()`): after the first wake the rail stayed
off until the next power cycle, every USB device on the connector
unpowered, `usb vbus 1` moving nothing and `/api/usb` still saying
`vbus: true`. Bench: 89 mA awake after a wake against 165 mA after a power
cycle; the firmware's VBUS switch worth 76 mA after a power cycle and 0
after a wake. Every sleep scenario had passed over it, because a wake was
judged as "more current than asleep"; the sleep matrix now holds every wake
against the current after a power cycle (`back_to_awake()`), and that is the
check to keep green when the entry sequence gains another latched pin.

**The activity hold (2026-11-06, store and forward).** A car back home after
a drive has its engine off, so the sleep countdown runs while the Home
Assistant integration pulls the drive's rows from the SD card through
`/api/logger/export` (and while the dashboard reads its history). The
composition root hands data_logger's reader hook to
`sleep_manager_activity(seconds)`: every read that carried rows pushes both
counting deadlines out to now + 60 s where that is later, and never past
the cap the first such read of a countdown set (that read + 10 min,
`SM_ACTIVITY_CAP_MS`): a reader that never stops cannot hold a flat battery
awake for good. Pure `sm_activity_apply()` (host-tested: the push, the cap,
only what counts, the clock wrap), edited under the same lock as the hold,
the tracker reset with it once nothing counts. Not a setting, never saved,
counts against nothing (the three holds stay the person's). One I line per
countdown (the reads come every second during a backfill). Surfaces:
`activity_s` on `GET /api/sleep` (seconds left of the extension, 0 = none),
a `held awake while the log is read` line in `sleep`, the web UI's countdown
bar says "Kept awake while the log is being read" and the Power Saving page
has a note. **The OBD chip firmware update holds the same way (Ali,
2026-11-08: no sleep while the chip is updated):** the state task asks
`obd_chip_update_in_progress()` once a lap and applies the activity hold
while it is true (the Quick Setup prompt, the Maintenance card and the
console all run the same job; the flag covers the stream and the chip's
fresh bring-up after it), one I line per update; `activity_why` on the
status (`log` or `chip_update`, "" with none) says which of the two gave
the extension, the bar reads "Kept awake while the OBD chip firmware is
updated". The entry sequence still waits for a running job
(`SM_FW_UPDATE_WAIT_MS`), so a countdown that was already due when the
job started does not pull the chip's pin under a half-written image. The store-and-forward bench
(`tools/testbench/ha/ha_history_backfill_bench.py`) holds the device at
12.5 V while the integration pulls and reads the deadline move.

## Bench safety (meatpi 2026-07-07, mandatory)

- The state task arms only after a **15 s boot grace**: a bootloop
  still leaves a flash window.
- All sleeping is **timer-woken light sleep**: there is no state
  only an external signal can leave.
- `sleep test <secs>` (CLI) / `sleep_manager_test_sleep()` force the
  full entry sequence with a timed wake: the USB-powered bench unit
  (which never sees a "recovered" voltage) always comes back.

## Settings (`sleep_manager`, v4, reboot-to-apply)

`enabled` (**true**: shipping default ON since 2026-07-18, 5 min delay:
a parked device must not drain the car battery out of the box),
`sleep_mv` (12000–14000, 13100), `wake_mv` (12100–15000, 13200; v3,
2026-10-01), `wake_delay_ms` (100–5000, 500; v4, 2026-10-01: "wake up
after", how long the battery must stay above `wake_mv` before the wake
reboot; was a fixed 1 s; the Power Saving page shows it in seconds),
`sleep_delay_min` (1–30, 5), `periodic_wakeup` (false),
`wakeup_interval_min` (5–1440, 30), `cli` (true).

The wake voltage used to be derived (sleep + 0.1 V, legacy). Since v3 it
is the user's own number, measured on the car by Quick Setup's "Battery
and sleep" step (charging voltage with the engine running, resting
voltage after key-off) or set on the Power Saving page. The pure
`sm_resolve_thresholds()` pulls a wake value that is not at least
0.1 V above sleep up to that band (one W line at boot); `on_migrate`
gives a v1/v2 document `wake_mv = sleep_mv + 100` (`sm_migrated_wake_mv()`),
so a configured device keeps its behaviour. autopid's "pause at the sleep
voltage" resumes polling at this wake voltage (one pair rules both).

## Surfaces

- `GET /api/sleep`: `{enabled, state, voltage, sleep_v, wake_v,
  naps, pending, sleep_in_s, critical_v, critical_s, hold_s, holds_left,
  holds_max, activity_s}`. `pending` is
  `none`, `delay` (the sleep delay runs) or `critical` (the floor runs,
  with sleep disabled too); `sleep_in_s` is the seconds to that entry
  (0 with `none`, and 0 while the entry sequence runs); `critical_v` /
  `critical_s` are the floor's fixed 11.90 V and 300 s, so the UI does
  not hard-code them; `hold_s` / `holds_left` / `holds_max` are the hold
  (below). The web UI's countdown bar and its sleep-off warning read this
  route.
- `POST /api/sleep/hold {"minutes":1..30}` (no body = 10): the Keep awake
  button; answers the status after the hold, 400 out of range, 409 when
  nothing is counting or the three holds of the boot are used.
- CLI `sleep` (status, with a `sleeps in N s (...)` line while a
  countdown runs and a `held awake on request` line while a hold runs) /
  `sleep hold <min>` / `sleep test <secs>`.
- Events: `sleep.entering {volts}`, `sleep.state {state, volts}`.
- dev_status: sets/clears WAKE_VOLTAGE_OK continuously; AWAKE→SLEEP
  on entry.

## Footprint

PSRAM: 4 KB task stack. Internal: FreeRTOS objects only. Host tests:
`host_test/` (23 tests: the pure ladder incl. clock wrap, the wake hold
following `wake_delay_ms`, the critical floor tracker, the v3 threshold
resolver, the v2 -> v3 and v3 -> v4 migration values, and since
2026-10-06 the countdown: nothing on a healthy battery or once asleep,
the sleep delay in whole seconds, the floor ahead of a longer delay, a
shorter delay ahead of the floor, the floor alone with sleep disabled,
both deadlines across the clock wrap, the published word read later
than it was written, across its own 30-bit wrap, and the hold on both
deadlines, on what counts only, across the wrap). Bench:
`sleep test` smoke (entry sequence + naps + POWER_WAKE reboot);
`tools/testbench/sleep/sleep_bench_test.py` (`.\test.ps1 sleep`) walks
the supply across sleep and wake, including a wake-voltage leg (a step
between sleep and wake must NOT wake the device) that also times the wake
with a 5 s hold against the default 0.5 s.
