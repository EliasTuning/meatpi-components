/**
 * @file test_parse.c
 * @brief Unit tests for the pure response framing / classification,
 *        including chunk-boundary re-splits and the real-car long-response
 *        fixture (the cases where the actual bugs live).
 */
#include <string.h>

#include "unity.h"

#include "fixtures.h"
#include "obd_chip_private.h"

static obd_resp_acc_t s_acc;
static char s_out[OBD_RESP_MAX];

/** Feed @p len bytes of @p data split into @p chunk-sized pieces (a NUL
 *  inside the data is a byte like any other); returns extract length. */
static size_t run_bytes(const char *cmd, const char *data, size_t len, size_t chunk)
{
    obd_parse_reset(&s_acc);

    for (size_t off = 0; off < len && !s_acc.done; off += chunk)
    {
        size_t n = (off + chunk > len) ? len - off : chunk;

        obd_parse_feed(&s_acc, data + off, n);
    }

    TEST_ASSERT_TRUE(s_acc.done);
    return obd_parse_extract(&s_acc, cmd, s_out, sizeof(s_out));
}

/** Feed @p data split into @p chunk-sized pieces; returns extract length. */
static size_t run_split(const char *cmd, const char *data, size_t chunk)
{
    return run_bytes(cmd, data, strlen(data), chunk);
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
       window are part of what the chip printed, kept verbatim, and the
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

void test_fw_version_policy(void)
{
    /* the v4.52p policy (main fab54ad, issue #1023): a chip at V2.3.22 or
       V2.3.24 is current; older chips, a chip stuck in its bootloader
       ('?') and silence are not */
    TEST_ASSERT_TRUE(obd_fw_version_is_current("MIC3624 V2.3.22"));
    TEST_ASSERT_TRUE(obd_fw_version_is_current("MIC3624 V2.3.24>"));
    TEST_ASSERT_TRUE(obd_fw_version_is_current("MIC3624 V2.3.22\r\r>"));
    TEST_ASSERT_FALSE(obd_fw_version_is_current("MIC3624 V2.3.18"));
    TEST_ASSERT_FALSE(obd_fw_version_is_current("MIC3624 V2.3.10"));
    TEST_ASSERT_FALSE(obd_fw_version_is_current("MIC3624 V2.3.2"));
    TEST_ASSERT_FALSE(obd_fw_version_is_current("?"));
    TEST_ASSERT_FALSE(obd_fw_version_is_current(""));
    TEST_ASSERT_FALSE(obd_fw_version_is_current(NULL));
    /* the log string names both; the packaged V2.3.24 is among them */
    TEST_ASSERT_NOT_NULL(strstr(obd_fw_current_versions(), "V2.3.22"));
    TEST_ASSERT_NOT_NULL(strstr(obd_fw_current_versions(), "V2.3.24"));
    /* the version numbers (2026-11-01, Ali: no forced rewrite): the first
       V<a>.<b>.<c> in a VTVERS answer, the packaged constant or a file's
       path; a 'V' without three numbers behind it (VTVERS) is skipped, the
       chip's upper-case form only */
    unsigned v[3];
    char tok[16];

    TEST_ASSERT_TRUE(obd_fw_version_parse("MIC3624 V2.3.24\r\r>", v));
    TEST_ASSERT_EQUAL(2, v[0]);
    TEST_ASSERT_EQUAL(3, v[1]);
    TEST_ASSERT_EQUAL(24, v[2]);
    TEST_ASSERT_TRUE(obd_fw_version_parse("/data/obd_fw/V2.3.99.txt", v));
    TEST_ASSERT_EQUAL(99, v[2]);
    TEST_ASSERT_TRUE(obd_fw_version_parse("VTVERS V2.3.2", v));
    TEST_ASSERT_EQUAL(2, v[2]);
    TEST_ASSERT_FALSE(obd_fw_version_parse("MIC3624", v));
    TEST_ASSERT_FALSE(obd_fw_version_parse("V2.3", v));
    TEST_ASSERT_FALSE(obd_fw_version_parse("v2.3.24", v));
    TEST_ASSERT_FALSE(obd_fw_version_parse("?", v));
    TEST_ASSERT_FALSE(obd_fw_version_parse("", v));
    TEST_ASSERT_FALSE(obd_fw_version_parse(NULL, v));
    /* the version alone, for the page and the logs */
    TEST_ASSERT_EQUAL(7, obd_fw_version_token("MIC3624 V2.3.24\r\r>", tok, sizeof(tok)));
    TEST_ASSERT_EQUAL_STRING("V2.3.24", tok);
    TEST_ASSERT_EQUAL(0, obd_fw_version_token("?", tok, sizeof(tok)));
    TEST_ASSERT_EQUAL_STRING("", tok);
    TEST_ASSERT_EQUAL(0, obd_fw_version_token("MIC3624 V2.3.24", tok, 4));
    TEST_ASSERT_EQUAL_STRING("", tok);
    /* the order is numeric, field by field */
    {
        const unsigned a[3] = { 2, 3, 9 };
        const unsigned b[3] = { 2, 3, 24 };
        const unsigned c[3] = { 2, 4, 0 };

        TEST_ASSERT_TRUE(obd_fw_version_cmp(a, b) < 0);
        TEST_ASSERT_TRUE(obd_fw_version_cmp(b, a) > 0);
        TEST_ASSERT_EQUAL(0, obd_fw_version_cmp(b, b));
        TEST_ASSERT_TRUE(obd_fw_version_cmp(b, c) < 0);
    }
    /* outdated = the chip's version is below the image's: the one rule
       every user update follows (never the same version, never a
       downgrade); a chip left on V2.3.22 under the packaged V2.3.24 is
       (Ali, 2026-10-28: flagged, and the user's update lifts it); the
       image's own version, a newer chip, silence, '?' and an image without
       a version are not */
    TEST_ASSERT_TRUE(obd_fw_version_is_outdated("MIC3624 V2.3.22", "V2.3.24"));
    TEST_ASSERT_TRUE(obd_fw_version_is_outdated("MIC3624 V2.3.22\r\r>", "V2.3.24"));
    TEST_ASSERT_TRUE(obd_fw_version_is_outdated("MIC3624 V2.3.18", "V2.3.24"));
    TEST_ASSERT_TRUE(obd_fw_version_is_outdated("MIC3624 V2.3.9", "V2.3.24"));
    TEST_ASSERT_TRUE(obd_fw_version_is_outdated("MIC3624 V2.3.24", "/data/V2.3.99.txt"));
    TEST_ASSERT_FALSE(obd_fw_version_is_outdated("MIC3624 V2.3.24", "V2.3.24"));
    TEST_ASSERT_FALSE(obd_fw_version_is_outdated("MIC3624 V2.3.24\r\r>", "V2.3.24"));
    TEST_ASSERT_FALSE(obd_fw_version_is_outdated("MIC3624 V2.3.26", "V2.3.24"));
    TEST_ASSERT_FALSE(obd_fw_version_is_outdated("MIC3624 V2.3.24", "/data/V2.3.22.txt"));
    TEST_ASSERT_FALSE(obd_fw_version_is_outdated("?", "V2.3.24"));
    TEST_ASSERT_FALSE(obd_fw_version_is_outdated("", "V2.3.24"));
    TEST_ASSERT_FALSE(obd_fw_version_is_outdated(NULL, "V2.3.24"));
    TEST_ASSERT_FALSE(obd_fw_version_is_outdated("MIC3624 V2.3.22", ""));
    TEST_ASSERT_FALSE(obd_fw_version_is_outdated("MIC3624 V2.3.22", "/data/image.txt"));
    TEST_ASSERT_FALSE(obd_fw_version_is_outdated("MIC3624 V2.3.22", NULL));
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
    /* a ';'-separated init chain (autopid): every token is a boundary */
    expect_rewrite("ATZ;ATSP6;atm1;ATSH7DF;atsp7", "ATZ;ATTP6;ATM0;ATSH7DF;ATTP7");
    /* the Renault Zoe profile init, verbatim */
    expect_rewrite("ATE0;ATH1;ATSP7;ATS0;ATM0;ATAT1;ATFCSM1;ATCP18;",
                   "ATE0;ATH1;ATTP7;ATS0;ATM0;ATAT1;ATFCSM1;ATCP18;");
    /* a MIXED-protocol profile (VW MEB family: standard PIDs on 11-bit,
       UDS on 29-bit): BOTH the base and the per-PID ATSP7 become RAM-only
       ATTP, headers/CRA untouched. The only real ATSP the chip ever sees
       is obd_chip_protocol_save(), which bypasses this guard on purpose. */
    expect_rewrite("ATSP6;ATSH7DF;ATCRA;ATSP7;ATSH17FC007B;ATCRA17FE007B",
                   "ATTP6;ATSH7DF;ATCRA;ATTP7;ATSH17FC007B;ATCRA17FE007B");

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

/* ---- protocol switches (issue #1023, bench 2026-11-01) ---------------------- */

void test_protocol_switch_classification(void)
{
    /* the words the request engine sends ATM0 ahead of: the ELM forms as
       the guard leaves them (ATSP already ATTP) and as a profile writes
       them, the STN and vendor forms, spaces and case as the chip reads
       them, a chain's first element */
    const char *yes[] = { "ATTP6", "attp7", "AT TP 8", "ATTP6\r", "ATTPA6",
                          "ATSP6", "atsp 7", "ATSP00", "ATSPA6", "ATTPC",
                          "ATTP0", "STP33", "stp 34", "STP11\r", "VTP234",
                          "VTP33", "ATTP7;ATSH7DF" };
    /* not: the memory words, the resets, the reads, the other AT words,
       the STN/vendor reads and settings, data, junk */
    const char *no[] = { "ATM0", "ATM1", "ATZ", "ATD", "ATWS", "ATDPN", "ATDP",
                         "ATSH7DF", "ATST96", "ATSTFF", "ATCRA7E8", "ATTP",
                         "ATSP", "ATTP6X", "ATTPG", "ATTP678", "STPR", "STPRS",
                         "STPBR", "STPBRR", "STPC", "STPO", "STPX", "STP3",
                         "STP123", "VTPRON", "VTPROT", "VTPBRD", "VTP1234",
                         "VTVERS", "0100", "22F190", "ATPPS", "", "x",
                         "ATSH7DF;ATTP7" };

    for (size_t i = 0; i < sizeof(yes) / sizeof(yes[0]); i++)
    {
        TEST_ASSERT_TRUE_MESSAGE(obd_parse_sets_protocol(yes[i]), yes[i]);
    }

    for (size_t i = 0; i < sizeof(no) / sizeof(no[0]); i++)
    {
        TEST_ASSERT_FALSE_MESSAGE(obd_parse_sets_protocol(no[i]), no[i]);
    }

    TEST_ASSERT_FALSE(obd_parse_sets_protocol(NULL));

    /* the guard's rewrite and the classification agree on a profile's word:
       ATSP7 goes out as ATTP7 and gets its ATM0 either way */
    char buf[8];

    strcpy(buf, "ATSP7");
    TEST_ASSERT_EQUAL(OBD_GUARD_REWRITTEN, obd_chip_guard_cmd(buf, strlen(buf)));
    TEST_ASSERT_EQUAL_STRING("ATTP7", buf);
    TEST_ASSERT_TRUE(obd_parse_sets_protocol(buf));
}

/* ---- an app's reset word (issue #1023, 2026-11-01) --------------------------- */

static void expect_reset(const char *in, obd_reset_t want)
{
    TEST_ASSERT_EQUAL_MESSAGE(want, obd_parse_reset_word(in, strlen(in)), in);
}

void test_reset_word_classification(void)
{
    /* the chunk an app sends: the word alone, its line end, blanks and a
       stray line end around it, any case and spacing */
    expect_reset("ATZ\r", OBD_RESET_ATZ);
    expect_reset("ATZ", OBD_RESET_ATZ);
    expect_reset("atz\r\n", OBD_RESET_ATZ);
    expect_reset("AT Z\r", OBD_RESET_ATZ);
    expect_reset("\r\nATZ\r", OBD_RESET_ATZ);
    expect_reset(" ATZ \r", OBD_RESET_ATZ);
    expect_reset("ATWS\r", OBD_RESET_ATWS);
    expect_reset("at ws\r", OBD_RESET_ATWS);
    expect_reset("ATD\r", OBD_RESET_ATD);
    expect_reset("atd", OBD_RESET_ATD);

    /* not: a chunk with a second command (lost in the reset anyway), the
       reads and settings that start the same way, other words, junk */
    const char *no[] = { "ATZ\rATE0\r", "ATZ;ATE0", "ATZX\r", "ATDPN\r", "ATDP",
                         "ATD0\r", "ATD1", "ATWS1", "ATWSX", "ATE0\r", "ATI\r",
                         "0100\r", "ATZ 0100\r", "AT\r", "A", "", "\r", " \r\n",
                         "ATSP6\r", "ATM0\r", "VTZ\r", "STZ" };

    for (size_t i = 0; i < sizeof(no) / sizeof(no[0]); i++)
    {
        expect_reset(no[i], OBD_RESET_NONE);
    }

    /* raw chunk semantics: only the first len bytes count */
    const char *raw = "ATZ\rATE0\r";

    TEST_ASSERT_EQUAL(OBD_RESET_ATZ, obd_parse_reset_word(raw, 4));
    TEST_ASSERT_EQUAL(OBD_RESET_NONE, obd_parse_reset_word(raw, strlen(raw)));
    TEST_ASSERT_EQUAL(OBD_RESET_NONE, obd_parse_reset_word(NULL, 3));
}

/* the MIC3624's ATZ answers of 2026-11-02 (test-reports/logs/atz_soft_reset_
   2026-11-02): junk ahead of the banner, echo off and on, a NUL among it */
void test_extract_skips_reset_junk(void)
{
    static const char nul_first[] = "\0\r\rELM327 v2.3\r\r>";
    static const char echo_then_junk[] = "ATZ\r\0\xF0\xFD\0\0\r\rELM327 v2.3\r\r>";
    static const char high_bytes[] = "\xBF\x80\x04\r\rELM327 v2.3\r\r>";
    static const char mixed[] = "\x06\0\0\xF8\r\rELM327 v2.3\r\r>";

    for (size_t chunk = 1; chunk <= 8; chunk++)
    {
        run_bytes("ATZ", nul_first, sizeof(nul_first) - 1, chunk);
        TEST_ASSERT_EQUAL_STRING("ELM327 v2.3", s_out);
        run_bytes("ATZ", echo_then_junk, sizeof(echo_then_junk) - 1, chunk);
        TEST_ASSERT_EQUAL_STRING("ELM327 v2.3", s_out);
        run_bytes("ATWS", high_bytes, sizeof(high_bytes) - 1, chunk);
        TEST_ASSERT_EQUAL_STRING("ELM327 v2.3", s_out);
        run_bytes("ATZ", mixed, sizeof(mixed) - 1, chunk);
        TEST_ASSERT_EQUAL_STRING("ELM327 v2.3", s_out);
    }

    /* a clean answer is untouched, and a tab inside a line stays */
    run_split("ATZ", "\r\rELM327 v2.3\r\r>", 64);
    TEST_ASSERT_EQUAL_STRING("ELM327 v2.3", s_out);
    run_split("0100", "41 00\tBE\r\r>", 64);
    TEST_ASSERT_EQUAL_STRING("41 00\tBE", s_out);
}

/* the bench chip's ATPPS as GET /api/obd_chip/report returned it (2026-11-01) */
void test_pp_parse(void)
{
    static const char atpps[] =
        "00:FF F  01:FF F  02:FF F  03:19 F\n"
        "04:01 F  05:FF F  06:F1 F  07:09 F\n"
        "08:FF F  09:00 F  0A:0A F  0B:FF F\n"
        "0C:23 N  0D:0D F  0E:7A N  0F:95 N\n"
        "10:0D F  11:00 F  12:FF F  13:55 F\n"
        "34:E0 F  35:0F F  36:FF F  37:FF F";
    uint8_t v = 0;
    bool on = false;

    TEST_ASSERT_TRUE(obd_parse_pp(atpps, 0x0F, &v, &on));
    TEST_ASSERT_EQUAL_HEX8(0x95, v);
    TEST_ASSERT_TRUE(on);
    TEST_ASSERT_TRUE(obd_parse_pp(atpps, 0x0E, &v, &on));
    TEST_ASSERT_EQUAL_HEX8(0x7A, v);
    TEST_ASSERT_TRUE(on);
    TEST_ASSERT_TRUE(obd_parse_pp(atpps, 0x10, &v, &on));
    TEST_ASSERT_EQUAL_HEX8(0x0D, v);
    TEST_ASSERT_FALSE(on);
    TEST_ASSERT_TRUE(obd_parse_pp(atpps, 0x00, &v, &on)); /* the first token */
    TEST_ASSERT_EQUAL_HEX8(0xFF, v);
    TEST_ASSERT_TRUE(obd_parse_pp(atpps, 0x37, &v, &on)); /* the last one */
    TEST_ASSERT_EQUAL_HEX8(0xFF, v);
    TEST_ASSERT_FALSE(obd_parse_pp(atpps, 0x38, &v, &on)); /* not in the text */

    /* the fault the bench injects (ATPP 0F OFF), the vendor default, a chip
       that answered something else, and the CR line ends of the raw wire */
    TEST_ASSERT_TRUE(obd_parse_pp("0E:7A N  0F:95 F", 0x0F, &v, &on));
    TEST_ASSERT_EQUAL_HEX8(0x95, v);
    TEST_ASSERT_FALSE(on);
    TEST_ASSERT_TRUE(obd_parse_pp("0F:D5 N", 0x0F, &v, &on));
    TEST_ASSERT_EQUAL_HEX8(0xD5, v);
    TEST_ASSERT_TRUE(on);
    TEST_ASSERT_TRUE(obd_parse_pp("0C:23 N\r0F:95 N\r", 0x0F, &v, &on));
    TEST_ASSERT_EQUAL_HEX8(0x95, v);
    TEST_ASSERT_FALSE(obd_parse_pp("?", 0x0F, &v, &on));
    TEST_ASSERT_FALSE(obd_parse_pp("0F:9", 0x0F, &v, &on));
    TEST_ASSERT_FALSE(obd_parse_pp("0F:95", 0x0F, &v, &on));
    TEST_ASSERT_FALSE(obd_parse_pp("10F:95 N", 0x0F, &v, &on)); /* not a token */
    TEST_ASSERT_FALSE(obd_parse_pp(NULL, 0x0F, &v, &on));
}

/* The chip's acknowledgement and nothing else (the sleep word, the PPSW
   write; 2026-11-07): "OK" as the extract leaves it, with the line ends of
   the raw wire, and the answers that are not it (the beta.8 review's
   "NOT OK", a prompt-less banner, the unknown-command marker). */
void test_sleep_word_ack(void)
{
    TEST_ASSERT_TRUE(obd_parse_is_ok("OK"));
    TEST_ASSERT_TRUE(obd_parse_is_ok("OK\r"));
    TEST_ASSERT_TRUE(obd_parse_is_ok("\r\nOK\r\n"));
    TEST_ASSERT_TRUE(obd_parse_is_ok(" OK "));
    TEST_ASSERT_FALSE(obd_parse_is_ok("NOT OK"));
    TEST_ASSERT_FALSE(obd_parse_is_ok("OK?"));
    TEST_ASSERT_FALSE(obd_parse_is_ok("OKAY"));
    TEST_ASSERT_FALSE(obd_parse_is_ok("?"));
    TEST_ASSERT_FALSE(obd_parse_is_ok("ELM327 v2.3"));
    TEST_ASSERT_FALSE(obd_parse_is_ok(""));
    TEST_ASSERT_FALSE(obd_parse_is_ok("\r"));
    TEST_ASSERT_FALSE(obd_parse_is_ok(NULL));
}
