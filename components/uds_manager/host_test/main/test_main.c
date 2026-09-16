/**
 * @file test_main.c
 * @brief Host tests for uds_proto: response predicates, NRC decode, and
 *        hex⇄bytes round-trips.
 *        Expected output: 7 Tests 0 Failures 0 Ignored.
 */
#include <string.h>

#include "unity.h"

#include "uds_proto.h"
#include "uds_transport.h"

void setUp(void) {}
void tearDown(void) {}

static void test_positive_response(void)
{
    const uint8_t r[] = { 0x62, 0xF1, 0x90, 0x01, 0x02 };
    TEST_ASSERT_TRUE(uds_is_positive_for(0x22, r, sizeof(r)));
    TEST_ASSERT_FALSE(uds_is_negative(r, sizeof(r)));
    TEST_ASSERT_FALSE(uds_is_pending(r, sizeof(r)));
    TEST_ASSERT_EQUAL_UINT8(0, uds_nrc_of(r, sizeof(r)));
    /* wrong SID doesn't match */
    TEST_ASSERT_FALSE(uds_is_positive_for(0x19, r, sizeof(r)));
}

static void test_negative_response(void)
{
    const uint8_t r[] = { 0x7F, 0x22, 0x31 }; /* requestOutOfRange */
    TEST_ASSERT_TRUE(uds_is_negative(r, sizeof(r)));
    TEST_ASSERT_FALSE(uds_is_positive_for(0x22, r, sizeof(r)));
    TEST_ASSERT_EQUAL_UINT8(0x31, uds_nrc_of(r, sizeof(r)));
    TEST_ASSERT_EQUAL_STRING("requestOutOfRange", uds_nrc_name(0x31));
}

static void test_pending_response(void)
{
    const uint8_t r[] = { 0x7F, 0x31, 0x78 };
    TEST_ASSERT_TRUE(uds_is_negative(r, sizeof(r)));
    TEST_ASSERT_TRUE(uds_is_pending(r, sizeof(r)));
    /* a different NRC is negative but not pending */
    const uint8_t r2[] = { 0x7F, 0x31, 0x33 };
    TEST_ASSERT_FALSE(uds_is_pending(r2, sizeof(r2)));
    TEST_ASSERT_EQUAL_STRING("securityAccessDenied", uds_nrc_name(0x33));
}

static void test_short_and_null_inputs(void)
{
    const uint8_t r[] = { 0x7F, 0x22 }; /* too short for an NRC */
    TEST_ASSERT_FALSE(uds_is_negative(r, sizeof(r)));
    TEST_ASSERT_FALSE(uds_is_negative(NULL, 0));
    TEST_ASSERT_FALSE(uds_is_positive_for(0x22, NULL, 0));
    TEST_ASSERT_EQUAL_STRING("unknown", uds_nrc_name(0x01));
}

static void test_response_matches(void)
{
    /* the answer to OUR request: positive or a negative naming our SID */
    const uint8_t pos[] = { 0x62, 0xF1, 0x90, 0x31 };
    const uint8_t neg[] = { 0x7F, 0x22, 0x31 };
    /* autopid's mode-01 reply landing in a UDS window (bench 2026-09-16) */
    const uint8_t stray[] = { 0x41, 0x0C, 0x0C, 0x80 };
    const uint8_t tp[] = { 0x7E, 0x00 };

    TEST_ASSERT_TRUE(uds_response_matches(0x22, pos, sizeof(pos)));
    TEST_ASSERT_TRUE(uds_response_matches(0x22, neg, sizeof(neg)));
    TEST_ASSERT_TRUE(uds_response_matches(0x3E, tp, sizeof(tp)));
    TEST_ASSERT_FALSE(uds_response_matches(0x3E, stray, sizeof(stray)));
    TEST_ASSERT_FALSE(uds_response_matches(0x22, stray, sizeof(stray)));
    TEST_ASSERT_FALSE(uds_response_matches(0x10, neg, sizeof(neg)));
    TEST_ASSERT_FALSE(uds_response_matches(0x22, NULL, 0));
}

static void test_hex_to_bytes_ok(void)
{
    uint8_t out[8];
    size_t n = 0;

    TEST_ASSERT_TRUE(uds_hex_to_bytes("22F190", out, sizeof(out), &n));
    TEST_ASSERT_EQUAL_size_t(3, n);
    TEST_ASSERT_EQUAL_UINT8(0x22, out[0]);
    TEST_ASSERT_EQUAL_UINT8(0xF1, out[1]);
    TEST_ASSERT_EQUAL_UINT8(0x90, out[2]);

    /* separators + 0x prefix + mixed case */
    TEST_ASSERT_TRUE(uds_hex_to_bytes("0x10 03-1a:F1", out, sizeof(out), &n));
    TEST_ASSERT_EQUAL_size_t(4, n);
    TEST_ASSERT_EQUAL_UINT8(0x10, out[0]);
    TEST_ASSERT_EQUAL_UINT8(0x03, out[1]);
    TEST_ASSERT_EQUAL_UINT8(0x1A, out[2]);
    TEST_ASSERT_EQUAL_UINT8(0xF1, out[3]);
}

static void test_hex_to_bytes_rejects(void)
{
    uint8_t out[2];
    size_t n = 0;

    TEST_ASSERT_FALSE(uds_hex_to_bytes("22F", out, sizeof(out), &n)); /* odd */
    TEST_ASSERT_FALSE(uds_hex_to_bytes("22GG", out, sizeof(out), &n)); /* junk */
    TEST_ASSERT_FALSE(uds_hex_to_bytes("112233", out, sizeof(out), &n)); /* overflow */
}

static void test_bytes_to_hex(void)
{
    const uint8_t b[] = { 0x62, 0xF1, 0x90 };
    char s[16];

    size_t n = uds_bytes_to_hex(b, sizeof(b), s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("62 F1 90", s);
    TEST_ASSERT_EQUAL_size_t(8, n);

    /* round-trip */
    uint8_t back[4];
    size_t bn = 0;
    TEST_ASSERT_TRUE(uds_hex_to_bytes(s, back, sizeof(back), &bn));
    TEST_ASSERT_EQUAL_size_t(3, bn);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(b, back, 3);

    /* too small a buffer refuses cleanly */
    char tiny[4];
    TEST_ASSERT_EQUAL_size_t(0, uds_bytes_to_hex(b, sizeof(b), tiny,
                                                 sizeof(tiny)));
    TEST_ASSERT_EQUAL_STRING("", tiny);
}

static void test_at_parse_single_frame(void)
{
    /* MIC/ELM headers-off single frame + prompt */
    uint8_t out[16];
    size_t n = 0;

    TEST_ASSERT_TRUE(uds_at_parse_response("62 F1 90 01 02 03\r\r>",
                                           out, sizeof(out), &n));
    TEST_ASSERT_EQUAL_size_t(6, n);
    TEST_ASSERT_EQUAL_UINT8(0x62, out[0]);
    TEST_ASSERT_EQUAL_UINT8(0x03, out[5]);
}

static void test_at_parse_multiline_isotp(void)
{
    /* ISO-TP multiline: length line + indexed frames (headers off) */
    uint8_t out[32];
    size_t n = 0;

    const char *r =
        "014\r0: 62 F1 90 31 32 33\r1: 34 35 36 37 38 39 41\r"
        "2: 42 43 44 45 46 47 48\r\r>";
    TEST_ASSERT_TRUE(uds_at_parse_response(r, out, sizeof(out), &n));
    TEST_ASSERT_EQUAL_size_t(0x14, n); /* length prefix "014" = 20 bytes */
    TEST_ASSERT_EQUAL_UINT8(0x62, out[0]);
    TEST_ASSERT_EQUAL_UINT8(0x48, out[19]);
}

static void test_at_parse_pending_lines_then_final(void)
{
    /* with the response-count digit the MIC prints every 7F xx 78 it rode
       out, then the final answer (bench 2026-09-16, erase routine) */
    uint8_t out[16];
    size_t n = 0;
    uint8_t pend = 0;

    const char *r = "7F 31 78\r7F 31 78\r7F 31 78\r71 01 FF 00 00\r\r>";
    TEST_ASSERT_TRUE(uds_at_parse_response_ex(r, out, sizeof(out), &n, &pend));
    TEST_ASSERT_EQUAL_size_t(5, n);
    TEST_ASSERT_EQUAL_UINT8(0x71, out[0]);
    TEST_ASSERT_EQUAL_UINT8(0x00, out[4]);
    TEST_ASSERT_EQUAL_UINT8(3, pend);

    /* the plain entry point still returns the final, pendings dropped */
    TEST_ASSERT_TRUE(uds_at_parse_response(r, out, sizeof(out), &n));
    TEST_ASSERT_EQUAL_size_t(5, n);

    /* only pendings, no final: nothing to return */
    TEST_ASSERT_FALSE(uds_at_parse_response_ex("7F 31 78\r7F 31 78\r\r>", out,
                                               sizeof(out), &n, &pend));
    TEST_ASSERT_EQUAL_UINT8(2, pend);

    /* a real negative response is NOT a pending: it is the answer */
    TEST_ASSERT_TRUE(uds_at_parse_response_ex("7F 22 31\r\r>", out, sizeof(out),
                                              &n, &pend));
    TEST_ASSERT_EQUAL_size_t(3, n);
    TEST_ASSERT_EQUAL_UINT8(0x31, out[2]);
    TEST_ASSERT_EQUAL_UINT8(0, pend);
}

static void test_at_parse_pending_then_multiframe_final(void)
{
    uint8_t out[32];
    size_t n = 0;
    uint8_t pend = 0;

    const char *r =
        "7F 22 78\r7F 22 78\r"
        "014\r0: 62 F1 90 31 32 33\r1: 34 35 36 37 38 39 41\r"
        "2: 42 43 44 45 46 47 48\r\r>";
    TEST_ASSERT_TRUE(uds_at_parse_response_ex(r, out, sizeof(out), &n, &pend));
    TEST_ASSERT_EQUAL_size_t(0x14, n);
    TEST_ASSERT_EQUAL_UINT8(0x62, out[0]);
    TEST_ASSERT_EQUAL_UINT8(0x48, out[19]);
    TEST_ASSERT_EQUAL_UINT8(2, pend);
}

static void test_at_parse_error_and_empty(void)
{
    uint8_t out[16];
    size_t n = 0;

    TEST_ASSERT_FALSE(uds_at_parse_response("NO DATA\r\r>", out,
                                            sizeof(out), &n));
    TEST_ASSERT_FALSE(uds_at_parse_response("7F 22 31 ERROR", out,
                                            sizeof(out), &n));
    TEST_ASSERT_FALSE(uds_at_parse_response("\r\r>", out, sizeof(out), &n));
}

/* ---- UDS DTC codec (uds_dtc_codec.c — TASK_dtc §12) ----------------------- */
#include "uds_dtc.h"

void test_dtc_requests(void)
{
    uint8_t b[4];

    TEST_ASSERT_EQUAL(3, uds_dtc_req_count(0x08, b));
    TEST_ASSERT_EQUAL_HEX8(0x19, b[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01, b[1]);
    TEST_ASSERT_EQUAL_HEX8(0x08, b[2]);

    TEST_ASSERT_EQUAL(3, uds_dtc_req_by_status(0x04, b));
    TEST_ASSERT_EQUAL_HEX8(0x02, b[1]);
    TEST_ASSERT_EQUAL_HEX8(0x04, b[2]);

    TEST_ASSERT_EQUAL(2, uds_dtc_req_supported(b));
    TEST_ASSERT_EQUAL_HEX8(0x0A, b[1]);

    /* clear: all groups + one specific DTC (the true per-code clear) */
    TEST_ASSERT_EQUAL(4, uds_dtc_req_clear(NULL, b));
    TEST_ASSERT_EQUAL_HEX8(0x14, b[0]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, b[1]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, b[3]);

    uint8_t one[3] = { 0x04, 0x20, 0x08 };

    TEST_ASSERT_EQUAL(4, uds_dtc_req_clear(one, b));
    TEST_ASSERT_EQUAL_HEX8(0x04, b[1]);
    TEST_ASSERT_EQUAL_HEX8(0x20, b[2]);
    TEST_ASSERT_EQUAL_HEX8(0x08, b[3]);
}

void test_dtc_parse_list_shapes(void)
{
    uds_dtc_t out[8];
    uint8_t mask = 0;

    /* two records (the ECU-sim shape: FTB 0, status from dtc_status) */
    const uint8_t two[] = { 0x59, 0x02, 0xFF,
                            0x04, 0x20, 0x00, 0x08,
                            0x01, 0x71, 0x00, 0x0C };

    TEST_ASSERT_EQUAL(2, uds_dtc_parse_list(two, sizeof(two), &mask,
                                            out, 8));
    TEST_ASSERT_EQUAL_HEX8(0xFF, mask);
    TEST_ASSERT_EQUAL_HEX8(0x04, out[0].hi);
    TEST_ASSERT_EQUAL_HEX8(0x20, out[0].mid);
    TEST_ASSERT_EQUAL_HEX8(0x00, out[0].ftb);
    TEST_ASSERT_EQUAL_HEX8(0x08, out[0].status);
    TEST_ASSERT_EQUAL_HEX8(0x0C, out[1].status);

    /* empty list (header only) */
    const uint8_t none[] = { 0x59, 0x02, 0xFF };

    TEST_ASSERT_EQUAL(0, uds_dtc_parse_list(none, sizeof(none), &mask,
                                            out, 8));

    /* truncated trailing record: ignored, earlier ones kept */
    const uint8_t trunc[] = { 0x59, 0x0A, 0xFF,
                              0x04, 0x20, 0x08, 0x08,
                              0x01, 0x71 };

    TEST_ASSERT_EQUAL(1, uds_dtc_parse_list(trunc, sizeof(trunc), &mask,
                                            out, 8));
    TEST_ASSERT_EQUAL_HEX8(0x08, out[0].ftb); /* sub-type carried      */

    /* out-cap: keep what fits */
    TEST_ASSERT_EQUAL(1, uds_dtc_parse_list(two, sizeof(two), &mask,
                                            out, 1));

    /* wrong service / wrong sub / NULL */
    const uint8_t neg[] = { 0x7F, 0x19, 0x31 };

    TEST_ASSERT_EQUAL(-1, uds_dtc_parse_list(neg, sizeof(neg), &mask,
                                             out, 8));

    const uint8_t sub[] = { 0x59, 0x04, 0x00 };

    TEST_ASSERT_EQUAL(-1, uds_dtc_parse_list(sub, sizeof(sub), &mask,
                                             out, 8));
    TEST_ASSERT_EQUAL(-1, uds_dtc_parse_list(NULL, 0, &mask, out, 8));
}

void test_dtc_parse_count_and_clear(void)
{
    uint8_t mask = 0;
    uint16_t count = 0;
    const uint8_t ok[] = { 0x59, 0x01, 0xFF, 0x01, 0x00, 0x02 };

    TEST_ASSERT_TRUE(uds_dtc_parse_count(ok, sizeof(ok), &mask, &count));
    TEST_ASSERT_EQUAL_HEX8(0xFF, mask);
    TEST_ASSERT_EQUAL(2, count);

    const uint8_t big[] = { 0x59, 0x01, 0xFF, 0x01, 0x01, 0x2C };

    TEST_ASSERT_TRUE(uds_dtc_parse_count(big, sizeof(big), NULL,
                                         &count));
    TEST_ASSERT_EQUAL(300, count);
    TEST_ASSERT_FALSE(uds_dtc_parse_count(ok, 5, &mask, &count));

    const uint8_t cleared[] = { 0x54 };
    const uint8_t nrc[] = { 0x7F, 0x14, 0x31 };

    TEST_ASSERT_TRUE(uds_dtc_clear_ok(cleared, 1));
    TEST_ASSERT_FALSE(uds_dtc_clear_ok(nrc, 3));
    TEST_ASSERT_FALSE(uds_dtc_clear_ok(NULL, 0));
}

void test_dtc_format_suffix_rules(void)
{
    char code[10];

    /* FTB 0 -> NO suffix (identical to the OBD code — stable diffs
       when `auto` flips protocols between scans) */
    uds_dtc_format(0x04, 0x20, 0x00, code);
    TEST_ASSERT_EQUAL_STRING("P0420", code);

    uds_dtc_format(0x04, 0x20, 0x08, code);
    TEST_ASSERT_EQUAL_STRING("P0420-08", code);

    /* every letter + high nibbles */
    uds_dtc_format(0x41, 0x23, 0x00, code);
    TEST_ASSERT_EQUAL_STRING("C0123", code);
    uds_dtc_format(0x9A, 0xBC, 0xFF, code);
    TEST_ASSERT_EQUAL_STRING("B1ABC-FF", code);
    uds_dtc_format(0xFF, 0xFF, 0x01, code);
    TEST_ASSERT_EQUAL_STRING("U3FFF-01", code);
}

void test_dtc_unformat_roundtrip(void)
{
    uint8_t hi, mid, ftb;

    TEST_ASSERT_TRUE(uds_dtc_unformat("P0420", &hi, &mid, &ftb));
    TEST_ASSERT_EQUAL_HEX8(0x04, hi);
    TEST_ASSERT_EQUAL_HEX8(0x20, mid);
    TEST_ASSERT_EQUAL_HEX8(0x00, ftb);

    TEST_ASSERT_TRUE(uds_dtc_unformat("P0420-08", &hi, &mid, &ftb));
    TEST_ASSERT_EQUAL_HEX8(0x08, ftb);

    TEST_ASSERT_TRUE(uds_dtc_unformat("u3fff-ff", &hi, &mid, &ftb));
    TEST_ASSERT_EQUAL_HEX8(0xFF, hi);
    TEST_ASSERT_EQUAL_HEX8(0xFF, mid);
    TEST_ASSERT_EQUAL_HEX8(0xFF, ftb);

    /* round-trip every shape */
    for (int i = 0; i < 256; i += 37)
    {
        char code[10];
        uint8_t h2, m2, f2;

        uds_dtc_format((uint8_t)i, (uint8_t)(255 - i), (uint8_t)i, code);
        TEST_ASSERT_TRUE(uds_dtc_unformat(code, &h2, &m2, &f2));
        TEST_ASSERT_EQUAL_HEX8((uint8_t)i, h2);
        TEST_ASSERT_EQUAL_HEX8((uint8_t)(255 - i), m2);
        TEST_ASSERT_EQUAL_HEX8((uint8_t)i, f2);
    }

    TEST_ASSERT_FALSE(uds_dtc_unformat("X0420", &hi, &mid, &ftb));
    TEST_ASSERT_FALSE(uds_dtc_unformat("P042", &hi, &mid, &ftb));
    TEST_ASSERT_FALSE(uds_dtc_unformat("P0420-8", &hi, &mid, &ftb));
    TEST_ASSERT_FALSE(uds_dtc_unformat("P0420-088", &hi, &mid, &ftb));
    TEST_ASSERT_FALSE(uds_dtc_unformat("P0420x", &hi, &mid, &ftb));
    TEST_ASSERT_FALSE(uds_dtc_unformat(NULL, &hi, &mid, &ftb));
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_positive_response);
    RUN_TEST(test_negative_response);
    RUN_TEST(test_pending_response);
    RUN_TEST(test_short_and_null_inputs);
    RUN_TEST(test_response_matches);
    RUN_TEST(test_hex_to_bytes_ok);
    RUN_TEST(test_hex_to_bytes_rejects);
    RUN_TEST(test_bytes_to_hex);
    RUN_TEST(test_at_parse_single_frame);
    RUN_TEST(test_at_parse_multiline_isotp);
    RUN_TEST(test_at_parse_pending_lines_then_final);
    RUN_TEST(test_at_parse_pending_then_multiframe_final);
    RUN_TEST(test_at_parse_error_and_empty);
    RUN_TEST(test_dtc_requests);
    RUN_TEST(test_dtc_parse_list_shapes);
    RUN_TEST(test_dtc_parse_count_and_clear);
    RUN_TEST(test_dtc_format_suffix_rules);
    RUN_TEST(test_dtc_unformat_roundtrip);
    UNITY_END();
}
