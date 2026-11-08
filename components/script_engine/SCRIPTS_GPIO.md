# Paste into the scripts page (bindings + limits)

Bindings to add:

| Group | Binding | What |
| --- | --- | --- |
| Raw CAN | `can_start()`, `can_stop()` | start or park the CAN controller and its transceiver. `can_stop()` frees the TX and RX pins. The bus is started again when the script ends. `can_start()` is false when CAN is disabled in settings. |
| Raw CAN | `can_rx(id, timeout_ms[, ext])` | one frame with that id, as a hex string, or `nil`. `timeout_ms` is milliseconds (`1000` = 1 s, at most 60 s). Sets `can_rx_id`, `can_rx_ext`, `can_rx_dlc`. |
| GPIO | `digitalWrite(pin, HIGH\|LOW)` | drive a pin. `HIGH` is 1, `LOW` is 0. `pin` is a GPIO number or `CAN_TX` / `CAN_RX`. |
| GPIO | `digitalPWM(pin, ontime_ms, offtime_ms[, rounds])` | pulse the pin, timed by the RMT peripheral. Times are milliseconds and may be fractional (`1.6`, `0.4`). `rounds` defaults to 1. `CAN_TX` and `CAN_RX` work after `can_stop()`. |

Limits to add:

| One `can_rx` wait | up to 60 s |
| One `digitalPWM` | `rounds * (ontime + offtime)` up to 1 s, at most 49104 rounds |
| `digitalPWM` timing | 0.1 us steps for levels up to 3.2 ms, 1 us up to 32 ms, 10 us up to 300 ms |
| Pins | not flash, PSRAM, USB, strapping, or the transceiver standby pin. CAN TX/RX only while the bus is stopped. Pins go back to inputs when the script ends. |

`digitalPWM` hands the pulse train to the RMT peripheral, which drives every edge in hardware. The call waits until the train has been sent; WiFi, BLE and the rest keep running. More than 1023 rounds are sent as a block of 48 pulses repeated by the hardware, plus a short remainder, with a gap of a few microseconds between the two parts.
