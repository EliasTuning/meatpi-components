/**
 * @file test_parse.c
 * @brief Unit tests for the pure response framing / classification —
 *        including chunk-boundary re-splits and the real-car long-response
 *        fixture (the cases where the actual bugs live).
 */
#include <string.h>

#include "unity.h"

#include "fixtures.h"
#include "obd_chip_private.h"

static obd_resp_acc_t s_acc;
static char s_out[OBD_RESP_MAX];

/** Feed @p data split into @p chunk-sized pieces; returns extract length. */
static size_t run_split(const char *cmd, const char *data, size_t chunk)
{
    size_t len = strlen(data);

    obd_parse_reset(&s_acc);

    for (size_t off = 0; off < len && !s_acc.done; off += chunk)
    {
        size_t n = (off + chunk > len) ? len - off : chunk;

        obd_parse_feed(&s_acc, data + off, n);
    }

    TEST_ASSERT_TRUE(s_acc.done);
    return obd_parse_extract(&s_acc, cmd, s_out, sizeof(s_out));
}

void test_simple_response(void)
{
    size_t n = run_split("ATI", "ATI\rELM327 v2.3\r\r>", 64);

    TEST_ASSERT_EQUAL_STRING("ELM327 v2.3", s_out);
    TEST_ASSERT_EQUAL(11, n);
}

void test_echo_off_response(void)
{
    run_split("0100", "41 00 FF FF FF FF \r\r>", 64);
    TEST_ASSERT_EQUAL_STRING("41 00 FF FF FF FF", s_out);
}

void test_prompt_split_across_chunks(void)
{
    /* every split point, including mid-echo and right before '>' */
    for (size_t chunk = 1; chunk <= 20; chunk++)
    {
        run_split("ATI", "ATI\rELM327 v2.3\r\r>", chunk);
        TEST_ASSERT_EQUAL_STRING("ELM327 v2.3", s_out);
    }
}

void test_bytes_after_prompt_not_consumed(void)
{
    const char *stream = "OK\r>GARBAGE-NEXT";

    obd_parse_reset(&s_acc);

    size_t consumed = obd_parse_feed(&s_acc, stream, strlen(stream));

    TEST_ASSERT_TRUE(s_acc.done);
    TEST_ASSERT_EQUAL(4, consumed); /* stops exactly after '>' */
    obd_parse_extract(&s_acc, NULL, s_out, sizeof(s_out));
    TEST_ASSERT_EQUAL_STRING("OK", s_out);
}

void test_unsolicited_noise_interleaves_into_window(void)
{
    /* broadcast contract: monitor frames landing inside the transaction
       window are part of what the chip printed — kept verbatim, and the
       prompt still terminates cleanly */
    run_split("0100", "7E8 06 41 00 BE 3F A8 13\r41 00 FF FF FF FF\r\r>", 7);
    TEST_ASSERT_NOT_NULL(strstr(s_out, "41 00 FF FF FF FF"));
    TEST_ASSERT_NOT_NULL(strstr(s_out, "7E8"));
}

void test_long_real_car_response_all_splits(void)
{
    /* meatpi's real proprietary-PID log: 36 lines. Replay whole and re-split
       at every chunk size from 1 to 128. */
    static const size_t CHUNKS[] = { 1, 2, 3, 7, 16, 64, 127, 128, 4096 };

    for (size_t i = 0; i < sizeof(CHUNKS) / sizeof(CHUNKS[0]); i++)
    {
        size_t n = run_split(FIXTURE_22202A_CMD, FIXTURE_22202A, CHUNKS[i]);

        TEST_ASSERT_TRUE(n > 0);
        TEST_ASSERT_FALSE(s_acc.overflow);

        /* first and last data lines intact */
        TEST_ASSERT_EQUAL_MEMORY("18DAF10110F662202A000031", s_out, 24);
        TEST_ASSERT_NOT_NULL(strstr(s_out, "18DAF1012300005555555555"));

        /* all 36 lines present */
        size_t lines = 0;

        for (const char *p = s_out; *p != '\0'; p++)
        {
            if (*p == '\r')
            {
                lines++;
            }
        }

        TEST_ASSERT_EQUAL(FIXTURE_22202A_LINES - 1, lines); /* 35 CRs join 36 lines */
    }
}

void test_overflow_is_flagged_not_fatal(void)
{
    obd_parse_reset(&s_acc);

    for (size_t i = 0; i < OBD_RESP_MAX + 100; i++)
    {
        obd_parse_feed(&s_acc, "A", 1);
    }

    obd_parse_feed(&s_acc, ">", 1);
    TEST_ASSERT_TRUE(s_acc.done);
    TEST_ASSERT_TRUE(s_acc.overflow);
}

void test_chip_error_classification(void)
{
    TEST_ASSERT_TRUE(obd_parse_is_chip_error("?"));
    TEST_ASSERT_TRUE(obd_parse_is_chip_error("UNABLE TO CONNECT"));
    TEST_ASSERT_TRUE(obd_parse_is_chip_error("CAN ERROR"));
    TEST_ASSERT_FALSE(obd_parse_is_chip_error("41 00 FF FF FF FF"));
    TEST_ASSERT_FALSE(obd_parse_is_chip_error("ELM327 v2.3"));
}

void test_monitor_command_classification(void)
{
    TEST_ASSERT_TRUE(obd_parse_is_monitor_cmd("ATMA"));
    TEST_ASSERT_TRUE(obd_parse_is_monitor_cmd("atma\r"));
    TEST_ASSERT_TRUE(obd_parse_is_monitor_cmd("AT MA"));
    TEST_ASSERT_TRUE(obd_parse_is_monitor_cmd("ATMR 10"));
    TEST_ASSERT_TRUE(obd_parse_is_monitor_cmd("ATMT7E8"));
    TEST_ASSERT_TRUE(obd_parse_is_monitor_cmd("STMA"));
    TEST_ASSERT_FALSE(obd_parse_is_monitor_cmd("ATI"));
    TEST_ASSERT_FALSE(obd_parse_is_monitor_cmd("0100"));
    TEST_ASSERT_FALSE(obd_parse_is_monitor_cmd("ATMAX")); /* not a monitor */
    TEST_ASSERT_FALSE(obd_parse_is_monitor_cmd(NULL));
}

void test_fw_iterator_and_end_marker(void)
{
    const char *fw = "AABB01\r\nCCDD02\n\nFFF1DEAD\nIGNORED\n";
    obd_fw_iter_t it;
    char line[64];

    obd_fw_iter_init(&it, fw, strlen(fw));

    TEST_ASSERT_EQUAL(6, obd_fw_iter_next(&it, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("AABB01", line); /* CR stripped */
    TEST_ASSERT_EQUAL(6, obd_fw_iter_next(&it, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("CCDD02", line); /* empty line skipped */
    TEST_ASSERT_EQUAL(8, obd_fw_iter_next(&it, line, sizeof(line)));
    TEST_ASSERT_TRUE(obd_fw_line_is_end_marker(line));
}

/* ---- EEPROM guard (obd_chip_guard.h) ------------------------------------------ */

static void expect_rewrite(const char *in, const char *want)
{
    char buf[96];

    strcpy(buf, in);
    TEST_ASSERT_EQUAL_MESSAGE(OBD_GUARD_REWRITTEN,
                              obd_chip_guard_cmd(buf, strlen(buf)), in);
    TEST_ASSERT_EQUAL_STRING(want, buf);
    /* the dry scan agrees and leaves the text alone */
    strcpy(buf, in);
    TEST_ASSERT_EQUAL(OBD_GUARD_REWRITTEN, obd_chip_guard_check(buf, strlen(buf)));
    TEST_ASSERT_EQUAL_STRING(in, buf);
}

static void expect_verdict(const char *in, obd_guard_t want)
{
    char buf[96];

    strcpy(buf, in);
    TEST_ASSERT_EQUAL_MESSAGE(want, obd_chip_guard_check(buf, strlen(buf)), in);
    TEST_ASSERT_EQUAL_STRING(in, buf);
}

void test_eeprom_guard(void)
{
    /* rewrites keep the length, canonicalize case, keep the spacing */
    expect_rewrite("ATSP6", "ATTP6");
    expect_rewrite("atsp0", "ATTP0");
    expect_rewrite("AT SP A", "AT TP A");
    expect_rewrite("at s p6", "AT T P6");
    expect_rewrite("ATSP6\r", "ATTP6\r");
    expect_rewrite("ATM1", "ATM0");
    expect_rewrite("at m 1", "AT M 0");
    /* a ';'-separated init chain (autopid) — every token is a boundary */
    expect_rewrite("ATZ;ATSP6;atm1;ATSH7DF;atsp7", "ATZ;ATTP6;ATM0;ATSH7DF;ATTP7");
    /* the Renault Zoe profile init, verbatim */
    expect_rewrite("ATE0;ATH1;ATSP7;ATS0;ATM0;ATAT1;ATFCSM1;ATCP18;",
                   "ATE0;ATH1;ATTP7;ATS0;ATM0;ATAT1;ATFCSM1;ATCP18;");

    /* pass-throughs: the protective twins, monitors, headers, reads, data */
    const char *pass[] = { "ATM0", "ATTP6", "ATSH7E4", "ATSTFF", "ATST96",
                           "ATCRA7E8", "ATCAF1", "ATPPS", "ATMA", "ATMR 11;ATMT 1A",
                           "ATRV", "ATRD", "ATDPN", "ATZ", "STSBR 2000000",
                           "STSLCS", "STMA", "0100", "22 F1 90", "010C1",
                           "VTVERS", "DATA", "xATSP6", "41 0C 0C 80" };

    for (size_t i = 0; i < sizeof(pass) / sizeof(pass[0]); i++)
    {
        expect_verdict(pass[i], OBD_GUARD_PASS);
    }

    /* refused: EEPROM writes with no RAM twin */
    const char *blocked[] = { "ATPP 0C SV 23", "ATPP0FON", "AT PP FF OFF",
                              "atpp 0e sv 7a\r", "ATSD 1A", "ATCV 1250",
                              "AT CV 0000", "STWBR", "st wbr\r", "STSAVCAL",
                              "ATSH7E0;ATPP 0F SV 95" };

    for (size_t i = 0; i < sizeof(blocked) / sizeof(blocked[0]); i++)
    {
        expect_verdict(blocked[i], OBD_GUARD_BLOCKED);
    }

    /* raw chunk semantics: only the first len bytes count */
    char raw[] = "ATSP6\rATPP 0F ON\r";

    TEST_ASSERT_EQUAL(OBD_GUARD_REWRITTEN, obd_chip_guard_check(raw, 6));
    TEST_ASSERT_EQUAL(OBD_GUARD_BLOCKED, obd_chip_guard_check(raw, strlen(raw)));
    TEST_ASSERT_EQUAL(OBD_GUARD_PASS, obd_chip_guard_cmd(NULL, 0));
}
