/**
 * @file script_engine_gpio.c
 * @brief Pin checks for digitalWrite / digitalPWM and the pulse sender.
 *        Other pins are timed by the RMT peripheral. CAN_TX is a busy-wait
 *        bitbang: the first thing that drives the pad is the on-level.
 *        An idle write or rmt_new_tx_channel before that edge holds a
 *        constant level through the radar's sample window.
 */
#include "script_engine_gpio.h"

#include <stddef.h>

#ifdef ESP_PLATFORM

#include "sdkconfig.h"

#include "driver/gpio.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

static const char *TAG = "script_gpio";

int se_can_tx_gpio(void) { return CONFIG_WICAN_CAN_TX_GPIO; }
int se_can_rx_gpio(void) { return CONFIG_WICAN_CAN_RX_GPIO; }

static int stdby_gpio(void) { return CONFIG_WICAN_CAN_STDBY_GPIO; }

#else

int se_can_tx_gpio(void) { return 2; }
int se_can_rx_gpio(void) { return 1; }

static int stdby_gpio(void) { return 38; }

#endif

bool se_gpio_denied(int pin)
{
    /* ESP32-S3: flash 26-32, octal PSRAM 33-37, USB 19/20, strapping
     * 0/3/45/46, console UART 43/44. Standby is the transceiver pin. */
    static const int denied[] =
    {
        0, 3, 19, 20,
        26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37,
        43, 44, 45, 46,
    };

    if (pin < 0 || pin > 48 || pin == stdby_gpio())
    {
        return true;
    }

    for (size_t i = 0; i < sizeof(denied) / sizeof(denied[0]); i++)
    {
        if (pin == denied[i])
        {
            return true;
        }
    }

    return false;
}

const char *se_gpio_check(int pin, bool bus_running)
{
    if (se_gpio_denied(pin))
    {
        return "pin refused";
    }

    if (bus_running && (pin == se_can_tx_gpio() || pin == se_can_rx_gpio()))
    {
        return "CAN pin in use: call can_stop() first";
    }

    return NULL;
}

/* One RMT duration is 15 bits. The tick is picked so both levels fit:
 * 0.1 us for levels up to 3.2 ms, else 1 us (32 ms), else 10 us (327 ms). */
#define RMT_MAX_TICKS    32767U
#define PWM_MAX_LEVEL_US 300000.0

bool se_pwm_plan(double on_ms, double off_ms, int rounds,
                 se_pwm_plan_t *plan, const char **err)
{
    static const uint32_t RES[] = { 10000000U, 1000000U, 100000U };

    if (rounds < 1 || rounds > SE_PWM_MAX_ROUNDS)
    {
        *err = "rounds";
        return false;
    }

    double on_us = on_ms * 1000.0;
    double off_us = off_ms * 1000.0;

    if (!(on_us >= 1.0) || !(off_us >= 1.0) ||
        on_us > PWM_MAX_LEVEL_US || off_us > PWM_MAX_LEVEL_US)
    {
        *err = "time";
        return false;
    }

    for (size_t i = 0; i < sizeof(RES) / sizeof(RES[0]); i++)
    {
        double on_t = on_us * RES[i] / 1e6;
        double off_t = off_us * RES[i] / 1e6;

        if (on_t + 0.5 > RMT_MAX_TICKS || off_t + 0.5 > RMT_MAX_TICKS)
        {
            continue;
        }

        uint64_t total_us = (uint64_t)rounds *
                            (uint64_t)(on_us + off_us + 0.5);

        if (total_us > SE_PWM_MAX_US)
        {
            *err = "cap";
            return false;
        }

        plan->res_hz = RES[i];
        plan->on_ticks = (uint32_t)(on_t + 0.5);
        plan->off_ticks = (uint32_t)(off_t + 0.5);
        plan->rounds = (uint32_t)rounds;
        plan->total_us = (uint32_t)total_us;
        return true;
    }

    *err = "time";
    return false;
}

int se_pwm_default_idle(int pin)
{
    return pin == se_can_tx_gpio() ? 1 : 0;
}

#define SE_GPIO_TOUCH_MAX 8

static int s_touched[SE_GPIO_TOUCH_MAX];
static int s_n_touched;

void se_gpio_touch(int pin)
{
    for (int i = 0; i < s_n_touched; i++)
    {
        if (s_touched[i] == pin)
        {
            return;
        }
    }

    if (s_n_touched < SE_GPIO_TOUCH_MAX)
    {
        s_touched[s_n_touched++] = pin;
    }
}

int se_gpio_touched_count(void)
{
    return s_n_touched;
}

#ifdef ESP_PLATFORM

#define RMT_BLOCK_SYMBOLS 48
#define RMT_LOOP_MAX      1023

void se_gpio_write(int pin, int level)
{
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)pin, level ? 1 : 0);
}

static bool send_train(rmt_channel_handle_t ch, rmt_encoder_handle_t enc,
                       const rmt_symbol_word_t *sym, size_t n, int loops,
                       int idle, int wait_ms)
{
    rmt_transmit_config_t tx = { .loop_count = loops };

    tx.flags.eot_level = idle ? 1 : 0;

    if (rmt_transmit(ch, enc, sym, n * sizeof(rmt_symbol_word_t), &tx) != ESP_OK)
    {
        return false;
    }

    return rmt_tx_wait_all_done(ch, wait_ms) == ESP_OK;
}

/* CAN_TX only. Do not write the idle level first, and do not create an
 * RMT channel here: both hold the pad at a constant level for about a
 * millisecond, which is already past the 0.6 ms fall limit. The first
 * drive is the on-level (dominant when idle is high). Each level then
 * lasts the planned time. Interrupts stay enabled; one level is 1.5 ms,
 * and a 300 ms lock is not acceptable. The pad is left an output at idle. */
static void bitbang_can_tx(int pin, const se_pwm_plan_t *plan)
{
    uint32_t on_us = (uint32_t)(((uint64_t)plan->on_ticks * 1000000ULL +
                                 plan->res_hz / 2) / plan->res_hz);
    uint32_t off_us = (uint32_t)(((uint64_t)plan->off_ticks * 1000000ULL +
                                  plan->res_hz / 2) / plan->res_hz);
    int on = plan->idle ? 0 : 1;
    int off = on ? 0 : 1;

    gpio_set_level((gpio_num_t)pin, on);
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);

    for (uint32_t i = 0; i < plan->rounds; i++)
    {
        if (i != 0)
        {
            gpio_set_level((gpio_num_t)pin, on);
        }
        esp_rom_delay_us(on_us);
        gpio_set_level((gpio_num_t)pin, off);
        esp_rom_delay_us(off_us);
    }

    gpio_set_level((gpio_num_t)pin, plan->idle ? 1 : 0);
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
}

bool se_gpio_pwm(int pin, const se_pwm_plan_t *plan)
{
    if (pin == se_can_tx_gpio())
    {
        bitbang_can_tx(pin, plan);
        return true;
    }

    /* Other pins. This idle write and the channel setup below must not
     * move onto CAN_TX: they run before the first edge. */
    gpio_set_level((gpio_num_t)pin, plan->idle ? 1 : 0);
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);

    rmt_tx_channel_config_t cc =
    {
        .gpio_num = (gpio_num_t)pin,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = plan->res_hz,
        .mem_block_symbols = RMT_BLOCK_SYMBOLS,
        .trans_queue_depth = 1,
    };
    rmt_channel_handle_t ch = NULL;

    if (rmt_new_tx_channel(&cc, &ch) != ESP_OK)
    {
        ESP_LOGW(TAG, "no RMT channel");
        return false;
    }

    rmt_copy_encoder_config_t ec = { 0 };
    rmt_encoder_handle_t enc = NULL;
    bool ok = false;

    if (rmt_new_copy_encoder(&ec, &enc) == ESP_OK && rmt_enable(ch) == ESP_OK)
    {
        rmt_symbol_word_t sym[RMT_BLOCK_SYMBOLS];
        int wait_ms = (int)(plan->total_us / 1000U) + 100;
        uint32_t left = plan->rounds;

        for (int i = 0; i < RMT_BLOCK_SYMBOLS; i++)
        {
            sym[i].level0 = plan->idle ? 0 : 1;
            sym[i].duration0 = plan->on_ticks;
            sym[i].level1 = plan->idle ? 1 : 0;
            sym[i].duration1 = plan->off_ticks;
        }

        ok = true;

        if (left <= RMT_LOOP_MAX)
        {
            /* one symbol, repeated by the hardware loop counter */
            ok = send_train(ch, enc, sym, 1, (int)left, plan->idle, wait_ms);
            left = 0;
        }
        else
        {
            /* a full block as one train; the loop counter repeats the block */
            uint32_t loops = left / RMT_BLOCK_SYMBOLS;

            ok = send_train(ch, enc, sym, RMT_BLOCK_SYMBOLS, (int)loops,
                           plan->idle, wait_ms);
            left -= loops * RMT_BLOCK_SYMBOLS;

            if (ok && left > 0)
            {
                ok = send_train(ch, enc, sym, left, 1, plan->idle, wait_ms);
            }
        }

        (void)rmt_disable(ch);
    }

    if (enc != NULL)
    {
        (void)rmt_del_encoder(enc);
    }

    (void)rmt_del_channel(ch);

    /* the channel gave the pad back: hold the idle level ourselves */
    gpio_set_level((gpio_num_t)pin, plan->idle ? 1 : 0);
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
    return ok;
}

void se_gpio_release_all(void)
{
    for (int i = 0; i < s_n_touched; i++)
    {
        gpio_num_t pin = (gpio_num_t)s_touched[i];

        /* CAN TX goes back recessive, not dominant */
        gpio_set_level(pin, (int)pin == se_can_tx_gpio() ? 1 : 0);
        gpio_set_direction(pin, GPIO_MODE_INPUT);
        gpio_reset_pin(pin);
    }

    s_n_touched = 0;
}

#else /* host */

void se_gpio_write(int pin, int level)
{
    (void)pin;
    (void)level;
}

bool se_gpio_pwm(int pin, const se_pwm_plan_t *plan)
{
    (void)pin;
    (void)plan;
    return true;
}

void se_gpio_release_all(void)
{
    s_n_touched = 0;
}

#endif
