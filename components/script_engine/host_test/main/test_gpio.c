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
    se_pwm_plan_t p;
    const char *err = NULL;

    /* 1.6 / 0.4 ms: 0.1 us ticks */
    TEST_ASSERT_TRUE(se_pwm_plan(1.6, 0.4, 1, &p, &err));
    TEST_ASSERT_EQUAL_UINT32(10000000, p.res_hz);
    TEST_ASSERT_EQUAL_UINT32(16000, p.on_ticks);
    TEST_ASSERT_EQUAL_UINT32(4000, p.off_ticks);
    TEST_ASSERT_EQUAL_UINT32(2000, p.total_us);

    /* a level above 3.2 ms moves to 1 us ticks */
    TEST_ASSERT_TRUE(se_pwm_plan(5.0, 1.0, 1, &p, &err));
    TEST_ASSERT_EQUAL_UINT32(1000000, p.res_hz);
    TEST_ASSERT_EQUAL_UINT32(5000, p.on_ticks);
    TEST_ASSERT_EQUAL_UINT32(1000, p.off_ticks);

    /* above 32 ms: 10 us ticks */
    TEST_ASSERT_TRUE(se_pwm_plan(40.0, 1.0, 1, &p, &err));
    TEST_ASSERT_EQUAL_UINT32(100000, p.res_hz);
    TEST_ASSERT_EQUAL_UINT32(4000, p.on_ticks);
    TEST_ASSERT_EQUAL_UINT32(100, p.off_ticks);

    TEST_ASSERT_FALSE(se_pwm_plan(400.0, 1.0, 1, &p, &err));
    TEST_ASSERT_EQUAL_STRING("time", err);

    /* 500 * (1.6 + 0.4) ms = 1 s, the cap */
    TEST_ASSERT_TRUE(se_pwm_plan(1.6, 0.4, 500, &p, &err));
    TEST_ASSERT_FALSE(se_pwm_plan(1.6, 0.4, 501, &p, &err));
    TEST_ASSERT_EQUAL_STRING("cap", err);

    TEST_ASSERT_FALSE(se_pwm_plan(1.6, 0.4, 0, &p, &err));
    TEST_ASSERT_EQUAL_STRING("rounds", err);
    TEST_ASSERT_FALSE(se_pwm_plan(0.002, 0.002, SE_PWM_MAX_ROUNDS + 1, &p, &err));
    TEST_ASSERT_EQUAL_STRING("rounds", err);
    TEST_ASSERT_TRUE(se_pwm_plan(0.002, 0.002, SE_PWM_MAX_ROUNDS, &p, &err));

    TEST_ASSERT_FALSE(se_pwm_plan(0, 0.4, 1, &p, &err));
    TEST_ASSERT_EQUAL_STRING("time", err);

    TEST_ASSERT_FALSE(se_pwm_plan(0.0001, 0.4, 1, &p, &err));
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
