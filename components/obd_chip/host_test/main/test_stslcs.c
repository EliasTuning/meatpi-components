/**
 * @file test_stslcs.c
 * @brief Host tests for the STSLCS sleep-config parser + the boot
 *        provisioning policy (the legacy main/obd.c grammar).
 */
#include <string.h>

#include "unity.h"

#include "obd_chip_private.h"

/* the REAL chip output, captured live from the MIC3624 2026-07-04
   (note the column-aligned double spacing) */
static const char SAMPLE[] =
    "CTRL MODE:  NATIVE\r"
    "PWR_CTRL:   LOW PWR = LOW\r"
    "UART_SLEEP: OFF, 1200 s\r"
    "UART_WAKE:  OFF, 0-0 us\r"
    "EXT_INPUT:  LOW = SLEEP\r"
    "EXT_SLEEP:  OFF, LOW, FOR 64 ms\r"
    "EXT_WAKE:   ON, HIGH, FOR 2000 ms\r"
    "VL_SLEEP:   OFF, <13.20V FOR 150 s\r"
    "VL_WAKE:    OFF, >13.50V FOR 1 s\r"
    "VCHG WAKE:  ON, 0.20V IN 1000 ms\r";

void test_stslcs_parse_full_native(void)
{
    obd_stslcs_t c;

    obd_stslcs_parse(SAMPLE, &c);
    TEST_ASSERT_EQUAL_STRING("NATIVE", c.ctrl_mode);
    TEST_ASSERT_EQUAL_INT(0, c.pwr_ctrl); /* SLEEP = LOW */
    TEST_ASSERT_EQUAL_INT(0, c.uart_sleep.en);
    TEST_ASSERT_EQUAL_UINT32(1200, c.uart_sleep.time);
    TEST_ASSERT_EQUAL_INT(0, c.uart_wake.en);
    TEST_ASSERT_EQUAL_INT(1, c.ext_wake.en);
    TEST_ASSERT_EQUAL_UINT32(2000, c.ext_wake.time);
    TEST_ASSERT_EQUAL_INT(0, c.vl_sleep.en);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 13.20f, c.vl_sleep.voltage);
    TEST_ASSERT_EQUAL_UINT32(150, c.vl_sleep.time);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 13.50f, c.vl_wake.voltage);
    TEST_ASSERT_EQUAL_INT(1, c.vchg_wake.en);
}

void test_stslcs_parse_elm327_and_armed_wake(void)
{
    obd_stslcs_t c;

    /* ELM327 control mode + the ">!" tripped-marker VL_WAKE variant */
    obd_stslcs_parse("CTRL MODE: ELM327\r\n"
                     "VL_WAKE: ON, >!13.50V FOR 1 s\r\n", &c);
    TEST_ASSERT_EQUAL_STRING("ELM327", c.ctrl_mode);
    TEST_ASSERT_EQUAL_INT(1, c.vl_wake.en);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 13.50f, c.vl_wake.voltage);
}

void test_stslcs_provision_policy(void)
{
    obd_stslcs_t c;

    /* matching config, all controls off -> nothing to do */
    obd_stslcs_parse(SAMPLE, &c);
    TEST_ASSERT_FALSE(obd_stslcs_needs_provision(&c, 13.5f, 13.2f, 150));

    /* any autonomous control ON -> reprovision */
    c.uart_wake.en = 1;
    TEST_ASSERT_TRUE(obd_stslcs_needs_provision(&c, 13.5f, 13.2f, 150));
    c.uart_wake.en = 0;
    c.vl_sleep.en = 1;
    TEST_ASSERT_TRUE(obd_stslcs_needs_provision(&c, 13.5f, 13.2f, 150));
    c.vl_sleep.en = 0;

    /* threshold drift -> reprovision; epsilon tolerates float noise */
    TEST_ASSERT_TRUE(obd_stslcs_needs_provision(&c, 13.6f, 13.2f, 150));
    TEST_ASSERT_TRUE(obd_stslcs_needs_provision(&c, 13.5f, 12.9f, 150));
    TEST_ASSERT_TRUE(obd_stslcs_needs_provision(&c, 13.5f, 13.2f, 630));
    TEST_ASSERT_FALSE(
        obd_stslcs_needs_provision(&c, 13.5001f, 13.2001f, 150));
}

void test_stslcs_ignores_garbage(void)
{
    obd_stslcs_t c;

    obd_stslcs_parse("?\r\nSTSLCS\r\nrandom noise line\r\n", &c);
    TEST_ASSERT_EQUAL_STRING("", c.ctrl_mode);
    TEST_ASSERT_FALSE(obd_stslcs_needs_provision(&c, 0.0f, 0.0f, 0));
}

void test_stslcs_midline_gt_is_not_the_prompt(void)
{
    /* THE live bug: '>' inside "VL_WAKE: OFF, >13.50V" terminated the
       accumulator, truncating STSLCS -> reprovision every boot. Only a
       line-start '>' is the prompt. */
    static const char WIRE[] =
        "VL_SLEEP:   OFF, <13.20V FOR 150 s\r"
        "VL_WAKE:    OFF, >13.50V FOR 1 s\r"
        "VCHG WAKE:  OFF, 0.20V IN 1000 ms\r\r>";
    obd_resp_acc_t acc;
    obd_stslcs_t c;

    obd_parse_reset(&acc);

    size_t consumed = obd_parse_feed(&acc, WIRE, sizeof(WIRE) - 1);

    TEST_ASSERT_TRUE(acc.done);
    TEST_ASSERT_EQUAL(sizeof(WIRE) - 1, consumed); /* the REAL prompt */

    obd_stslcs_parse(acc.buf, &c);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 13.50f, c.vl_wake.voltage);
    TEST_ASSERT_FALSE(obd_stslcs_needs_provision(&c, 13.5f, 13.2f, 150));
}

/* VTPPSWS, the vendor's PowerEn switch (2026-11-07): the bench chip's "10"
   as the probe read it ('10..>' = "10\r\r>"), the fault the sleep word
   bench injects ("0F"), the echo of a bring-up before ATE0, stray bytes
   ahead of a reset's answer, and what is not a value. */
void test_ppsw_parse(void)
{
    uint8_t v = 0;

    TEST_ASSERT_TRUE(obd_parse_ppsw("10", &v));
    TEST_ASSERT_EQUAL_HEX8(0x10, v);
    TEST_ASSERT_TRUE(obd_parse_ppsw("10\r\r", &v));
    TEST_ASSERT_EQUAL_HEX8(0x10, v);
    TEST_ASSERT_TRUE(obd_parse_ppsw("0F\r", &v));
    TEST_ASSERT_EQUAL_HEX8(0x0F, v);
    TEST_ASSERT_TRUE(obd_parse_ppsw("VTPPSWS\r01\r", &v)); /* the echo */
    TEST_ASSERT_EQUAL_HEX8(0x01, v);
    TEST_ASSERT_TRUE(obd_parse_ppsw("\r\n 10 \r", &v));
    TEST_ASSERT_EQUAL_HEX8(0x10, v);
    TEST_ASSERT_TRUE(obd_parse_ppsw("a5", &v));
    TEST_ASSERT_EQUAL_HEX8(0xA5, v);
    TEST_ASSERT_FALSE(obd_parse_ppsw("?", &v));
    TEST_ASSERT_FALSE(obd_parse_ppsw("OK", &v));
    TEST_ASSERT_FALSE(obd_parse_ppsw("1", &v));
    TEST_ASSERT_FALSE(obd_parse_ppsw("100", &v));
    TEST_ASSERT_FALSE(obd_parse_ppsw("", &v));
    TEST_ASSERT_FALSE(obd_parse_ppsw("ELM327 v2.3", &v));
    TEST_ASSERT_FALSE(obd_parse_ppsw(NULL, &v));
    TEST_ASSERT_FALSE(obd_parse_ppsw("10", NULL));
}
