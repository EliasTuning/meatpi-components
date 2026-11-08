#include "unity.h"

#include "script_engine_gpio.h"

void test_gpio_denylist(void)
{
    TEST_ASSERT_TRUE(se_gpio_denied(0));
    TEST_ASSERT_TRUE(se_gpio_denied(26));
    TEST_ASSERT_TRUE(se_gpio_denied(19));
    TEST_ASSERT_TRUE(se_gpio_denied(38)); /* transceiver standby */
    TEST_ASSERT_TRUE(se_gpio_denied(-1));
    TEST_ASSERT_TRUE(se_gpio_denied(49));
    TEST_ASSERT_FALSE(se_gpio_denied(2));
    TEST_ASSERT_FALSE(se_gpio_denied(1));
    TEST_ASSERT_FALSE(se_gpio_denied(4));
}

void test_gpio_can_alias_needs_stop(void)
{
    TEST_ASSERT_EQUAL_INT(2, se_can_tx_gpio());
    TEST_ASSERT_EQUAL_INT(1, se_can_rx_gpio());
    TEST_ASSERT_NULL(se_gpio_check(se_can_tx_gpio(), false));
    TEST_ASSERT_NOT_NULL(se_gpio_check(se_can_tx_gpio(), true));
    TEST_ASSERT_NOT_NULL(se_gpio_check(se_can_rx_gpio(), true));
    TEST_ASSERT_NULL(se_gpio_check(4, true));
    TEST_ASSERT_NOT_NULL(se_gpio_check(26, false));
}

void test_pwm_span(void)
{
    uint32_t on = 0;
    uint32_t off = 0;
    const char *err = NULL;

    TEST_ASSERT_TRUE(se_pwm_span_ok(1.6, 0.4, 1, &on, &off, &err));
    TEST_ASSERT_EQUAL_UINT32(1600, on);
    TEST_ASSERT_EQUAL_UINT32(400, off);

    /* 50 * (1.6 + 0.4) ms = 100 ms, the cap */
    TEST_ASSERT_TRUE(se_pwm_span_ok(1.6, 0.4, 50, &on, &off, &err));
    TEST_ASSERT_FALSE(se_pwm_span_ok(1.6, 0.4, 51, &on, &off, &err));
    TEST_ASSERT_EQUAL_STRING("cap", err);

    TEST_ASSERT_FALSE(se_pwm_span_ok(1.6, 0.4, 0, &on, &off, &err));
    TEST_ASSERT_EQUAL_STRING("rounds", err);

    TEST_ASSERT_FALSE(se_pwm_span_ok(0, 0.4, 1, &on, &off, &err));
    TEST_ASSERT_EQUAL_STRING("time", err);

    TEST_ASSERT_FALSE(se_pwm_span_ok(0.0001, 0.4, 1, &on, &off, &err));
    TEST_ASSERT_EQUAL_STRING("time", err);
}

void test_gpio_touch_release(void)
{
    se_gpio_release_all();
    se_gpio_touch(4);
    se_gpio_touch(4);
    se_gpio_touch(5);
    TEST_ASSERT_EQUAL_INT(2, se_gpio_touched_count());
    se_gpio_release_all();
    TEST_ASSERT_EQUAL_INT(0, se_gpio_touched_count());
}
