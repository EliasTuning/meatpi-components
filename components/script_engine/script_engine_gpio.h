#pragma once

#include <stdbool.h>
#include <stdint.h>

/** One digitalPWM() call may run up to one second. */
#define SE_PWM_MAX_US     1000000U
/** 48 symbols x 1023 hardware loops (the S3 RMT loop counter limit). */
#define SE_PWM_MAX_ROUNDS 49104

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

/** The RMT timing of one digitalPWM() call. */
typedef struct
{
    uint32_t res_hz;     /**< RMT tick rate: 10 MHz, 1 MHz or 100 kHz.   */
    uint32_t on_ticks;   /**< high time in ticks (1..32767).              */
    uint32_t off_ticks;  /**< low time in ticks (1..32767).               */
    uint32_t rounds;
    uint32_t total_us;   /**< rounds * (on + off), for the wait timeout.  */
    int      idle;       /**< pin level between and after the pulses; the
                              pulse (ontime) is the opposite level.        */
} se_pwm_plan_t;

/** Idle level when the script gives none: HIGH on CAN_TX (recessive, so the
 *  transceiver's dominant timeout never trips), LOW elsewhere. */
int se_pwm_default_idle(int pin);

/**
 * on/off are milliseconds. Picks the finest tick that fits both levels in
 * the 15-bit RMT duration (0.1 us up to 3.2 ms, 1 us up to 32 ms, 10 us up
 * to 300 ms). false sets @p err to "rounds", "time" or "cap" (total longer
 * than one second).
 */
bool se_pwm_plan(double on_ms, double off_ms, int rounds,
                 se_pwm_plan_t *plan, const char **err);

void se_gpio_touch(int pin);
void se_gpio_release_all(void);
int  se_gpio_touched_count(void);

void se_gpio_write(int pin, int level);

/** Blocks until the pulses have been sent. CAN_TX is bit-banged; other pins
 *  use the RMT. false: no RMT channel free or the transmission failed. */
bool se_gpio_pwm(int pin, const se_pwm_plan_t *plan);
