#pragma once

#include <stdbool.h>
#include <stdint.h>

/** Configured CAN controller pins (Kconfig on device, GPIO2 / GPIO1 on host). */
int se_can_tx_gpio(void);
int se_can_rx_gpio(void);

/** Flash, PSRAM, USB, strapping and the transceiver standby pin. */
bool se_gpio_denied(int pin);

/**
 * NULL when @p pin may be driven. When the CAN controller is running, its
 * TX and RX pins are refused until can_stop().
 */
const char *se_gpio_check(int pin, bool bus_running);

/**
 * on/off are milliseconds. Fills microseconds. false sets @p err to
 * "rounds", "time" or "cap" (total longer than 100 ms).
 */
bool se_pwm_span_ok(double on_ms, double off_ms, int rounds,
                    uint32_t *on_us, uint32_t *off_us, const char **err);

void se_gpio_touch(int pin);
void se_gpio_release_all(void);
int  se_gpio_touched_count(void);

void se_gpio_write(int pin, int level);
void se_gpio_pwm(int pin, uint32_t on_us, uint32_t off_us, uint32_t rounds);
