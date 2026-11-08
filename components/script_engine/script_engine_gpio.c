/**
 * @file script_engine_gpio.c
 * @brief Pin checks and the blocking pulse generator for digitalWrite /
 *        digitalPWM. The pulse loop runs from IRAM with interrupts off.
 */
#include "script_engine_gpio.h"

#include <stddef.h>

#ifdef ESP_PLATFORM

#include "sdkconfig.h"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "rom/ets_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/gpio_struct.h"

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

bool se_pwm_span_ok(double on_ms, double off_ms, int rounds,
                    uint32_t *on_us, uint32_t *off_us, const char **err)
{
    if (rounds < 1)
    {
        *err = "rounds";
        return false;
    }

    if (!(on_ms > 0.0) || !(off_ms > 0.0) || on_ms > 100.0 || off_ms > 100.0)
    {
        *err = "time";
        return false;
    }

    double on = on_ms * 1000.0;
    double off = off_ms * 1000.0;

    if (on < 1.0 || off < 1.0)
    {
        *err = "time";
        return false;
    }

    uint64_t total = (uint64_t)rounds * ((uint64_t)(on + 0.5) + (uint64_t)(off + 0.5));

    if (total > 100000ULL)
    {
        *err = "cap";
        return false;
    }

    *on_us = (uint32_t)(on + 0.5);
    *off_us = (uint32_t)(off + 0.5);
    return true;
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

static inline void IRAM_ATTR pin_level(int pin, int level)
{
    if (pin < 32)
    {
        if (level)
        {
            GPIO.out_w1ts = (1U << pin);
        }
        else
        {
            GPIO.out_w1tc = (1U << pin);
        }
    }
    else
    {
        if (level)
        {
            GPIO.out1_w1ts.val = (1U << (pin - 32));
        }
        else
        {
            GPIO.out1_w1tc.val = (1U << (pin - 32));
        }
    }
}

static void IRAM_ATTR pulse_loop(int pin, uint32_t on_us, uint32_t off_us,
                                 uint32_t rounds)
{
    uint32_t per_us = esp_rom_get_cpu_ticks_per_us();

    if (per_us == 0)
    {
        per_us = 1;
    }

    uint32_t on_c = on_us * per_us;
    uint32_t off_c = off_us * per_us;
    uint32_t t = esp_cpu_get_cycle_count();

    for (uint32_t i = 0; i < rounds; i++)
    {
        pin_level(pin, 1);
        t += on_c;

        while ((int32_t)(esp_cpu_get_cycle_count() - t) < 0)
        {
        }

        pin_level(pin, 0);
        t += off_c;

        while ((int32_t)(esp_cpu_get_cycle_count() - t) < 0)
        {
        }
    }
}

#if !defined(CONFIG_FREERTOS_UNICORE) && (CONFIG_FREERTOS_NUMBER_OF_CORES > 1)

static volatile int s_hold;
static TaskHandle_t s_halt_task;

static void halt_task(void *arg)
{
    (void)arg;

    for (;;)
    {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        portDISABLE_INTERRUPTS();
        s_hold = 1;

        while (s_hold == 1)
        {
        }

        portENABLE_INTERRUPTS();
    }
}

static void halt_other_core(void)
{
    if (s_halt_task == NULL)
    {
        int other = xPortGetCoreID() ^ 1;

        if (xTaskCreatePinnedToCore(halt_task, "gpio_halt", 2048, NULL,
                                    configMAX_PRIORITIES - 1, &s_halt_task,
                                    other) != pdPASS)
        {
            ESP_LOGW(TAG, "other core not halted");
            s_halt_task = NULL;
            return;
        }
    }

    s_hold = 0;
    xTaskNotifyGive(s_halt_task);

    while (s_hold != 1)
    {
    }
}

static void release_other_core(void)
{
    s_hold = 0;
}

#else

static void halt_other_core(void) {}
static void release_other_core(void) {}

#endif

void se_gpio_write(int pin, int level)
{
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)pin, level ? 1 : 0);
}

void se_gpio_pwm(int pin, uint32_t on_us, uint32_t off_us, uint32_t rounds)
{
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)pin, 0);

    (void)esp_task_wdt_reset();
    halt_other_core();
    portDISABLE_INTERRUPTS();
    pulse_loop(pin, on_us, off_us, rounds);
    portENABLE_INTERRUPTS();
    release_other_core();
    (void)esp_task_wdt_reset();

    gpio_set_level((gpio_num_t)pin, 0);
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

void se_gpio_pwm(int pin, uint32_t on_us, uint32_t off_us, uint32_t rounds)
{
    (void)pin;
    (void)on_us;
    (void)off_us;
    (void)rounds;
}

void se_gpio_release_all(void)
{
    s_n_touched = 0;
}

#endif
