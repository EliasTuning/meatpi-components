/**
 * @file script_engine_gpio.c
 * @brief Pin checks for digitalWrite / digitalPWM and the RMT pulse sender.
 *        digitalPWM is timed by the RMT peripheral: the CPU only loads the
 *        symbols and waits, the hardware drives every edge.
 */
#include "script_engine_gpio.h"

#include <stddef.h>

#ifdef ESP_PLATFORM

#include "sdkconfig.h"

#include "driver/gpio.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"

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
                       int wait_ms)
{
    rmt_transmit_config_t tx = { .loop_count = loops };

    tx.flags.eot_level = 0;

    if (rmt_transmit(ch, enc, sym, n * sizeof(rmt_symbol_word_t), &tx) != ESP_OK)
    {
        return false;
    }

    return rmt_tx_wait_all_done(ch, wait_ms) == ESP_OK;
}

bool se_gpio_pwm(int pin, const se_pwm_plan_t *plan)
{
    gpio_set_level((gpio_num_t)pin, 0);
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
            sym[i].level0 = 1;
            sym[i].duration0 = plan->on_ticks;
            sym[i].level1 = 0;
            sym[i].duration1 = plan->off_ticks;
        }

        ok = true;

        if (left <= RMT_LOOP_MAX)
        {
            /* one symbol, repeated by the hardware loop counter */
            ok = send_train(ch, enc, sym, 1, (int)left, wait_ms);
            left = 0;
        }
        else
        {
            /* a full block as one train; the loop counter repeats the block */
            uint32_t loops = left / RMT_BLOCK_SYMBOLS;

            ok = send_train(ch, enc, sym, RMT_BLOCK_SYMBOLS, (int)loops, wait_ms);
            left -= loops * RMT_BLOCK_SYMBOLS;

            if (ok && left > 0)
            {
                ok = send_train(ch, enc, sym, left, 1, wait_ms);
            }
        }

        (void)rmt_disable(ch);
    }

    if (enc != NULL)
    {
        (void)rmt_del_encoder(enc);
    }

    (void)rmt_del_channel(ch);

    /* the channel gave the pad back: drive it low ourselves */
    gpio_set_level((gpio_num_t)pin, 0);
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
    return ok;
}

void se_gpio_release_all(void)
{
    for (int i = 0; i < s_n_touched; i++)
    {
        gpio_num_t pin = (gpio_num_t)s_touched[i];

        gpio_set_level(pin, 0);
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
