/**
 * @file test_diag.c
 * @brief Host tests for the link diagnostics' pure layer (2026-11-08): the
 *        disconnect reason names, the address and window words, and the
 *        session summary line.
 */
#include <string.h>

#include "unity.h"

#include "ble_manager_private.h"

static void test_diag_reason_names(void)
{
    TEST_ASSERT_EQUAL_STRING("the peer ended it", blm_diag_reason_name(0x213));
    TEST_ASSERT_EQUAL_STRING("supervision timeout (radio link lost)",
                             blm_diag_reason_name(0x208));
    TEST_ASSERT_EQUAL_STRING("the device ended it", blm_diag_reason_name(0x216));
    TEST_ASSERT_EQUAL_STRING("connection failed to be established",
                             blm_diag_reason_name(0x23E));
    TEST_ASSERT_EQUAL_STRING("key missing on the peer (stale bond)",
                             blm_diag_reason_name(0x206));
    TEST_ASSERT_EQUAL_STRING("MIC failure (encryption)", blm_diag_reason_name(0x23D));
    TEST_ASSERT_EQUAL_STRING("other HCI reason", blm_diag_reason_name(0x2A0));
    TEST_ASSERT_EQUAL_STRING("no reason given", blm_diag_reason_name(0));
    TEST_ASSERT_EQUAL_STRING("host error", blm_diag_reason_name(13)); /* BLE_HS_ETIMEOUT */
}

static void test_diag_addr_and_window(void)
{
    char out[64];
    const uint8_t le[6] = { 0x3E, 0x65, 0x5A, 0x8F, 0xEE, 0x68 }; /* NimBLE order */

    blm_diag_format_addr(le, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("68:EE:8F:5A:65:3E", out);
    blm_diag_format_addr(NULL, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("unknown", out);

    TEST_ASSERT_EQUAL_STRING("public", blm_diag_addr_type_name(0));
    TEST_ASSERT_EQUAL_STRING("random", blm_diag_addr_type_name(1));
    TEST_ASSERT_EQUAL_STRING("random identity", blm_diag_addr_type_name(3));
    TEST_ASSERT_EQUAL_STRING("unknown type", blm_diag_addr_type_name(9));

    /* 24 units = 30 ms, 72 units = 720 ms (a phone's defaults); 12 = 15 ms */
    blm_diag_format_window(24, 0, 72, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("interval 30.00 ms latency 0 timeout 720 ms", out);
    blm_diag_format_window(12, 4, 400, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("interval 15.00 ms latency 4 timeout 4000 ms", out);
    blm_diag_format_window(6, 0, 42, out, sizeof(out)); /* 7.5 ms */
    TEST_ASSERT_EQUAL_STRING("interval 7.50 ms latency 0 timeout 420 ms", out);
}

static void test_diag_summary_full_session(void)
{
    blm_diag_session_t s =
    {
        .duration_ms = 83200, .secured_at_ms = 1842, .first_write_at_ms = 2310,
        .last_write_at_ms = 82800, .rx_writes = 412, .rx_bytes = 3214,
        .tx_notifications = 1380, .tx_bytes = 25160, .sub_drops = 0,
        .tx_drops = 2, .subscribed = true,
    };
    char out[240];

    int n = blm_diag_format_summary(&s, out, sizeof(out));

    TEST_ASSERT_TRUE(n > 0 && n < (int)sizeof(out));
    TEST_ASSERT_EQUAL_STRING(
        "83.2 s, secured 1.8 s, first write 2.3 s, last write 0.4 s before end, "
        "pipe subscribed, app->dev 412 wr 3214 B, dev->app 1380 ntf 25160 B, "
        "drops sub 0 tx 2", out);
    /* the whole console line (level, time, tag, the reason) must fit the log
       manager's 256 B line: the body stays well under 160 */
    TEST_ASSERT_TRUE(n < 160);
}

static void test_diag_summary_nothing_happened(void)
{
    /* a central that connected, paired nothing, wrote nothing, left */
    blm_diag_session_t s =
    {
        .duration_ms = 4100, .secured_at_ms = BLM_DIAG_NEVER,
        .first_write_at_ms = BLM_DIAG_NEVER, .last_write_at_ms = BLM_DIAG_NEVER,
    };
    char out[240];

    blm_diag_format_summary(&s, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(
        "4.1 s, never secured, no app write, pipe not subscribed, app->dev 0 wr 0 B, "
        "dev->app 0 ntf 0 B, drops sub 0 tx 0", out);
}

void run_diag_tests(void)
{
    RUN_TEST(test_diag_reason_names);
    RUN_TEST(test_diag_addr_and_window);
    RUN_TEST(test_diag_summary_full_session);
    RUN_TEST(test_diag_summary_nothing_happened);
}
