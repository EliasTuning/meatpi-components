# obd_gate

The vehicle-bus conversation gate (policy component). The WiCAN Pro has
TWO OBD requesters on the SAME physical CAN bus: the MIC3624 chip
(`obd_chip`, driven by apps over BLE/TCP/WS/USB) and any ESP-side
requester on the TWAI controller (an add-on pack's virtual ELM jacks).
Both address the ECU the same way and both hear every
response — when two request/response conversations OVERLAP, a requester
attributes the other's response to its own request. Bench-measured with
the gate off: 12 cross-attributed responses in 16 s of concurrent
polling (a driving app + autopid = bad data, meatpi 2026-07-11).

The gate serializes those conversations: ONE holder at a time, keyed by
an opaque owner pointer (the chip, or one engine instance).

## Why its own component (not files in obd_chip)

Asked 2026-09-16. `obd_chip` is a **driver** (the MIC3624 over UART: the HAL
end of the standard's `core <- services <- app` rule). The gate is **policy**
over the one physical bus, arbitrating between that driver and requesters
that never touch the chip: autopid's and the add-on pack's engines,
uds_manager's native ISO-TP transport, the J2534 ISO15765 channel, the
script engine's bindings. Policy inside the driver would make every
native-CAN feature depend on the UART chip driver and would make a lower
layer know about the higher ones - section 3 forbids both ("a lower layer
never depends on a higher one"; "extract the shared part into a lower
component"). The 2026-09-16 diagnostics hold showed the second reason
concretely: autopid depends on uds_manager, so uds_manager pausing autopid
directly would be a cycle - the gate is the lower component both point down
at. Cost: four small files with their own host suite (14 tests).

## Semantics (fail-open by design)

- `obd_gate_acquire(owner, wait_ms)` blocks (10 ms poll) while another
  owner holds the gate, then TAKES it anyway (counted `steals` + a
  throttled WARN) — a wedged holder can never brick the other side.
- A hold auto-expires after `OBD_GATE_HOLD_MS` (2 s): a holder that never
  releases (e.g. a monitor command with no `'>'` prompt) self-clears.
- Re-acquire by the current holder extends the hold (multi-step
  conversations stay owned).
- **Fairness**: the first owner refused while the gate is held becomes
  the WAITER and owns the next turn — without this, a tight requester
  loop starves a 10 ms poller (measured live: the chip's TCP client
  re-won the gate 14 consecutive cycles). The reservation lapses
  `OG_RESERVE_MS` (500 ms) after the waiter's last poll.
- Disabled → every call is a no-op.

## Hook points

- **obd_chip**: `obd_chip_send()` acquires when the buffer carries a CR
  (a command submission); `obd_chip_request()` brackets its transaction.
  Released when the RX fan-out sees the `'>'` prompt at line start (or
  by hold expiry — monitor-class commands).
- **ESP-side engines** (add-on pack jacks): optional
  `gate_acquire/gate_release/gate_ctx` callbacks in the engine config —
  acquire just before an OBD request is transmitted on CAN, release when
  the request's response window ends
  (`obd_gate_engine_acquire/release`, ctx = the engine instance).
- NOT yet gated: direct ISO-TP requesters (uds `isotp` transport,
  j2534 ISO15765 channels) — diagnostic tools, rarely concurrent with
  a driving app; they can adopt `obd_gate_acquire` later.

## Diagnostics hold (the tools' "Exclusive bus" option, 2026-09-16)

A second, independent policy in the same component: an ESP-side diagnostic
tool (the UDS Tool, the J2534 PassThru server, a running script's ECU
bindings) with its Exclusive bus switch
on asks the background bus pollers (autopid: PID polling AND DTC scans) to
stay off the bus while it is in use — not "who speaks next" (the gate above)
but "who may speak at all". `obd_gate_diag_hold(owner, on)` is refcounted by
owner identity (up to 4 holders; pure core `obd_gate_diag.c`, host-tested);
autopid's poller calls `obd_gate_diag_held()` every loop, pauses, and
acknowledges with `obd_gate_diag_ack(true)`; a tool waits for that ack
(`obd_gate_diag_wait_ack`, up to 700 ms — autopid loops within 500 ms) before
its first request, so the bus is really quiet. The ack resets when the last
holder leaves. Independent of the `enabled` setting. Lives here because
autopid depends on uds_manager (a direct call would be a dependency cycle)
and both requesters already depend on this policy component.

## Settings (`obd_gate`, v1, reboot-to-apply)

| key | type | default | |
|---|---|---|---|
| `enabled` | bool | **true** | disable only for benches that WANT concurrent conversations |

## Dependencies

`settings_manager`, `log_manager`, `esp_timer` — the gate sits BELOW
both requester sides; they point down at it (no cycle).

## Memory footprint (measured shape, trivial)

Internal `.bss`: one `og_core_t` (~40 B) + spinlock. No tasks, no heap.

## Tests

- Host suite (`host_test/`, 11 tests): grant/deny, same-owner extension,
  hold-expiry reaping, wrong-owner release, force/steal, NULL args,
  waiter next-turn fairness, reservation expiry, expiry-honors-waiter,
  three-owner serialization.
- Bench (`tools/testbench/obd_gate_bench.py`, `test.ps1` stage
  `live obdgate`): python ECU on the PCAN (0x7E1→0x7E9, 80 ms delayed
  responses = wide conversation windows) + two concurrent ELM clients
  (MIC chip via `obd0`, `elm0` via `slcan0`, PID-disjoint so
  cross-attribution is visible). Asserts gate ON = zero overlapping
  request windows + zero cross-PID data + both sides progress; gate
  OFF = overlaps occur. Bench gotcha baked in: clients set `ATST64`
  (400 ms) — the ELM default response timeout (100 ms) races the ECU
  delay with a 20 ms margin and the gate's turn-taking phase jitter
  eats it.
