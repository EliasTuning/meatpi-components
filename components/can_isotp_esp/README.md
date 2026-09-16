# can_isotp_esp — the public build's native ISO-TP provider

`can_manager` owns the TWAI bus and offers an ISO-TP (ISO 15765-2) **provider
slot** (`can_isotp.h`): whoever registers a `can_isotp_ops_t` there serves
`uds_manager`'s "isotp" transport and the J2534 ISO15765 channel. Until
2026-09-16 only the internal add-on pack registered one, so the public
firmware ran UDS through the OBD chip's AT commands and could not bind a
J2534 ISO15765 channel at all (`TASK_isotp_public.md` §0).

This component is the public provider: the vendored `esp_isotp` stack in its
**externally-managed mode** over `can_manager`'s shared bus.

## Design

```
 consumer task            can_core_rx task              "isotp" task (prio 9, PSRAM stack)
 (uds / j2534)                 |                                |
  open()  ---- subscribe rx_id ->  s_frame_q  <---------------- xQueueReceive
  send()  -> esp_isotp_send ----> FF/SF out via can_manager_send |
             (under s->lock)                                      feed_frame(): esp_isotp_feed_can_frame
  recv()  <- s->rx_sem + mailbox <---------------- on_rx_pdu() <-  + esp_isotp_poll (CFs, FC, N_Bs/N_Cr)
  close() -> unsubscribe, delete transport
```

- **One frame queue, one subscription per session** on the session's exact
  rx id (`can_manager_subscribe_queue(filter=rx_id, mask=0x7FF|0x1FFFFFFF)`).
  Every other bus client keeps getting its own copy of every frame: the CAN
  monitor, bridges and autopid (on the MIC chip) do not change while a
  session is open.
- **The task owns the stack.** `esp_isotp` has no locking, so every
  `esp_isotp_*` call on a session runs under the session mutex; the task
  feeds + polls under it, the consumer's `send()` takes it for the
  `esp_isotp_send` call only. Sessions are looked up under the table mutex,
  which the task holds while feeding, so `close()` cannot free a session
  the task is inside.
- **Polling cadence:** idle = a wake every 100 ms; after any activity a
  session is polled every tick for 300 ms (longer than the stack's N_Bs/N_Cr
  of 100 ms), and for as long as a multi-frame send is in flight. This is
  what guarantees the stack always gets to flag a stalled transfer, so a
  link is never left "in progress" after a timeout.
- **Mailbox** (`can_isotp_esp_mbox.c`, pure, host-tested): a ring of 4 PDUs
  per session in PSRAM; the consumer's `recv()` waits on a counting
  semaphore paired with every put that grows the ring. Full → oldest
  dropped (the newest reply wins). `recv()` into a buffer too small returns
  `ESP_ERR_NO_MEM` and has CONSUMED the PDU — the `can_isotp.h` contract the
  UDS transport drains stale traffic on.
- **Memory:** nothing until the first `open()`: then the task (4 KB PSRAM
  stack, static) and the frame queue (2 KB PSRAM, static); per session
  ~8.3 KB (esp_isotp tx/rx buffers, PSRAM) + 16.5 KB mailbox (PSRAM) + a
  few hundred bytes internal for the semaphores.
- **Registration:** `can_isotp_esp_register()` is called by main right
  after `ext_manager_init` and registers ONLY when `can_isotp()` is still
  NULL — a build carrying the add-on pack keeps the pack's provider (the
  slot is single-writer). It also registers the `esp_isotp` log tag at WARN
  (the stack logs every send at INFO).

## Bus sharing with the OBD chip (autopid)

The WiCAN Pro has two requesters on one bus: the MIC chip (autopid, apps)
and the ESP side (these sessions). `obd_gate` serializes the conversations:
the chip already holds the gate around every command; the two consumers of
this provider hold it around theirs — the UDS isotp transport per
request→final response, the J2534 ISO15765 channel from `WRITE_MSGS` until
`READ_MSGS` delivers the reply (or the 2 s hold self-expires). The gate is
fail-open (3 s wait, then take), so a wedged side can never brick the other.

## Limits (esp_isotp 0.1.1)

- The flow control THIS side sends as a receiver uses the Kconfig defaults
  (`CONFIG_ISO_TP_DEFAULT_BLOCK_SIZE` 8, `CONFIG_ISO_TP_DEFAULT_ST_MIN_US`
  1000): `can_isotp_cfg_t.block_size/stmin_ms` are accepted and ignored
  (logged once). Both consumers pass 0/0 today.
- N_Bs / N_Cr = `CONFIG_ISO_TP_DEFAULT_RESPONSE_TIMEOUT_US` (100 ms).
- One PDU ≤ `CAN_ISOTP_ESP_MAX_PDU` (4128 B, SAE J2534's cap); 5 sessions
  (1 UDS + the 4 J2534 channels); a rx id can have one session at a time.
- 29-bit ids through `ext_id`; padding per session (`use_padding`,
  `padding_byte`).

## Stats

`can_isotp_esp_get_stats()`: sessions open/peak, opens/open_fails,
frames_fed/orphan, pdus_tx/rx, tx_timeouts, rx_dropped (mailbox full),
rx_oversize (`recv()` cap too small).

## Bench results (2026-09-16, public build)

- `j2534_transport_probe.py`: raw CAN PASS, ISO15765 bind PASS.
- `j2534_bench.py --reflash --tx 7E2 --rx 7EA` vs `pcan_reflash_ecu.py --scenario
  happy --req 7E2 --resp 7EA`: J2534 REFLASH PASS (10 02, seed/key, the multi-frame 34 -> 74, the 31 01 erase routine).
- `uds_route_bench.py --expect-backend isotp`, autopid running: 20/20 on 7E0
  (median 22 ms) and 20/20 on the same ECU autopid floods (7E4, 28 ms), 0
  corrupt; autopid kept ~11 polls/s; `/api/can` counters unaffected.
- `isotp` task high-water 3296 B free of 4096 after the reflash leg; PSRAM
  min_free 3.89 MB with a session open.

## Testing

- Host: `host_test/` (the mailbox ring) — `run_host_tests.sh can_isotp_esp`.
- Bench (public build, DUT + ECU simulator; `TESTING.md`):
  `tools/testbench/obd/uds_route_bench.py --expect-backend isotp`,
  `tools/testbench/usb/j2534_transport_probe.py` (raw CAN + ISO15765 bind),
  `tools/testbench/usb/j2534_bench.py --reflash` against
  `actors/pcan_reflash_ecu.py`, and `autopid_matrix_bench.py` for
  coexistence.
