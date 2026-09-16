/**
 * @file test_main.c
 * @brief Host suite for autopid's pure modules: the scheduler (with THE
 *        one-request-per-PID regression test, TASK_autopid.md fix #1),
 *        the ELM response -> payload parser, and config parse/validate.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "autopid_private.h"

#include "expression_parser.h"
#include "obd2_standard_pids.h"   /* the vendored table, iterated below */

/* ---- helpers ----------------------------------------------------------------- */

static ap_config_t s_cfg;
static ap_sched_t s_st;

static void cfg_reset(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    memset(&s_st, 0, sizeof(s_st));
    s_st.type_enabled[0] = s_st.type_enabled[1] = s_st.type_enabled[2] =
        true;
}

static int add_group(const char *name, bool enabled, uint32_t period_ms)
{
    ap_group_t *g = &s_cfg.groups[s_cfg.n_groups];

    snprintf(g->name, sizeof(g->name), "%s", name);
    g->enabled_default = enabled;
    g->period_ms = period_ms;
    return s_cfg.n_groups++;
}

static int add_pid(const char *name, int group, uint32_t period_ms,
                   int n_params)
{
    ap_pid_t *p = &s_cfg.pids[s_cfg.n_pids];

    snprintf(p->name, sizeof(p->name), "%s", name);
    snprintf(p->cmd, sizeof(p->cmd), "010C");
    p->group = group;
    p->period_ms = period_ms;
    p->enabled = true;
    p->type = AP_PID_CUSTOM;
    p->param_start = s_cfg.n_params;
    p->param_count = n_params;

    for (int i = 0; i < n_params; i++)
    {
        ap_param_t *prm = &s_cfg.params[s_cfg.n_params++];

        snprintf(prm->name, sizeof(prm->name), "%s_p%d", name, i);
        snprintf(prm->expression, sizeof(prm->expression), "B%d", i);
        prm->enabled = true;
    }

    return s_cfg.n_pids++;
}

/* ---- scheduler ---------------------------------------------------------------- */

void test_sched_one_request_per_pid_per_period(void)
{
    /* FIX #1 REGRESSION: a PID with FIVE parameters is scheduled exactly
       once per period — parameters are not schedulable units at all */
    cfg_reset();

    int g = add_group("default", true, 1000);

    add_pid("multi", g, 0, 5);
    ap_sched_reset(&s_st, &s_cfg, 0);

    int runs = 0;
    int64_t now = 0;

    /* simulate 10 s of polling */
    while (now < 10 * 1000 * 1000)
    {
        int64_t due;
        int i = ap_sched_next(&s_st, &s_cfg, &due);

        TEST_ASSERT_EQUAL(0, i);   /* only one schedulable ENTRY exists */
        now = (due > now) ? due : now;
        ap_sched_ran(&s_st, &s_cfg, i, now, true);
        runs++;
    }

    /* 10 s at 1 Hz = 10 runs (+1 for the t=0 run) — NOT 5x that */
    TEST_ASSERT_INT_WITHIN(1, 10, runs);
}

void test_sched_group_inheritance_and_override(void)
{
    cfg_reset();

    int g = add_group("driving", true, 2000);
    int p = add_pid("rpm", g, 0, 1);

    ap_sched_reset(&s_st, &s_cfg, 0);
    TEST_ASSERT_EQUAL(2000 * 1000, ap_sched_period_us(&s_st, &s_cfg, p));

    /* per-PID period beats the group's */
    s_cfg.pids[p].period_ms = 500;
    TEST_ASSERT_EQUAL(500 * 1000, ap_sched_period_us(&s_st, &s_cfg, p));

    /* a runtime group override retargets everything in the group */
    s_st.group_period_override[g] = 100;
    TEST_ASSERT_EQUAL(100 * 1000, ap_sched_period_us(&s_st, &s_cfg, p));
}

void test_sched_group_toggle_gates_entries(void)
{
    cfg_reset();

    int g1 = add_group("driving", true, 1000);
    int g2 = add_group("charging", false, 1000);

    add_pid("rpm", g1, 0, 1);
    int soc = add_pid("soc", g2, 0, 1);

    ap_sched_reset(&s_st, &s_cfg, 0);

    int64_t due;

    TEST_ASSERT_EQUAL(0, ap_sched_next(&s_st, &s_cfg, &due));
    TEST_ASSERT_FALSE(ap_sched_entry_enabled(&s_st, &s_cfg, soc));

    /* the ephemeral flip (autopid.group action) */
    s_st.group_enabled[g1] = false;
    s_st.group_enabled[g2] = true;
    TEST_ASSERT_EQUAL(soc, ap_sched_next(&s_st, &s_cfg, &due));
}

void test_sched_type_gates(void)
{
    cfg_reset();

    int g = add_group("default", true, 1000);
    int p = add_pid("std_pid", g, 0, 1);

    s_cfg.pids[p].type = AP_PID_STD;
    ap_sched_reset(&s_st, &s_cfg, 0);
    s_st.type_enabled[AP_PID_STD] = false;

    int64_t due;

    TEST_ASSERT_EQUAL(-1, ap_sched_next(&s_st, &s_cfg, &due));
}

void test_sched_high_fidelity_round_robin(void)
{
    /* two period-0 PIDs share the chip fairly (last_run tiebreak) */
    cfg_reset();

    int g = add_group("fast", true, 0);

    add_pid("a", g, 0, 1);
    add_pid("b", g, 0, 1);
    ap_sched_reset(&s_st, &s_cfg, 0);

    int64_t now = 1000;
    int last = -1;

    for (int n = 0; n < 10; n++)
    {
        int64_t due;
        int i = ap_sched_next(&s_st, &s_cfg, &due);

        TEST_ASSERT_NOT_EQUAL(last, i); /* strict alternation */
        last = i;
        now += 50 * 1000; /* ~50 ms per request */
        ap_sched_ran(&s_st, &s_cfg, i, now, true);
    }
}

void test_sched_fail_backoff(void)
{
    cfg_reset();

    int g = add_group("default", true, 1000);
    int p = add_pid("dead", g, 0, 1);

    ap_sched_reset(&s_st, &s_cfg, 0);

    int64_t now = 0;

    for (int n = 0; n < AP_SCHED_BACKOFF_STREAK; n++)
    {
        ap_sched_ran(&s_st, &s_cfg, p, now, false);
    }

    /* streak reached: period x4 */
    TEST_ASSERT_EQUAL(4000 * 1000, ap_sched_period_us(&s_st, &s_cfg, p));

    /* one success clears it */
    ap_sched_ran(&s_st, &s_cfg, p, now, true);
    TEST_ASSERT_EQUAL(1000 * 1000, ap_sched_period_us(&s_st, &s_cfg, p));
}

void test_sched_stagger(void)
{
    cfg_reset();

    int g = add_group("default", true, 1000);

    add_pid("a", g, 0, 1);
    add_pid("b", g, 0, 1);
    ap_sched_reset(&s_st, &s_cfg, 1000000);
    TEST_ASSERT_EQUAL(1000000, s_st.slots[0].due_us);
    TEST_ASSERT_EQUAL(1000000 + AP_SCHED_STAGGER_US,
                      s_st.slots[1].due_us);
}

/* ---- response parser ------------------------------------------------------------ */

void test_resp_headers_off_single(void)
{
    uint8_t out[AP_PAYLOAD_MAX];
    size_t n;

    TEST_ASSERT_EQUAL(ESP_OK,
                      ap_resp_to_payload("41 0C 1A F8 \r\r", out,
                                         sizeof(out), &n));
    TEST_ASSERT_EQUAL(4, n);
    TEST_ASSERT_EQUAL_HEX8(0x41, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x0C, out[1]);
    TEST_ASSERT_EQUAL_HEX8(0x1A, out[2]);
    TEST_ASSERT_EQUAL_HEX8(0xF8, out[3]);
}

void test_resp_searching_then_data(void)
{
    uint8_t out[AP_PAYLOAD_MAX];
    size_t n;

    TEST_ASSERT_EQUAL(ESP_OK,
                      ap_resp_to_payload("SEARCHING...\r41 05 5A\r", out,
                                         sizeof(out), &n));
    TEST_ASSERT_EQUAL(3, n);
    TEST_ASSERT_EQUAL_HEX8(0x5A, out[2]);
}

void test_resp_headers_off_isotp_multiline(void)
{
    /* the real bench VIN transcript shape (0902) */
    const char *resp =
        "014\r"
        "0: 49 02 01 31 34 33\r"
        "1: 31 39 32 30 33 37 38\r"
        "2: 39 33 34 35 36 00 00\r";
    uint8_t out[AP_PAYLOAD_MAX];
    size_t n;

    TEST_ASSERT_EQUAL(ESP_OK,
                      ap_resp_to_payload(resp, out, sizeof(out), &n));
    TEST_ASSERT_EQUAL(0x14, n); /* trimmed to the announced length */
    TEST_ASSERT_EQUAL_HEX8(0x49, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x02, out[1]);
    TEST_ASSERT_EQUAL_HEX8(0x31, out[3]);
}

void test_resp_headers_on_single(void)
{
    uint8_t out[AP_PAYLOAD_MAX];
    size_t n;

    TEST_ASSERT_EQUAL(ESP_OK,
                      ap_resp_to_payload("7E8 06 41 00 BE 7F B8 13\r", out,
                                         sizeof(out), &n));
    TEST_ASSERT_EQUAL(6, n); /* PCI length honored */
    TEST_ASSERT_EQUAL_HEX8(0x41, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x13, out[5]);
}

void test_resp_headers_on_lowest_responder_wins(void)
{
    /* two ECUs answer; the LOWEST id is the legacy rule */
    const char *resp = "7E9 03 41 05 40\r7E8 03 41 05 5A\r";
    uint8_t out[AP_PAYLOAD_MAX];
    size_t n;

    TEST_ASSERT_EQUAL(ESP_OK,
                      ap_resp_to_payload(resp, out, sizeof(out), &n));
    TEST_ASSERT_EQUAL(3, n);
    TEST_ASSERT_EQUAL_HEX8(0x5A, out[2]); /* from 7E8, not 7E9 */
}

void test_resp_headers_on_isotp_multiframe(void)
{
    /* proper 8-byte CAN frames: FF = PCI(2) + 6 data, CF = PCI(1) + 7 */
    const char *resp =
        "7E8 10 14 49 02 01 31 34 33\r"
        "7E8 21 31 39 32 30 33 37 38\r"
        "7E8 22 39 33 34 35 36 00 00\r";
    uint8_t out[AP_PAYLOAD_MAX];
    size_t n;

    TEST_ASSERT_EQUAL(ESP_OK,
                      ap_resp_to_payload(resp, out, sizeof(out), &n));
    TEST_ASSERT_EQUAL(0x14, n);
    TEST_ASSERT_EQUAL_HEX8(0x49, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x31, out[3]);
    TEST_ASSERT_EQUAL_HEX8(0x36, out[17]); /* last VIN char */
    TEST_ASSERT_EQUAL_HEX8(0x00, out[19]); /* padding kept to length */
}

void test_resp_errors(void)
{
    uint8_t out[AP_PAYLOAD_MAX];
    size_t n;

    TEST_ASSERT_EQUAL(ESP_FAIL,
                      ap_resp_to_payload("NO DATA\r", out, sizeof(out),
                                         &n));
    TEST_ASSERT_EQUAL(ESP_FAIL,
                      ap_resp_to_payload("CAN ERROR\r", out, sizeof(out),
                                         &n));
    TEST_ASSERT_EQUAL(ESP_FAIL,
                      ap_resp_to_payload("?\r", out, sizeof(out), &n));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ap_resp_to_payload("SEARCHING...\r", out,
                                         sizeof(out), &n));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ap_resp_to_payload("", out, sizeof(out), &n));
}

void test_resp_payload_matches_cmd(void)
{
    /* the Phase 1b cross-talk guard: response must echo the request */
    const uint8_t rpm[] = { 0x41, 0x0C, 0x1A, 0xF8 };
    const uint8_t temp[] = { 0x41, 0x05, 0x5A };
    const uint8_t uds[] = { 0x62, 0x20, 0x2A, 0x01 };
    const uint8_t dtc[] = { 0x43, 0x01, 0x01, 0x30 };

    TEST_ASSERT_TRUE(ap_payload_matches_cmd("010C", rpm, sizeof(rpm)));
    TEST_ASSERT_FALSE(ap_payload_matches_cmd("010C", temp, sizeof(temp)));
    TEST_ASSERT_TRUE(ap_payload_matches_cmd("0105", temp, sizeof(temp)));
    /* ELM response-count hint digit is ignored by pair alignment */
    TEST_ASSERT_TRUE(ap_payload_matches_cmd("010C1", rpm, sizeof(rpm)));
    /* UDS read-by-identifier echoes 0x62 + DID */
    TEST_ASSERT_TRUE(ap_payload_matches_cmd("22202A", uds, sizeof(uds)));
    TEST_ASSERT_FALSE(ap_payload_matches_cmd("222130", uds, sizeof(uds)));
    /* single-byte service (mode 03) */
    TEST_ASSERT_TRUE(ap_payload_matches_cmd("03", dtc, sizeof(dtc)));
    TEST_ASSERT_FALSE(ap_payload_matches_cmd("07", dtc, sizeof(dtc)));
    /* AT/ST/VT commands: nothing to verify */
    TEST_ASSERT_TRUE(ap_payload_matches_cmd("ATRV", rpm, sizeof(rpm)));
    TEST_ASSERT_TRUE(ap_payload_matches_cmd("STM", rpm, sizeof(rpm)));
}

/* ---- ATMA filter frames (Phase 4) --------------------------------------------------- */

void test_filter_frame_shapes(void)
{
    uint8_t out[AP_PAYLOAD_MAX];
    size_t n;

    /* 11-bit contiguous header */
    const char *l1 = "123 DE AD BE EF 11 22 33 44";

    TEST_ASSERT_TRUE(ap_filter_frame(l1, strlen(l1), 0x123, out,
                                     sizeof(out), &n));
    TEST_ASSERT_EQUAL(8, n);
    TEST_ASSERT_EQUAL_HEX8(0xDE, out[0]);   /* B0 = first DATA byte */
    TEST_ASSERT_EQUAL_HEX8(0x44, out[7]);

    /* wrong id: skipped */
    TEST_ASSERT_FALSE(ap_filter_frame(l1, strlen(l1), 0x7E8, out,
                                      sizeof(out), &n));

    /* 29-bit contiguous header */
    const char *l2 = "18DAF110 01 02 03 04";

    TEST_ASSERT_TRUE(ap_filter_frame(l2, strlen(l2), 0x18DAF110, out,
                                     sizeof(out), &n));
    TEST_ASSERT_EQUAL(4, n);
    TEST_ASSERT_EQUAL_HEX8(0x01, out[0]);

    /* 29-bit id split into byte tokens (legacy shape) */
    const char *l3 = "18 DA F1 10 55 66";

    TEST_ASSERT_TRUE(ap_filter_frame(l3, strlen(l3), 0x18DAF110, out,
                                     sizeof(out), &n));
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_HEX8(0x55, out[0]);

    /* the REAL ATSP7 bench line: byte-split 29-bit id + 8 data bytes +
       the chip's "<DATA ERROR" marker */
    const char *l3b =
        "18 DA F1 10 DE AD BE EF 11 22 33 44 <DATA ERROR";

    TEST_ASSERT_TRUE(ap_filter_frame(l3b, strlen(l3b), 0x18DAF110, out,
                                     sizeof(out), &n));
    TEST_ASSERT_EQUAL(8, n);
    TEST_ASSERT_EQUAL_HEX8(0xDE, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x44, out[7]);

    /* 11-bit id split into two byte tokens ("01 23 ..") */
    const char *l4 = "01 23 AA BB";

    TEST_ASSERT_TRUE(ap_filter_frame(l4, strlen(l4), 0x123, out,
                                     sizeof(out), &n));
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_HEX8(0xAA, out[0]);

    /* noise / non-frames */
    TEST_ASSERT_FALSE(ap_filter_frame("STOPPED", 7, 0x123, out,
                                      sizeof(out), &n));
    TEST_ASSERT_FALSE(ap_filter_frame(">", 1, 0x123, out, sizeof(out),
                                      &n));
    TEST_ASSERT_FALSE(ap_filter_frame("", 0, 0x123, out, sizeof(out),
                                      &n));
    /* header-only line (no data bytes) */
    TEST_ASSERT_FALSE(ap_filter_frame("123", 3, 0x123, out, sizeof(out),
                                      &n));

    /* the REAL bench shape: chip appends "<DATA ERROR" after
       byte-perfect data — markers are skipped, bytes kept (legacy) */
    const char *marked = "123 DE AD BE EF 11 22 33 44 <DATA ERROR";

    TEST_ASSERT_TRUE(ap_filter_frame(marked, strlen(marked), 0x123, out,
                                     sizeof(out), &n));
    TEST_ASSERT_EQUAL(8, n);
    TEST_ASSERT_EQUAL_HEX8(0xDE, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x44, out[7]);

    /* non-hex tokens are skipped, hex kept (legacy semantics) */
    const char *bad = "123 DE ZZ AD";

    TEST_ASSERT_TRUE(ap_filter_frame(bad, strlen(bad), 0x123, out,
                                     sizeof(out), &n));
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_HEX8(0xAD, out[1]);
}

void test_filter_stream_busy_bus(void)
{
    /* a crowded monitor: many other ids + noise; the target is ONE
       frame among them (also the pre-ATCRA-buffered-frames case) */
    const char *stream =
        "100 01 02 03 04 05 06 07 08\r"
        "3AA 11 22 33 44\r"
        "SEARCHING...\r"
        "200 AA BB CC DD EE FF 00 11\r"
        "18DAF110 09 08 07 06\r"
        "123 DE AD BE EF 11 22 33 44 <DATA ERROR\r"
        "100 01 02 03 04 05 06 07 08\r";
    uint8_t out[AP_PAYLOAD_MAX];
    size_t n = 0;
    ap_flt_stream_t st;

    /* feed in EVERY chunking (uart chunk boundaries are arbitrary) */
    size_t sizes[] = { 1, 3, 7, 16, 64, 200 };

    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++)
    {
        ap_flt_stream_init(&st);

        bool got = false;
        size_t total = strlen(stream);

        for (size_t off = 0; off < total && !got; off += sizes[s])
        {
            size_t len = (off + sizes[s] > total) ? total - off
                                                  : sizes[s];

            got = ap_flt_stream_feed(&st, (const uint8_t *)stream + off,
                                     len, 0x123, out, sizeof(out), &n);
        }

        TEST_ASSERT_TRUE_MESSAGE(got, "target frame missed");
        TEST_ASSERT_EQUAL(8, n);
        TEST_ASSERT_EQUAL_HEX8(0xDE, out[0]);
        TEST_ASSERT_EQUAL_HEX8(0x44, out[7]);
    }
}

void test_filter_stream_duplicates_first_wins(void)
{
    /* the same id repeats within one window: the FIRST occurrence is
       the captured one, the collector stops there */
    const char *stream =
        "123 01 01 01 01 01 01 01 01\r"
        "123 02 02 02 02 02 02 02 02\r"
        "123 03 03 03 03 03 03 03 03\r";
    uint8_t out[AP_PAYLOAD_MAX];
    size_t n = 0;
    ap_flt_stream_t st;

    ap_flt_stream_init(&st);
    TEST_ASSERT_TRUE(ap_flt_stream_feed(&st, (const uint8_t *)stream,
                                        strlen(stream), 0x123, out,
                                        sizeof(out), &n));
    TEST_ASSERT_EQUAL_HEX8(0x01, out[0]);   /* first frame, not last */

    /* a FRESH window (new init) sees the newest traffic — feeding the
       remaining stream again matches the next frame */
    ap_flt_stream_init(&st);

    const char *rest = strchr(stream, '\r') + 1;

    TEST_ASSERT_TRUE(ap_flt_stream_feed(&st, (const uint8_t *)rest,
                                        strlen(rest), 0x123, out,
                                        sizeof(out), &n));
    TEST_ASSERT_EQUAL_HEX8(0x02, out[0]);
}

void test_filter_stream_mixed_dlc_bus(void)
{
    /* other ids with all kinds of DLCs (1..8 bytes) around a SHORT
       (DLC 2) target frame — line length never confuses the match */
    const char *stream =
        "100 01\r"
        "200 01 02 03\r"
        "300 01 02 03 04 05 06 07 08\r"
        "123 AA BB\r"
        "400 11 22 33 44 55\r";
    uint8_t out[AP_PAYLOAD_MAX];
    size_t n = 0;
    ap_flt_stream_t st;

    ap_flt_stream_init(&st);
    TEST_ASSERT_TRUE(ap_flt_stream_feed(&st, (const uint8_t *)stream,
                                        strlen(stream), 0x123, out,
                                        sizeof(out), &n));
    TEST_ASSERT_EQUAL(2, n);            /* the frame's REAL length     */
    TEST_ASSERT_EQUAL_HEX8(0xAA, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0xBB, out[1]);
}

void test_filter_short_dlc_expression_bounds(void)
{
    /* the runner's per-parameter behavior on a short frame: B0 works,
       B6/[B0:B3] beyond the 2 captured bytes = INVALID_SIZE (the
       Phase-0 bounds hardening — legacy read past the buffer here);
       the out-of-range parameter is SKIPPED, never cached as garbage */
    const char *line = "123 AA BB <DATA ERROR";
    uint8_t out[AP_PAYLOAD_MAX];
    size_t n = 0;

    TEST_ASSERT_TRUE(ap_filter_frame(line, strlen(line), 0x123, out,
                                     sizeof(out), &n));
    TEST_ASSERT_EQUAL(2, n);

    double v = 0;

    TEST_ASSERT_EQUAL(ESP_OK,
                      expression_parser_eval("B0", out, n, 0, &v));
    TEST_ASSERT_EQUAL(0xAA, (int)v);
    TEST_ASSERT_EQUAL(ESP_OK,
                      expression_parser_eval("[B0:B1]", out, n, 0, &v));
    TEST_ASSERT_EQUAL(0xAABB, (int)v);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      expression_parser_eval("B6", out, n, 0, &v));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      expression_parser_eval("[B0:B3]", out, n, 0, &v));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      expression_parser_eval("B2:7", out, n, 0, &v));
}

void test_filter_stream_truncation_recovers(void)
{
    /* an overlong garbage line (>160 chars, e.g. corrupted burst) must
       be dropped WHOLE — and the next line still parses */
    char stream[420];
    size_t w = 0;

    stream[0] = '\0';
    w += snprintf(stream + w, sizeof(stream) - w, "123 ");

    for (int i = 0; i < 80; i++)
    {
        w += snprintf(stream + w, sizeof(stream) - w, "AB ");
    }

    w += snprintf(stream + w, sizeof(stream) - w,
                  "\r123 DE AD BE EF 11 22 33 44\r");

    uint8_t out[AP_PAYLOAD_MAX];
    size_t n = 0;
    ap_flt_stream_t st;

    ap_flt_stream_init(&st);

    bool got = ap_flt_stream_feed(&st, (const uint8_t *)stream, w,
                                  0x123, out, sizeof(out), &n);

    TEST_ASSERT_TRUE(got);
    TEST_ASSERT_EQUAL(8, n);
    TEST_ASSERT_EQUAL_HEX8(0xDE, out[0]);   /* the SECOND line's frame */
}

void test_config_filter_monitor_bounds(void)
{
    /* ~1 MB struct (2048-param pool) — STATIC, never on the task stack
     * (the linux-port task stack is FreeRTOS-sized; x86-64 frames blew
     * it and the suite segfaulted in CI, 2026-07-27; §2 discipline) */
    static ap_config_t cfg;
    char err[96] = "";

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ap_config_parse("{\"filters\":[{\"frame_id\":291,"
                                      "\"monitor_ms\":10,"
                                      "\"parameters\":[{\"name\":\"x\","
                                      "\"expression\":\"B0\"}]}]}",
                                      &cfg, err, sizeof(err)));
    TEST_ASSERT_EQUAL(ESP_OK,
                      ap_config_parse("{\"filters\":[{\"frame_id\":291,"
                                      "\"monitor_ms\":500,"
                                      "\"parameters\":[{\"name\":\"x\","
                                      "\"expression\":\"B0\"}]}]}",
                                      &cfg, err, sizeof(err)));
}

/* ---- standard-PID helpers ---------------------------------------------------------- */

void test_std_expression_mapping(void)
{
    char buf[AP_EXPR_LEN];

    /* EngineRPM: bit_start 31, 16 bits, x0.25 -> [B2:B3]*0.25 */
    TEST_ASSERT_EQUAL(ESP_OK,
                      ap_std_expression(31, 16, 0.25, 0, buf,
                                        sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("[B2:B3]*0.25", buf);

    /* CoolantTemp: 8 bits, offset -40 -> B2-40 */
    TEST_ASSERT_EQUAL(ESP_OK,
                      ap_std_expression(31, 8, 1.0, -40, buf,
                                        sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("B2-40", buf);

    /* OxySensor STFT: second data byte, scale AND offset */
    TEST_ASSERT_EQUAL(ESP_OK,
                      ap_std_expression(39, 8, 0.78125, -100, buf,
                                        sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("B3*0.78125-100", buf);

    /* support bitmap: 32 bits raw */
    TEST_ASSERT_EQUAL(ESP_OK,
                      ap_std_expression(31, 32, 1.0, 0, buf,
                                        sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("[B2:B5]", buf);

    /* placeholder rows are refused */
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED,
                      ap_std_expression(0, 0, 1.0, 0, buf, sizeof(buf)));
}

void test_std_expression_whole_table_parses(void)
{
    /* EVERY non-placeholder table row must map to an expression the
       real parser accepts (no exponent literals, sane byte refs) */
    int checked = 0;

    for (int pid = 0; pid < 256; pid++)
    {
        const std_pid_t *info = get_pid((uint8_t)pid);

        if (info == NULL || info->base_name == NULL)
        {
            continue;
        }

        for (int i = 0; i < info->num_params; i++)
        {
            const std_parameter_t *prm = &info->params[i];
            char expr[AP_EXPR_LEN];
            char err[64] = "";
            size_t max_byte = 0;

            if (ap_std_expression(prm->bit_start, prm->bit_length,
                                  (double)prm->scale,
                                  (double)prm->offset, expr,
                                  sizeof(expr)) != ESP_OK)
            {
                continue;   /* placeholder */
            }

            TEST_ASSERT_NULL(strchr(expr, 'e')); /* no exponent form */
            TEST_ASSERT_EQUAL_MESSAGE(
                ESP_OK,
                expression_parser_check(expr, &max_byte, err,
                                        sizeof(err)),
                expr);
            TEST_ASSERT_TRUE(max_byte >= 2 && max_byte <= 8);
            checked++;
        }
    }

    TEST_ASSERT_GREATER_THAN(140, checked); /* 205 rows minus placeholders */
}

void test_std_scan_parse_shapes(void)
{
    uint32_t bm = 0;

    /* headers off, single ECU */
    TEST_ASSERT_TRUE(ap_std_scan_parse("41 00 BE 7F B8 13 \r\r", 0x00,
                                       &bm));
    TEST_ASSERT_EQUAL_HEX32(0xBE7FB813, bm);

    /* SEARCHING noise + data */
    TEST_ASSERT_TRUE(ap_std_scan_parse("SEARCHING...\r41 20 80 00 00 01\r",
                                       0x20, &bm));
    TEST_ASSERT_EQUAL_HEX32(0x80000001, bm);

    /* multi-ECU rows OR together */
    TEST_ASSERT_TRUE(ap_std_scan_parse("41 00 BE 7F B8 13\r"
                                       "41 00 80 00 00 01\r",
                                       0x00, &bm));
    TEST_ASSERT_EQUAL_HEX32(0xBE7FB813 | 0x80000001, bm);

    /* headers on (11-bit): anchor skips header + PCI */
    TEST_ASSERT_TRUE(ap_std_scan_parse("7E8 06 41 00 BE 7F B8 13\r",
                                       0x00, &bm));
    TEST_ASSERT_EQUAL_HEX32(0xBE7FB813, bm);

    /* wrong PID echo, errors, truncation */
    TEST_ASSERT_FALSE(ap_std_scan_parse("41 05 5A\r", 0x00, &bm));
    TEST_ASSERT_FALSE(ap_std_scan_parse("NO DATA\r", 0x00, &bm));
    TEST_ASSERT_FALSE(ap_std_scan_parse("41 00 BE 7F\r", 0x00, &bm));
    TEST_ASSERT_FALSE(ap_std_scan_parse("", 0x00, &bm));
}

/* ---- config parse ----------------------------------------------------------------- */

void test_config_parse_happy(void)
{
    const char *json =
        "{\"groups\":[{\"name\":\"driving\",\"enabled\":true,"
        "\"period_ms\":500}],"
        "\"pids\":[{\"name\":\"engine\",\"type\":\"std\",\"cmd\":\"010C\","
        "\"group\":\"driving\",\"parameters\":["
        "{\"name\":\"rpm\",\"expression\":\"[B2:B3]/4\",\"unit\":\"rpm\","
        "\"min\":0,\"max\":16384},"
        "{\"name\":\"rpm_raw\",\"expression\":\"[B2:B3]\"}]}],"
        "\"filters\":[{\"frame_id\":666,\"monitor_ms\":800,"
        "\"parameters\":[{\"name\":\"soc\",\"expression\":\"B4/2\"}]}]}";
    /* ~1 MB struct (2048-param pool) — STATIC, never on the task stack
     * (the linux-port task stack is FreeRTOS-sized; x86-64 frames blew
     * it and the suite segfaulted in CI, 2026-07-27; §2 discipline) */
    static ap_config_t cfg;
    char err[96] = "";

    TEST_ASSERT_EQUAL(ESP_OK,
                      ap_config_parse(json, &cfg, err, sizeof(err)));
    TEST_ASSERT_EQUAL(2, cfg.n_groups); /* driving + auto "default" */
    TEST_ASSERT_EQUAL(1, cfg.n_pids);
    TEST_ASSERT_EQUAL(1, cfg.n_filters);
    TEST_ASSERT_EQUAL(3, cfg.n_params);
    TEST_ASSERT_EQUAL(AP_PID_STD, cfg.pids[0].type);
    TEST_ASSERT_EQUAL(2, cfg.pids[0].param_count);
    TEST_ASSERT_EQUAL_STRING("rpm", cfg.params[0].name);
    TEST_ASSERT_EQUAL(0, cfg.pids[0].group); /* driving is group 0 */
    TEST_ASSERT_EQUAL(1, cfg.filters[0].group); /* default */
    TEST_ASSERT_FALSE(cfg.filters[0].is_extended);
}

void test_config_parse_rejects_bad_expression(void)
{
    const char *json =
        "{\"pids\":[{\"name\":\"x\",\"cmd\":\"0105\",\"parameters\":["
        "{\"name\":\"t\",\"expression\":\"[B3:B0]\"}]}]}";
    /* ~1 MB struct (2048-param pool) — STATIC, never on the task stack
     * (the linux-port task stack is FreeRTOS-sized; x86-64 frames blew
     * it and the suite segfaulted in CI, 2026-07-27; §2 discipline) */
    static ap_config_t cfg;
    char err[96] = "";

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ap_config_parse(json, &cfg, err, sizeof(err)));
    TEST_ASSERT_TRUE(strlen(err) > 0);
    TEST_ASSERT_EQUAL(0, cfg.n_pids); /* tables wiped on failure */
}

void test_config_parse_rejects_unknown_group_and_dupes(void)
{
    /* ~1 MB struct (2048-param pool) — STATIC, never on the task stack
     * (the linux-port task stack is FreeRTOS-sized; x86-64 frames blew
     * it and the suite segfaulted in CI, 2026-07-27; §2 discipline) */
    static ap_config_t cfg;
    char err[96] = "";

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ap_config_parse("{\"pids\":[{\"name\":\"x\","
                                      "\"cmd\":\"0105\","
                                      "\"group\":\"nope\"}]}",
                                      &cfg, err, sizeof(err)));

    /* duplicate parameter names break cache keying */
    const char *dupes =
        "{\"pids\":["
        "{\"name\":\"a\",\"cmd\":\"0105\",\"parameters\":["
        "{\"name\":\"temp\",\"expression\":\"B2\"}]},"
        "{\"name\":\"b\",\"cmd\":\"0106\",\"parameters\":["
        "{\"name\":\"temp\",\"expression\":\"B2\"}]}]}";

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ap_config_parse(dupes, &cfg, err, sizeof(err)));
}

void test_config_parse_empty_and_garbage(void)
{
    /* ~1 MB struct (2048-param pool) — STATIC, never on the task stack
     * (the linux-port task stack is FreeRTOS-sized; x86-64 frames blew
     * it and the suite segfaulted in CI, 2026-07-27; §2 discipline) */
    static ap_config_t cfg;
    char err[96] = "";

    /* empty object = valid empty tables + the auto default group */
    TEST_ASSERT_EQUAL(ESP_OK, ap_config_parse("{}", &cfg, err,
                                              sizeof(err)));
    TEST_ASSERT_EQUAL(1, cfg.n_groups);
    TEST_ASSERT_EQUAL(0, cfg.n_pids);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ap_config_parse("nonsense", &cfg, err, sizeof(err)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ap_config_parse("[1,2]", &cfg, err, sizeof(err)));
}

void test_init_sanitize(void)
{
    /* ATSP and ATM1 write the chip's EEPROM; init strings replay per
       type/PID transition, so both must be neutralized before send */
    char buf[64];

    strcpy(buf, "ATSP6");
    ap_init_sanitize(buf);
    TEST_ASSERT_EQUAL_STRING("ATTP6", buf);

    /* case-insensitive, canonicalized */
    strcpy(buf, "atsp0");
    ap_init_sanitize(buf);
    TEST_ASSERT_EQUAL_STRING("ATTP0", buf);

    /* whitespace-tolerant, spacing preserved */
    strcpy(buf, "AT SP 6");
    ap_init_sanitize(buf);
    TEST_ASSERT_EQUAL_STRING("AT TP 6", buf);

    strcpy(buf, "at s p6");
    ap_init_sanitize(buf);
    TEST_ASSERT_EQUAL_STRING("AT T P6", buf);

    /* memory-on -> memory-off */
    strcpy(buf, "ATM1");
    ap_init_sanitize(buf);
    TEST_ASSERT_EQUAL_STRING("ATM0", buf);

    strcpy(buf, "at m 1");
    ap_init_sanitize(buf);
    TEST_ASSERT_EQUAL_STRING("AT M 0", buf);

    /* memory-off passes through (it is the protective command) */
    strcpy(buf, "ATM0");
    ap_init_sanitize(buf);
    TEST_ASSERT_EQUAL_STRING("ATM0", buf);

    /* monitor commands are NOT memory: ATMA, ATMR hh, ATMT hh —
       even with a '1' in their argument */
    strcpy(buf, "ATMA;ATMR 11;ATMT 1A;AT MA");
    ap_init_sanitize(buf);
    TEST_ASSERT_EQUAL_STRING("ATMA;ATMR 11;ATMT 1A;AT MA", buf);

    /* multiple occurrences in one string */
    strcpy(buf, "ATZ;ATSP6;atm1;ATSH7DF;atsp7");
    ap_init_sanitize(buf);
    TEST_ASSERT_EQUAL_STRING("ATZ;ATTP6;ATM0;ATSH7DF;ATTP7", buf);

    /* 2026-09-16: commands with no RAM twin are refused at config parse
       (ATPP re-baud = a bricked link), the rewrite still applies */
    static ap_config_t cfg;
    char err[96] = "";
    const char *bad =
        "{\"pids\":[{\"name\":\"x\",\"cmd\":\"0105\",\"init\":\"ATPP 0C SV 23\","
        "\"parameters\":[{\"name\":\"t\",\"expression\":\"B2\"}]}]}";

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ap_config_parse(bad, &cfg, err, sizeof(err)));
    TEST_ASSERT_TRUE(strstr(err, "EEPROM") != NULL);

    const char *ok =
        "{\"pids\":[{\"name\":\"x\",\"cmd\":\"0105\",\"init\":\"ATSP6;ATM1\","
        "\"parameters\":[{\"name\":\"t\",\"expression\":\"B2\"}]}]}";

    TEST_ASSERT_EQUAL(ESP_OK, ap_config_parse(ok, &cfg, err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("ATTP6;ATM0", cfg.pids[0].init);

    /* the Renault Zoe profile init, verbatim */
    strcpy(buf, "ATE0;ATH1;ATSP7;ATS0;ATM0;ATAT1;ATFCSM1;ATCP18;");
    ap_init_sanitize(buf);
    TEST_ASSERT_EQUAL_STRING("ATE0;ATH1;ATTP7;ATS0;ATM0;ATAT1;ATFCSM1;"
                             "ATCP18;", buf);

    /* non-matches untouched: ATS1 (spaces), ATSH (headers), ATTP */
    strcpy(buf, "ATS1;ATSH7E0;ATTP6");
    ap_init_sanitize(buf);
    TEST_ASSERT_EQUAL_STRING("ATS1;ATSH7E0;ATTP6", buf);

    /* any AT..S..P run is claimed as ATSP (legacy behavior — there is
       no other AT S*P* command for it to collide with) */
    strcpy(buf, "AT SPACES");
    ap_init_sanitize(buf);
    TEST_ASSERT_EQUAL_STRING("AT TPACES", buf);

    /* truncated tails don't read past the terminator */
    strcpy(buf, "ATS");
    ap_init_sanitize(buf);
    TEST_ASSERT_EQUAL_STRING("ATS", buf);
    strcpy(buf, "AT S ");
    ap_init_sanitize(buf);
    TEST_ASSERT_EQUAL_STRING("AT S ", buf);
    strcpy(buf, "ATM");
    ap_init_sanitize(buf);
    TEST_ASSERT_EQUAL_STRING("ATM", buf);
    strcpy(buf, "AT M ");
    ap_init_sanitize(buf);
    TEST_ASSERT_EQUAL_STRING("AT M ", buf);
    strcpy(buf, "A");
    ap_init_sanitize(buf);
    TEST_ASSERT_EQUAL_STRING("A", buf);

    ap_init_sanitize(NULL); /* no crash */
}

void test_config_parse_sanitizes_cmd_and_init(void)
{
    /* profile/custom PIDs can carry AT commands in cmd as well as init —
       both must leave ap_config_parse EEPROM-safe */
    const char *json =
        "{\"pids\":[{\"name\":\"w\",\"cmd\":\"ATSP6\","
        "\"init\":\"ATM1;atsp7\"}]}";
    /* ~1 MB struct (2048-param pool) — STATIC, never on the task stack
     * (the linux-port task stack is FreeRTOS-sized; x86-64 frames blew
     * it and the suite segfaulted in CI, 2026-07-27; §2 discipline) */
    static ap_config_t cfg;
    char err[96] = "";

    TEST_ASSERT_EQUAL(ESP_OK,
                      ap_config_parse(json, &cfg, err, sizeof(err)));
    TEST_ASSERT_EQUAL(1, cfg.n_pids);
    TEST_ASSERT_EQUAL_STRING("ATTP6", cfg.pids[0].cmd);
    TEST_ASSERT_EQUAL_STRING("ATM0;ATTP7", cfg.pids[0].init);
}

/* ---- DTC codec (autopid_dtc_codec.c — TASK_dtc.md §4) ------------------- */

void test_dtc_format_all_letters(void)
{
    char code[AP_DTC_CODE_LEN];

    ap_dtc_format(0x01, 0x43, code);           /* 00 -> P */
    TEST_ASSERT_EQUAL_STRING("P0143", code);
    ap_dtc_format(0x41, 0x23, code);           /* 01 -> C */
    TEST_ASSERT_EQUAL_STRING("C0123", code);
    ap_dtc_format(0x9A, 0xBC, code);           /* 10 -> B, digit 1 */
    TEST_ASSERT_EQUAL_STRING("B1ABC", code);
    ap_dtc_format(0xF1, 0xAB, code);           /* 11 -> U, digit 3 */
    TEST_ASSERT_EQUAL_STRING("U31AB", code);
    ap_dtc_format(0x04, 0x20, code);
    TEST_ASSERT_EQUAL_STRING("P0420", code);
}

void test_dtc_unformat_roundtrip_and_rejects(void)
{
    uint8_t hi, lo;

    TEST_ASSERT_TRUE(ap_dtc_unformat("P0420", &hi, &lo));
    TEST_ASSERT_EQUAL_HEX8(0x04, hi);
    TEST_ASSERT_EQUAL_HEX8(0x20, lo);
    TEST_ASSERT_TRUE(ap_dtc_unformat("u31ab", &hi, &lo)); /* case-insensitive */
    TEST_ASSERT_EQUAL_HEX8(0xF1, hi);
    TEST_ASSERT_EQUAL_HEX8(0xAB, lo);

    /* full roundtrip across the letter space */
    for (int b = 0; b < 256; b += 7)
    {
        char code[AP_DTC_CODE_LEN];
        uint8_t rhi, rlo;

        ap_dtc_format((uint8_t)b, 0x5A, code);
        TEST_ASSERT_TRUE(ap_dtc_unformat(code, &rhi, &rlo));
        TEST_ASSERT_EQUAL_HEX8((uint8_t)b, rhi);
        TEST_ASSERT_EQUAL_HEX8(0x5A, rlo);
    }

    TEST_ASSERT_FALSE(ap_dtc_unformat("X0420", &hi, &lo)); /* bad letter  */
    TEST_ASSERT_FALSE(ap_dtc_unformat("P4420", &hi, &lo)); /* digit > 3   */
    TEST_ASSERT_FALSE(ap_dtc_unformat("P042", &hi, &lo));  /* short       */
    TEST_ASSERT_FALSE(ap_dtc_unformat("P04200", &hi, &lo));/* long        */
    TEST_ASSERT_FALSE(ap_dtc_unformat("P0G20", &hi, &lo)); /* non-hex     */
    TEST_ASSERT_FALSE(ap_dtc_unformat(NULL, &hi, &lo));
}

void test_dtc_parse_codes_shapes(void)
{
    char out[8][AP_DTC_CODE_LEN];

    /* canonical: 43 <count> <pairs> */
    const uint8_t two[] = { 0x43, 0x02, 0x01, 0x43, 0x04, 0x20 };

    TEST_ASSERT_EQUAL(2, ap_dtc_parse_codes(two, sizeof(two), 0x43,
                                            out, 8));
    TEST_ASSERT_EQUAL_STRING("P0143", out[0]);
    TEST_ASSERT_EQUAL_STRING("P0420", out[1]);

    /* zero codes */
    const uint8_t none[] = { 0x43, 0x00 };

    TEST_ASSERT_EQUAL(0, ap_dtc_parse_codes(none, sizeof(none), 0x43,
                                            out, 8));

    /* padding pairs skipped */
    const uint8_t padded[] = { 0x43, 0x01, 0x04, 0x20, 0x00, 0x00 };

    TEST_ASSERT_EQUAL(1, ap_dtc_parse_codes(padded, sizeof(padded), 0x43,
                                            out, 8));
    TEST_ASSERT_EQUAL_STRING("P0420", out[0]);

    /* stripped count byte (odd remainder after svc+count parse) */
    const uint8_t nocount[] = { 0x43, 0x04, 0x20 };

    TEST_ASSERT_EQUAL(1, ap_dtc_parse_codes(nocount, sizeof(nocount),
                                            0x43, out, 8));
    TEST_ASSERT_EQUAL_STRING("P0420", out[0]);

    /* pending (47) + permanent (4A) service bytes */
    const uint8_t pend[] = { 0x47, 0x01, 0x41, 0x23 };

    TEST_ASSERT_EQUAL(1, ap_dtc_parse_codes(pend, sizeof(pend), 0x47,
                                            out, 8));
    TEST_ASSERT_EQUAL_STRING("C0123", out[0]);

    /* wrong service byte = cross-talk -> -1 */
    TEST_ASSERT_EQUAL(-1, ap_dtc_parse_codes(pend, sizeof(pend), 0x43,
                                             out, 8));
    TEST_ASSERT_EQUAL(-1, ap_dtc_parse_codes(NULL, 0, 0x43, out, 8));

    /* out_max cap keeps what fits */
    const uint8_t many[] = { 0x43, 0x03, 0x01, 0x00, 0x02, 0x00,
                             0x03, 0x00 };

    TEST_ASSERT_EQUAL(2, ap_dtc_parse_codes(many, sizeof(many), 0x43,
                                            out, 2));
}

void test_dtc_parse_mil(void)
{
    bool mil = false;
    uint8_t count = 0;

    const uint8_t on[] = { 0x41, 0x01, 0x83, 0x07, 0x65, 0x04 };

    TEST_ASSERT_TRUE(ap_dtc_parse_mil(on, sizeof(on), &mil, &count));
    TEST_ASSERT_TRUE(mil);
    TEST_ASSERT_EQUAL(3, count);

    const uint8_t off[] = { 0x41, 0x01, 0x00, 0x07, 0x65, 0x04 };

    TEST_ASSERT_TRUE(ap_dtc_parse_mil(off, sizeof(off), &mil, &count));
    TEST_ASSERT_FALSE(mil);
    TEST_ASSERT_EQUAL(0, count);

    const uint8_t wrong[] = { 0x41, 0x0C, 0x1A, 0xF8 };

    TEST_ASSERT_FALSE(ap_dtc_parse_mil(wrong, sizeof(wrong), &mil,
                                       &count));
    TEST_ASSERT_FALSE(ap_dtc_parse_mil(on, 2, &mil, &count)); /* short */
}

void test_dtc_clear_condition_matrix(void)
{
    char present[3][AP_DTC_CODE_LEN];

    strcpy(present[0], "P0420");
    strcpy(present[1], "P0171");
    strcpy(present[2], "C0123");

    /* always: unconditional */
    TEST_ASSERT_TRUE(ap_dtc_clear_allowed(present, 3, NULL,
                                          AP_DTC_CLEAR_ALWAYS));
    TEST_ASSERT_TRUE(ap_dtc_clear_allowed(present, 0, NULL,
                                          AP_DTC_CLEAR_ALWAYS));

    /* if_any: one listed code present suffices */
    TEST_ASSERT_TRUE(ap_dtc_clear_allowed(present, 3, "P0420",
                                          AP_DTC_CLEAR_IF_ANY));
    TEST_ASSERT_TRUE(ap_dtc_clear_allowed(present, 3, "P9999, p0171",
                                          AP_DTC_CLEAR_IF_ANY));
    TEST_ASSERT_FALSE(ap_dtc_clear_allowed(present, 3, "P9999",
                                           AP_DTC_CLEAR_IF_ANY));
    TEST_ASSERT_FALSE(ap_dtc_clear_allowed(present, 0, "P0420",
                                           AP_DTC_CLEAR_IF_ANY));

    /* if_only: every present code must be listed */
    TEST_ASSERT_TRUE(ap_dtc_clear_allowed(present, 3,
                                          "P0420,P0171,C0123",
                                          AP_DTC_CLEAR_IF_ONLY));
    TEST_ASSERT_FALSE(ap_dtc_clear_allowed(present, 3, "P0420,P0171",
                                           AP_DTC_CLEAR_IF_ONLY));
    /* nothing present: refuse (don't burn readiness for a no-op) */
    TEST_ASSERT_FALSE(ap_dtc_clear_allowed(present, 0, "P0420",
                                           AP_DTC_CLEAR_IF_ONLY));

    TEST_ASSERT_FALSE(ap_dtc_clear_allowed(present, 3, "P0420",
                                           AP_DTC_CLEAR_INVALID));
}

void test_dtc_clear_mode_parse(void)
{
    TEST_ASSERT_EQUAL(AP_DTC_CLEAR_ALWAYS,
                      ap_dtc_clear_mode_parse(NULL, NULL));
    TEST_ASSERT_EQUAL(AP_DTC_CLEAR_ALWAYS,
                      ap_dtc_clear_mode_parse("", ""));
    TEST_ASSERT_EQUAL(AP_DTC_CLEAR_IF_ANY,
                      ap_dtc_clear_mode_parse(NULL, "P0420"));
    TEST_ASSERT_EQUAL(AP_DTC_CLEAR_ALWAYS,
                      ap_dtc_clear_mode_parse("always", "P0420"));
    TEST_ASSERT_EQUAL(AP_DTC_CLEAR_IF_ANY,
                      ap_dtc_clear_mode_parse("if_any", NULL));
    TEST_ASSERT_EQUAL(AP_DTC_CLEAR_IF_ONLY,
                      ap_dtc_clear_mode_parse("if_only", "P0420"));
    TEST_ASSERT_EQUAL(AP_DTC_CLEAR_INVALID,
                      ap_dtc_clear_mode_parse("sometimes", NULL));
}

/* ---- DTC database importer (autopid_dtc_db_codec.c — TASK_dtc_db §2) ---- */

static char g_scratch[4096];
static ap_dtc_db_item_t g_items[64];
static char g_out[8192];

static int db_import(const char *in, char *fmt, char *err, size_t errlen)
{
    return ap_dtc_db_import(in, strlen(in), g_scratch, sizeof(g_scratch),
                            g_items, 64, fmt, err, errlen);
}

/** import -> serialize -> re-index roundtrip; returns entry count. */
static int db_roundtrip(const char *in)
{
    char fmt[12], err[64];
    int n = db_import(in, fmt, err, sizeof(err));

    if (n < 0)
    {
        return n;
    }

    size_t w = ap_dtc_db_serialize(g_scratch, g_items, n, g_out,
                                   sizeof(g_out));

    TEST_ASSERT_TRUE(w > 0);
    return ap_dtc_db_index(g_out, w, g_items, 64);
}

static const char *db_desc_of(const char *code)
{
    static char buf[128];
    int n = ap_dtc_db_index(g_out, strlen(g_out), g_items, 64);

    for (int i = 0; i < n; i++)
    {
        if (strcmp(g_items[i].code, code) == 0)
        {
            memcpy(buf, g_out + g_items[i].off, g_items[i].len);
            buf[g_items[i].len] = '\0';
            return buf;
        }
    }

    return NULL;
}

void test_dtcdb_csv_variants(void)
{
    char fmt[12], err[64];

    /* comma + header + quoted field with comma + CRLF */
    TEST_ASSERT_EQUAL(2, db_roundtrip(
        "code,description\r\n"
        "P0420,\"Catalyst, efficiency low\"\r\n"
        "p0171,System Too Lean Bank 1\r\n"));
    TEST_ASSERT_EQUAL_STRING("Catalyst, efficiency low",
                             db_desc_of("P0420"));
    TEST_ASSERT_NOT_NULL(db_desc_of("P0171")); /* upper-cased */

    /* semicolon */
    TEST_ASSERT_EQUAL(1, db_import("C0035;Left front wheel speed\n",
                                   fmt, err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("semicolon", fmt);

    /* tab */
    TEST_ASSERT_EQUAL(1, db_import("U0100\tLost comm with ECM\n", fmt,
                                   err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("tsv", fmt);

    /* plain text (whitespace split) */
    TEST_ASSERT_EQUAL(1, db_import("B1000   ECU internal fault\n", fmt,
                                   err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("text", fmt);
}

void test_dtcdb_json_shapes(void)
{
    char fmt[12], err[64];

    TEST_ASSERT_EQUAL(2, db_roundtrip(
        "{\"P0420\": \"Catalyst \\\"low\\\"\", \"P0301\": \"Cyl 1 misfire\"}"));
    TEST_ASSERT_EQUAL_STRING("Catalyst \"low\"", db_desc_of("P0420"));

    TEST_ASSERT_EQUAL(2, db_import(
        "[{\"code\":\"P0102\",\"description\":\"MAF low\"},"
        " {\"dtc\":\"p0113\",\"desc\":\"IAT high\",\"severity\":3}]",
        fmt, err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("json-array", fmt);

    /* nested values + non-string entries are skipped, not fatal */
    TEST_ASSERT_EQUAL(1, db_import(
        "{\"meta\":{\"v\":1},\"count\":2,\"P0500\":\"VSS malfunction\"}",
        fmt, err, sizeof(err)));

    TEST_ASSERT_EQUAL(-1, db_import("{\"P0420\":\"unterminated",
                                    fmt, err, sizeof(err)));
}

void test_dtcdb_dedup_sort_suffix(void)
{
    /* duplicate: LAST wins; suffix "-00" truncated to base; sorted out */
    TEST_ASSERT_EQUAL(2, db_roundtrip(
        "P0420,first text\n"
        "U0001,CAN bus off\n"
        "P0420-00,second text\n"));
    TEST_ASSERT_EQUAL_STRING("second text", db_desc_of("P0420"));

    /* sorted: P before U */
    int n = ap_dtc_db_index(g_out, strlen(g_out), g_items, 64);

    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_STRING("P0420", g_items[0].code);
    TEST_ASSERT_EQUAL_STRING("U0001", g_items[1].code);
}

void test_dtcdb_rejects_and_edges(void)
{
    char fmt[12], err[64];

    /* zero valid entries -> -1 with a useful error */
    TEST_ASSERT_EQUAL(-1, db_import("hello,world\nfoo,bar\n", fmt, err,
                                    sizeof(err)));
    TEST_ASSERT_TRUE(strstr(err, "no valid entries") != NULL);

    TEST_ASSERT_EQUAL(-1, ap_dtc_db_import(NULL, 0, g_scratch,
                                           sizeof(g_scratch), g_items,
                                           64, fmt, err, sizeof(err)));

    /* bad rows between good ones are skipped */
    TEST_ASSERT_EQUAL(2, db_import(
        "# comment\n\nP0001,Fuel volume reg\nnot-a-code,junk\n"
        "P0002,Fuel volume range\n", fmt, err, sizeof(err)));

    /* long description truncates at AP_DTC_DESC_MAX */
    char big[400] = "P0700,";

    memset(big + 6, 'x', 300);
    big[306] = '\n';
    big[307] = '\0';
    TEST_ASSERT_EQUAL(1, db_roundtrip(big));

    const char *d = db_desc_of("P0700");

    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL(AP_DTC_DESC_MAX, strlen(d));

    /* canonical index rejects garbage + wrong counts */
    TEST_ASSERT_EQUAL(-1, ap_dtc_db_index("junk", 4, g_items, 64));
    TEST_ASSERT_EQUAL(-1, ap_dtc_db_index("#dtcdb1 5\nP0420\tx\n",
                                          strlen("#dtcdb1 5\nP0420\tx\n"),
                                          g_items, 64));
}

void test_dtcdb_search_match(void)
{
    /* code prefix, case-insensitive */
    TEST_ASSERT_TRUE(ap_dtc_db_match("P0420", "Catalyst", 8, "p04"));
    TEST_ASSERT_TRUE(ap_dtc_db_match("P0420", "Catalyst", 8, "P0420"));
    TEST_ASSERT_FALSE(ap_dtc_db_match("P0420", "Catalyst", 8, "P05"));

    /* description substring, case-insensitive */
    TEST_ASSERT_TRUE(ap_dtc_db_match("P0420", "Catalyst efficiency", 19,
                                     "EFFIC"));
    TEST_ASSERT_FALSE(ap_dtc_db_match("P0420", "Catalyst", 8, "misfire"));

    /* empty q = browse-all */
    TEST_ASSERT_TRUE(ap_dtc_db_match("P0420", "x", 1, ""));
    TEST_ASSERT_TRUE(ap_dtc_db_match("P0420", "x", 1, NULL));
}

/* ---- DBC codec (autopid_dbc_codec.c — TASK_dbc.md) ----------------------- */

static ap_dbc_msg_t g_msgs[16];
static ap_dbc_sig_t g_sigs[32];

void test_dbc_parse_fixture(void)
{
    const char *dbc =
        "VERSION \"\"\n\nNS_ :\n BA_\n\nBS_:\n\nBU_ ECU1 Vector__XXX\n\n"
        "BO_ 3221225472 VECTOR__INDEPENDENT_SIG_MSG: 0 Vector__XXX\n"
        " SG_ Orphan : 0|8@1+ (1,0) [0|0] \"\" Vector__XXX\n\n"
        "BO_ 291 Engine: 8 ECU1\n"
        " SG_ EngineSpeed : 24|16@1+ (0.125,0) [0|8031.875] \"rpm\" V\n"
        " SG_ CoolantTemp : 16|8@1+ (1,-40) [-40|215] \"degC\" V\n"
        " SG_ MuxSwitch M : 0|4@1+ (1,0) [0|15] \"\" V\n"
        " SG_ MuxedVal m2 : 8|8@1+ (1,0) [0|255] \"\" V\n"
        "BO_ 2364540158 EEC1: 8 ECU1\n"
        " SG_ TorqueBE : 7|16@0- (1,0) [-32768|32767] \"Nm\" V\n"
        " SG_ FloatSig : 39|32@1+ (1,0) [0|0] \"\" V\n"
        "SIG_VALTYPE_ 2364540158 FloatSig : 1;\n";
    char err[96];
    int nm = 0;
    int ns = ap_dbc_parse(dbc, strlen(dbc), g_msgs, 16, &nm, g_sigs, 32,
                          err, sizeof(err));

    TEST_ASSERT_EQUAL(6, ns);           /* orphan msg + its SG_ skipped */
    TEST_ASSERT_EQUAL(2, nm);
    TEST_ASSERT_EQUAL_STRING("Engine", g_msgs[0].name);
    TEST_ASSERT_EQUAL_HEX32(0x123, g_msgs[0].id);
    TEST_ASSERT_FALSE(g_msgs[0].ext);
    TEST_ASSERT_TRUE(g_msgs[1].ext);
    TEST_ASSERT_EQUAL_HEX32(2364540158UL & 0x1FFFFFFF, g_msgs[1].id);

    const ap_dbc_sig_t *rpm = &g_sigs[0];

    TEST_ASSERT_EQUAL_STRING("EngineSpeed", rpm->name);
    TEST_ASSERT_EQUAL(24, rpm->start);
    TEST_ASSERT_EQUAL(16, rpm->len);
    TEST_ASSERT_TRUE(rpm->intel);
    TEST_ASSERT_FALSE(rpm->is_signed);
    TEST_ASSERT_EQUAL_DOUBLE(0.125, rpm->factor);
    TEST_ASSERT_EQUAL_STRING("rpm", rpm->unit);
    TEST_ASSERT_EQUAL_DOUBLE(-40.0, g_sigs[1].offset);
    TEST_ASSERT_EQUAL(2, g_sigs[2].mux);       /* M switch             */
    TEST_ASSERT_EQUAL(1, g_sigs[3].mux);       /* m2                   */
    TEST_ASSERT_EQUAL(2, g_sigs[3].mux_val);   /* the N of m2          */
    TEST_ASSERT_FALSE(g_msgs[0].mux_complex);  /* simple mux           */
    TEST_ASSERT_FALSE(g_sigs[4].intel);        /* TorqueBE @0          */
    TEST_ASSERT_TRUE(g_sigs[4].is_signed);
    TEST_ASSERT_EQUAL(1, g_sigs[5].valtype);   /* FloatSig marked      */

    TEST_ASSERT_EQUAL(-1, ap_dbc_parse("BU_ nothing\n", 12, g_msgs, 16,
                                       &nm, g_sigs, 32, err,
                                       sizeof(err)));
}

/** compile -> expression_parser_eval vs the reference decoder. */
static void crosscheck(const ap_dbc_sig_t *s)
{
    char expr[AP_EXPR_LEN];
    const char *reason = NULL;

    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK,
                              ap_dbc_expr(s, expr, sizeof(expr), &reason),
                              s->name);

    static const uint8_t PAYLOADS[][8] =
    {
        { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
        { 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0 },
        { 0x80, 0x7F, 0x01, 0xFE, 0x55, 0xAA, 0xC3, 0x3C },
        { 0x0F, 0xF0, 0x81, 0x18, 0x42, 0x24, 0x99, 0x66 },
    };

    for (size_t i = 0; i < sizeof(PAYLOADS) / sizeof(PAYLOADS[0]); i++)
    {
        double got = 0;
        double want = ap_dbc_decode_ref(s, PAYLOADS[i]);
        char msg[160];

        snprintf(msg, sizeof(msg), "%s expr=%s payload=%zu", s->name,
                 expr, i);
        TEST_ASSERT_EQUAL_MESSAGE(ESP_OK,
                                  expression_parser_eval(expr,
                                                         PAYLOADS[i], 8,
                                                         12.0, &got),
                                  msg);
        TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(1e-6 + fabs(want) * 1e-9, want,
                                          got, msg);
    }
}

static ap_dbc_sig_t mk_sig(const char *name, int start, int len,
                           bool intel, bool sgn, double factor,
                           double offset)
{
    ap_dbc_sig_t s;

    memset(&s, 0, sizeof(s));
    snprintf(s.name, sizeof(s.name), "%s", name);
    s.start = (uint16_t)start;
    s.len = (uint8_t)len;
    s.intel = intel;
    s.is_signed = sgn;
    s.factor = factor;
    s.offset = offset;
    return s;
}

void test_dbc_expr_crosscheck_matrix(void)
{
    ap_dbc_sig_t cases[] =
    {
        /* Intel unsigned */
        mk_sig("i8",        16, 8,  true,  false, 1, 0),
        mk_sig("i16",       24, 16, true,  false, 0.125, 0),
        mk_sig("i16_off",   8,  16, true,  false, 0.01, -100),
        mk_sig("i32",       0,  32, true,  false, 1, 0),
        mk_sig("i4_sub",    12, 4,  true,  false, 1, 0),
        mk_sig("i12_cross", 4,  12, true,  false, 0.5, 0),
        mk_sig("i1_bit",    39, 1,  true,  false, 1, 0),
        /* Intel signed */
        mk_sig("s8_i",      8,  8,  true,  true,  1, 0),
        mk_sig("s16_i",     16, 16, true,  true,  0.1, 0),
        mk_sig("s10_cross", 6,  10, true,  true,  1, -512),
        mk_sig("s32_i",     0,  32, true,  true,  1, 0),
        /* Motorola unsigned */
        mk_sig("m8",        7,  8,  false, false, 1, 0),
        mk_sig("m16_span",  7,  16, false, false, 0.25, 0),
        mk_sig("m12_una",   7,  12, false, false, 1, 0),
        mk_sig("m11_j1939", 3,  11, false, false, 2, 100),
        /* Motorola signed */
        mk_sig("m16_s",     7,  16, false, true,  1, 0),
        mk_sig("m24_s",     7,  24, false, true,  1, 0), /* 3B: generic */
        mk_sig("m32_s",     7,  32, false, true,  0.001, 0),
        mk_sig("m9_s_una",  5,  9,  false, true,  1, 0),
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        crosscheck(&cases[i]);
    }
}

void test_dbc_expr_shapes(void)
{
    char expr[AP_EXPR_LEN];
    const char *reason = NULL;
    ap_dbc_sig_t s;

    s = mk_sig("rpm", 24, 16, true, false, 0.125, 0);
    TEST_ASSERT_EQUAL(ESP_OK, ap_dbc_expr(&s, expr, sizeof(expr),
                                          &reason));
    TEST_ASSERT_EQUAL_STRING("(B3+B4*256)*0.125", expr);

    s = mk_sig("temp", 16, 8, true, false, 1, -40);
    TEST_ASSERT_EQUAL(ESP_OK, ap_dbc_expr(&s, expr, sizeof(expr),
                                          &reason));
    TEST_ASSERT_EQUAL_STRING("B2-40", expr);

    s = mk_sig("be16", 7, 16, false, false, 1, 0);
    TEST_ASSERT_EQUAL(ESP_OK, ap_dbc_expr(&s, expr, sizeof(expr),
                                          &reason));
    TEST_ASSERT_EQUAL_STRING("[B0:B1]", expr);

    s = mk_sig("be16s", 7, 16, false, true, 1, 0);
    TEST_ASSERT_EQUAL(ESP_OK, ap_dbc_expr(&s, expr, sizeof(expr),
                                          &reason));
    TEST_ASSERT_EQUAL_STRING("[S0:S1]", expr);
}

void test_dbc_expr_unsupported(void)
{
    char expr[AP_EXPR_LEN];
    const char *reason = NULL;
    ap_dbc_sig_t s;

    /* simple mux (m<N> / M) now COMPILES — only extended is refused */
    s = mk_sig("mux_ext", 0, 8, true, false, 1, 0);
    s.mux = 3;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED,
                      ap_dbc_expr(&s, expr, sizeof(expr), &reason));
    TEST_ASSERT_TRUE(strstr(reason, "multiplex") != NULL);

    s = mk_sig("mux_ok", 8, 8, true, false, 1, 0);
    s.mux = 1;
    s.mux_val = 2;
    TEST_ASSERT_EQUAL(ESP_OK, ap_dbc_expr(&s, expr, sizeof(expr),
                                          &reason));
    TEST_ASSERT_EQUAL_STRING("B1", expr);

    s = mk_sig("mux_sw", 0, 4, true, false, 1, 0);
    s.mux = 2;
    TEST_ASSERT_EQUAL(ESP_OK, ap_dbc_expr(&s, expr, sizeof(expr),
                                          &reason));
    TEST_ASSERT_EQUAL_STRING("B0&15", expr);

    s = mk_sig("flt", 0, 32, true, false, 1, 0);
    s.valtype = 1;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED,
                      ap_dbc_expr(&s, expr, sizeof(expr), &reason));
    TEST_ASSERT_TRUE(strstr(reason, "float") != NULL);

    /* too long for the config expression cap */
    s = mk_sig("wide", 0, 64, true, true, 0.0001220703125, -3.5);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED,
                      ap_dbc_expr(&s, expr, sizeof(expr), &reason));
    TEST_ASSERT_TRUE(strstr(reason, "long") != NULL);
}

/* ---- multi-ECU response assembly (ap_resp_to_payloads) ------------------- */

void test_resp_multi_ecu_payloads(void)
{
    /* three responders to a functional request, the 7E8 answer
       multi-frame and INTERLEAVED with the others */
    const char *resp =
        "7E9 04 43 01 01 33\r"
        "7E8 10 0A 43 04 01 33 02 44\r"
        "7EA 04 43 01 05 55\r"
        "7E8 21 05 55 C1 07 00 00 00\r";
    ap_resp_ecu_t ecus[AP_RESP_ECUS_MAX];
    int n = ap_resp_to_payloads(resp, ecus, AP_RESP_ECUS_MAX);

    TEST_ASSERT_EQUAL(3, n);
    TEST_ASSERT_EQUAL_HEX32(0x7E8, ecus[0].header);   /* ascending */
    TEST_ASSERT_EQUAL_HEX32(0x7E9, ecus[1].header);
    TEST_ASSERT_EQUAL_HEX32(0x7EA, ecus[2].header);

    TEST_ASSERT_EQUAL(10, ecus[0].len);   /* FF length honored */
    TEST_ASSERT_EQUAL_HEX8(0x43, ecus[0].payload[0]);
    TEST_ASSERT_EQUAL_HEX8(0x07, ecus[0].payload[9]); /* last pair    */
    TEST_ASSERT_EQUAL(4, ecus[1].len);
    TEST_ASSERT_EQUAL_HEX8(0x33, ecus[1].payload[3]);
    TEST_ASSERT_EQUAL(4, ecus[2].len);
    TEST_ASSERT_EQUAL_HEX8(0x55, ecus[2].payload[3]);

    /* headers off — single indistinguishable payload */
    n = ap_resp_to_payloads("41 05 5A\r", ecus, AP_RESP_ECUS_MAX);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL_HEX32(UINT32_MAX, ecus[0].header);
    TEST_ASSERT_EQUAL(3, ecus[0].len);

    /* chip/bus error and emptiness */
    TEST_ASSERT_EQUAL(-1, ap_resp_to_payloads("CAN ERROR\r", ecus,
                                              AP_RESP_ECUS_MAX));
    TEST_ASSERT_EQUAL(0, ap_resp_to_payloads("\r\r", ecus,
                                             AP_RESP_ECUS_MAX));

    /* the single-payload view of the same text still keeps the lowest
       responder only (PID-poll semantics unchanged) */
    uint8_t out[AP_PAYLOAD_MAX];
    size_t sn = 0;

    TEST_ASSERT_EQUAL(ESP_OK,
                      ap_resp_to_payload(resp, out, sizeof(out), &sn));
    TEST_ASSERT_EQUAL(10, sn);
    TEST_ASSERT_EQUAL_HEX8(0x43, out[0]);

    /* 29-bit responders (ISO 15765-4 extended, contiguous-hex header
       shape "18DAF1xx") group per-ECU the same way. The byte-SPACED
       29-bit shape ("18 DA F1 10 ...") is indistinguishable from data
       bytes at line level — a pre-existing parser property, same as
       the legacy lowest-responder rule (documented, TASK_dtc §13.1). */
    const char *resp29 =
        "18DAF118 04 43 01 01 33\r"
        "18DAF110 06 43 02 04 20 01 71\r";
    n = ap_resp_to_payloads(resp29, ecus, AP_RESP_ECUS_MAX);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_HEX32(0x18DAF110, ecus[0].header);
    TEST_ASSERT_EQUAL_HEX32(0x18DAF118, ecus[1].header);
    TEST_ASSERT_EQUAL(6, ecus[0].len);
    TEST_ASSERT_EQUAL_HEX8(0x43, ecus[0].payload[0]);
    TEST_ASSERT_EQUAL(4, ecus[1].len);
    TEST_ASSERT_EQUAL_HEX8(0x33, ecus[1].payload[3]);
}

void test_dtc_merge_codes_dedup(void)
{
    char dst[8][AP_DTC_CODE_LEN] = { "P0420", "P0171" };
    const char src[3][AP_DTC_CODE_LEN] = { "P0171", "U0100", "P0420" };
    uint8_t n = ap_dtc_merge_codes(dst, 2, 8, src, 3);

    TEST_ASSERT_EQUAL(3, n);
    TEST_ASSERT_EQUAL_STRING("P0420", dst[0]);
    TEST_ASSERT_EQUAL_STRING("P0171", dst[1]);
    TEST_ASSERT_EQUAL_STRING("U0100", dst[2]);

    /* cap honored: no room, extra codes dropped */
    const char more[2][AP_DTC_CODE_LEN] = { "C1234", "B0001" };

    n = ap_dtc_merge_codes(dst, n, 3, more, 2);
    TEST_ASSERT_EQUAL(3, n);
}

/* ---- freeze frame (mode 02) — TASK_dtc §14 ------------------------------- */

void test_frz_dtc_parse(void)
{
    char code[AP_DTC_CODE_LEN];

    /* 42 02 00 03 01 -> P0301 */
    const uint8_t good[] = { 0x42, 0x02, 0x00, 0x03, 0x01 };

    TEST_ASSERT_TRUE(ap_frz_dtc(good, sizeof(good), code));
    TEST_ASSERT_EQUAL_STRING("P0301", code);

    /* letter bits: C1234 = 0x52 0x34 */
    const uint8_t chassis[] = { 0x42, 0x02, 0x00, 0x52, 0x34 };

    TEST_ASSERT_TRUE(ap_frz_dtc(chassis, sizeof(chassis), code));
    TEST_ASSERT_EQUAL_STRING("C1234", code);

    /* zeros = no frame stored */
    const uint8_t zeros[] = { 0x42, 0x02, 0x00, 0x00, 0x00 };

    TEST_ASSERT_FALSE(ap_frz_dtc(zeros, sizeof(zeros), code));

    /* cross-talk / short / wrong echo */
    const uint8_t wrong[] = { 0x41, 0x02, 0x00, 0x03, 0x01 };

    TEST_ASSERT_FALSE(ap_frz_dtc(wrong, sizeof(wrong), code));
    TEST_ASSERT_FALSE(ap_frz_dtc(good, 4, code));
    TEST_ASSERT_FALSE(ap_frz_dtc(NULL, 5, code));
}

void test_frz_bitmap_parse(void)
{
    uint32_t bm = 0;

    /* 42 00 00 BE 1F A8 13 (base 0x00) */
    const uint8_t p[] = { 0x42, 0x00, 0x00, 0xBE, 0x1F, 0xA8, 0x13 };

    TEST_ASSERT_TRUE(ap_frz_bitmap(p, sizeof(p), 0x00, &bm));
    TEST_ASSERT_EQUAL_HEX32(0xBE1FA813, bm);

    /* MSB = PID 0x01, bit 20 = PID 0x0C (RPM): 0xBE1FA813 has both */
    TEST_ASSERT_TRUE((bm >> 31) & 1u);
    TEST_ASSERT_TRUE((bm >> (31 - (0x0C - 1))) & 1u);

    /* base echo must match; short payload rejected */
    TEST_ASSERT_FALSE(ap_frz_bitmap(p, sizeof(p), 0x20, &bm));
    TEST_ASSERT_FALSE(ap_frz_bitmap(p, 6, 0x00, &bm));
}

void test_frz_decode_values(void)
{
    ap_frz_val_t vals[8];
    int n = 0;

    /* RPM: 42 0C 00 1A F8 -> 0x1AF8/4 = 1726 */
    const uint8_t rpm[] = { 0x42, 0x0C, 0x00, 0x1A, 0xF8 };

    n = ap_frz_decode(rpm, sizeof(rpm), vals, n, 8);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL_STRING("EngineRPM", vals[0].name);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1726.0f, vals[0].value);

    /* coolant: 42 05 00 7B -> 123 - 40 = 83 (appends after RPM) */
    const uint8_t clt[] = { 0x42, 0x05, 0x00, 0x7B };

    n = ap_frz_decode(clt, sizeof(clt), vals, n, 8);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 83.0f, vals[1].value);

    /* vehicle speed: 42 0D 00 3C -> 60 */
    const uint8_t spd[] = { 0x42, 0x0D, 0x00, 0x3C };

    n = ap_frz_decode(spd, sizeof(spd), vals, n, 8);
    TEST_ASSERT_EQUAL(3, n);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 60.0f, vals[2].value);

    /* wrong echo / short payload / unknown pid: appends nothing */
    const uint8_t wrong[] = { 0x41, 0x0C, 0x00, 0x1A, 0xF8 };
    const uint8_t shrt[] = { 0x42, 0x0C, 0x00, 0x1A };
    const uint8_t unk[] = { 0x42, 0xF9, 0x00, 0x12 };

    TEST_ASSERT_EQUAL(3, ap_frz_decode(wrong, sizeof(wrong), vals, 3, 8));
    TEST_ASSERT_EQUAL(3, ap_frz_decode(shrt, sizeof(shrt), vals, 3, 8));
    TEST_ASSERT_EQUAL(3, ap_frz_decode(unk, sizeof(unk), vals, 3, 8));

    /* cap honored */
    TEST_ASSERT_EQUAL(3, ap_frz_decode(rpm, sizeof(rpm), vals, 3, 3));
}

void test_filter_stream_feed_ex_resume(void)
{
    /* one chunk carrying two matching frames — feed_ex resumes
       mid-chunk so the second frame (another mux page) isn't lost */
    ap_flt_stream_t st;
    const char *chunk = "123 01 02 03\r123 04 05 06\r123 07";
    size_t len = strlen(chunk);
    uint8_t pay[16];
    size_t plen = 0, used = 0, off = 0;

    ap_flt_stream_init(&st);

    TEST_ASSERT_TRUE(ap_flt_stream_feed_ex(&st, (const uint8_t *)chunk,
                                           len, 0x123, pay, sizeof(pay),
                                           &plen, &used));
    TEST_ASSERT_EQUAL(3, plen);
    TEST_ASSERT_EQUAL_HEX8(0x01, pay[0]);
    off += used;

    TEST_ASSERT_TRUE(ap_flt_stream_feed_ex(&st,
                                           (const uint8_t *)chunk + off,
                                           len - off, 0x123, pay,
                                           sizeof(pay), &plen, &used));
    TEST_ASSERT_EQUAL(3, plen);
    TEST_ASSERT_EQUAL_HEX8(0x04, pay[0]);
    off += used;

    /* trailing partial line: consumed to the end, no frame */
    TEST_ASSERT_FALSE(ap_flt_stream_feed_ex(&st,
                                            (const uint8_t *)chunk + off,
                                            len - off, 0x123, pay,
                                            sizeof(pay), &plen, &used));
    TEST_ASSERT_EQUAL(len - off, used);
}

/* ---- DBC simple-mux support ---------------------------------------------- */

void test_dbc_parse_mux_extended_flags(void)
{
    const char *dbc =
        "BO_ 100 ExtMux: 8 E\n"
        " SG_ Sw M : 0|4@1+ (1,0) [0|15] \"\" V\n"
        " SG_ SubSw m1M : 4|4@1+ (1,0) [0|15] \"\" V\n"
        " SG_ Deep m2 : 8|8@1+ (1,0) [0|255] \"\" V\n"
        "BO_ 200 TwoSw: 8 E\n"
        " SG_ SwA M : 0|4@1+ (1,0) [0|15] \"\" V\n"
        " SG_ SwB M : 4|4@1+ (1,0) [0|15] \"\" V\n"
        "BO_ 300 MulVal: 8 E\n"
        " SG_ Sel M : 0|8@1+ (1,0) [0|255] \"\" V\n"
        " SG_ S1 m0 : 8|8@1+ (1,0) [0|255] \"\" V\n"
        "SG_MUL_VAL_ 300 S1 Sel 0-0;\n";
    char err[96];
    int nm = 0;
    int ns = ap_dbc_parse(dbc, strlen(dbc), g_msgs, 16, &nm, g_sigs, 32,
                          err, sizeof(err));

    TEST_ASSERT_EQUAL(7, ns);
    TEST_ASSERT_EQUAL(3, nm);
    TEST_ASSERT_TRUE(g_msgs[0].mux_complex);   /* m1M token            */
    TEST_ASSERT_EQUAL(3, g_sigs[1].mux);       /* SubSw = extended     */
    TEST_ASSERT_TRUE(g_msgs[1].mux_complex);   /* two M switches       */
    TEST_ASSERT_TRUE(g_msgs[2].mux_complex);   /* SG_MUL_VAL_ map      */
}

void test_dbc_mux_cond(void)
{
    ap_dbc_msg_t msgs[1];
    ap_dbc_sig_t sigs[3];
    char mexpr[AP_MUX_EXPR_LEN];
    float mval = -1;
    const char *reason = NULL;

    memset(msgs, 0, sizeof(msgs));
    sigs[0] = mk_sig("Sw", 0, 4, true, false, 1, 0);
    sigs[0].mux = 2;
    sigs[1] = mk_sig("Val", 8, 8, true, false, 0.5, 0);
    sigs[1].mux = 1;
    sigs[1].mux_val = 3;
    sigs[2] = mk_sig("Plain", 16, 8, true, false, 1, 0);

    /* plain + the switch itself: unconditional */
    TEST_ASSERT_EQUAL(ESP_OK,
                      ap_dbc_mux_cond(&sigs[2], msgs, sigs, 3, mexpr,
                                      sizeof(mexpr), &mval, &reason));
    TEST_ASSERT_EQUAL_STRING("", mexpr);
    TEST_ASSERT_EQUAL(ESP_OK,
                      ap_dbc_mux_cond(&sigs[0], msgs, sigs, 3, mexpr,
                                      sizeof(mexpr), &mval, &reason));
    TEST_ASSERT_EQUAL_STRING("", mexpr);

    /* the m3 signal: raw switch slice + expected value */
    TEST_ASSERT_EQUAL(ESP_OK,
                      ap_dbc_mux_cond(&sigs[1], msgs, sigs, 3, mexpr,
                                      sizeof(mexpr), &mval, &reason));
    TEST_ASSERT_EQUAL_STRING("B0&15", mexpr);
    TEST_ASSERT_EQUAL_FLOAT(3.0f, mval);

    /* the comparison is on the RAW switch value even when the switch
       itself is scaled/signed in the DBC */
    sigs[0].factor = 0.25;
    sigs[0].offset = -2;
    sigs[0].is_signed = true;
    TEST_ASSERT_EQUAL(ESP_OK,
                      ap_dbc_mux_cond(&sigs[1], msgs, sigs, 3, mexpr,
                                      sizeof(mexpr), &mval, &reason));
    TEST_ASSERT_EQUAL_STRING("B0&15", mexpr);

    /* the gate evaluates against real payloads the way the runner does */
    const uint8_t page3[8] = { 0x03, 0x42, 0, 0, 0, 0, 0, 0 };
    const uint8_t page4[8] = { 0x04, 0x42, 0, 0, 0, 0, 0, 0 };
    double mv = 0;

    TEST_ASSERT_EQUAL(ESP_OK,
                      expression_parser_eval(mexpr, page3, 8, 12.0, &mv));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 3.0, mv);
    TEST_ASSERT_EQUAL(ESP_OK,
                      expression_parser_eval(mexpr, page4, 8, 12.0, &mv));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 4.0, mv);

    /* switchless m<N> signal */
    sigs[0].mux = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED,
                      ap_dbc_mux_cond(&sigs[1], msgs, sigs, 3, mexpr,
                                      sizeof(mexpr), &mval, &reason));
    TEST_ASSERT_TRUE(strstr(reason, "switch") != NULL);
    sigs[0].mux = 2;

    /* extended-multiplexing message */
    msgs[0].mux_complex = true;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED,
                      ap_dbc_mux_cond(&sigs[1], msgs, sigs, 3, mexpr,
                                      sizeof(mexpr), &mval, &reason));
    TEST_ASSERT_TRUE(strstr(reason, "multiplex") != NULL);
}

void test_dbc_muxed_value_crosscheck(void)
{
    /* a muxed signal's VALUE expression compiles exactly like a plain
       one — cross-check it against the reference decoder */
    ap_dbc_sig_t s = mk_sig("mx", 8, 12, true, false, 0.1, -10);

    s.mux = 1;
    s.mux_val = 5;
    crosscheck(&s);
}

void test_config_parse_mux_param(void)
{
    const char *json =
        "{\"filters\":[{\"frame_id\":291,\"monitor_ms\":800,"
        "\"parameters\":[{\"name\":\"gear\",\"expression\":\"B1\","
        "\"mux_expr\":\"B0&15\",\"mux_val\":2}]}]}";
    /* ~1 MB struct (2048-param pool) — STATIC, never on the task stack
     * (the linux-port task stack is FreeRTOS-sized; x86-64 frames blew
     * it and the suite segfaulted in CI, 2026-07-27; §2 discipline) */
    static ap_config_t cfg;
    char err[96] = "";

    TEST_ASSERT_EQUAL(ESP_OK,
                      ap_config_parse(json, &cfg, err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("B0&15", cfg.params[0].mux_expr);
    TEST_ASSERT_EQUAL_FLOAT(2.0f, cfg.params[0].mux_val);

    /* absent = unconditional */
    const char *plain =
        "{\"filters\":[{\"frame_id\":291,\"monitor_ms\":800,"
        "\"parameters\":[{\"name\":\"soc\",\"expression\":\"B4/2\"}]}]}";

    TEST_ASSERT_EQUAL(ESP_OK,
                      ap_config_parse(plain, &cfg, err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("", cfg.params[0].mux_expr);

    /* a bad mux expression is rejected like a bad value expression */
    const char *bad =
        "{\"filters\":[{\"frame_id\":291,\"monitor_ms\":800,"
        "\"parameters\":[{\"name\":\"gear\",\"expression\":\"B1\","
        "\"mux_expr\":\"[B3:B0]\"}]}]}";

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ap_config_parse(bad, &cfg, err, sizeof(err)));
    TEST_ASSERT_TRUE(strstr(err, "mux") != NULL);
}

/* one PID whose reply decodes into n parameters (c0..c<n-1>) */
static void build_wide_pid_json(char *out, size_t cap, int n)
{
    size_t off = (size_t)snprintf(out, cap,
                                  "{\"pids\":[{\"name\":\"bms\","
                                  "\"cmd\":\"220102\",\"parameters\":[");

    for (int i = 0; i < n; i++)
    {
        off += (size_t)snprintf(out + off, cap - off,
                                "%s{\"name\":\"c%d\",\"expression\":\"B%d\"}",
                                i ? "," : "", i, i % 60);
    }

    snprintf(out + off, cap - off, "]}]}");
}

void test_config_parse_wide_pid(void)
{
    /* REGRESSION (2026-09-16): the published Hyundai/Kia BMS DIDs decode
       20–32 values from ONE reply and Xpeng's cell-voltage DID 192; the
       old per-PID cap of 16 rejected every such profile on import with
       "220105: more than 16 parameters". */
    static ap_config_t cfg;          /* ~1 MB: static, never on the stack */
    static char json[16384];
    char err[96] = "";

    /* Ioniq5/6 220102: 32 parameters parse into one contiguous slice */
    build_wide_pid_json(json, sizeof(json), 32);
    TEST_ASSERT_EQUAL(ESP_OK, ap_config_parse(json, &cfg, err, sizeof(err)));
    TEST_ASSERT_EQUAL(1, cfg.n_pids);
    TEST_ASSERT_EQUAL(32, cfg.pids[0].param_count);
    TEST_ASSERT_EQUAL(32, cfg.n_params);
    TEST_ASSERT_EQUAL(0, cfg.pids[0].param_start);
    TEST_ASSERT_EQUAL_STRING("c31", cfg.params[31].name);

    /* the widest published PID (Xpeng 221122) fits */
    build_wide_pid_json(json, sizeof(json), 192);
    TEST_ASSERT_EQUAL(ESP_OK, ap_config_parse(json, &cfg, err, sizeof(err)));
    TEST_ASSERT_EQUAL(192, cfg.pids[0].param_count);

    /* the cap itself still exists and is reported, not silently clipped */
    build_wide_pid_json(json, sizeof(json), AP_PARAMS_PER + 1);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ap_config_parse(json, &cfg, err, sizeof(err)));
    TEST_ASSERT_TRUE(strstr(err, "more than") != NULL);
    TEST_ASSERT_EQUAL(0, cfg.n_pids); /* tables wiped on failure */
}

/* ---- yield-to-app window (legacy DEV_AUTOPID_ELM327_APP_BIT parity) ---------- */

static void test_client_hold_window(void)
{
    /* an app that just wrote holds the chip; silence past the window
       releases it; "never wrote" (UINT32_MAX) never holds */
    TEST_ASSERT_TRUE(ap_sched_client_hold(0));
    TEST_ASSERT_TRUE(ap_sched_client_hold(AP_CLIENT_YIELD_MS - 1));
    TEST_ASSERT_FALSE(ap_sched_client_hold(AP_CLIENT_YIELD_MS));
    TEST_ASSERT_FALSE(ap_sched_client_hold(AP_CLIENT_YIELD_MS + 1));
    TEST_ASSERT_FALSE(ap_sched_client_hold(UINT32_MAX));
    TEST_ASSERT_EQUAL_UINT32(10000, AP_CLIENT_YIELD_MS); /* the legacy 10 s */
}

void app_main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_sched_one_request_per_pid_per_period);
    RUN_TEST(test_sched_group_inheritance_and_override);
    RUN_TEST(test_sched_group_toggle_gates_entries);
    RUN_TEST(test_sched_type_gates);
    RUN_TEST(test_sched_high_fidelity_round_robin);
    RUN_TEST(test_sched_fail_backoff);
    RUN_TEST(test_sched_stagger);

    RUN_TEST(test_resp_headers_off_single);
    RUN_TEST(test_resp_searching_then_data);
    RUN_TEST(test_resp_headers_off_isotp_multiline);
    RUN_TEST(test_resp_headers_on_single);
    RUN_TEST(test_resp_headers_on_lowest_responder_wins);
    RUN_TEST(test_resp_headers_on_isotp_multiframe);
    RUN_TEST(test_resp_errors);
    RUN_TEST(test_resp_payload_matches_cmd);
    RUN_TEST(test_resp_multi_ecu_payloads);

    RUN_TEST(test_filter_frame_shapes);
    RUN_TEST(test_filter_stream_busy_bus);
    RUN_TEST(test_filter_stream_duplicates_first_wins);
    RUN_TEST(test_filter_stream_mixed_dlc_bus);
    RUN_TEST(test_filter_short_dlc_expression_bounds);
    RUN_TEST(test_filter_stream_truncation_recovers);
    RUN_TEST(test_filter_stream_feed_ex_resume);
    RUN_TEST(test_config_filter_monitor_bounds);

    RUN_TEST(test_std_expression_mapping);
    RUN_TEST(test_std_expression_whole_table_parses);
    RUN_TEST(test_std_scan_parse_shapes);

    RUN_TEST(test_config_parse_happy);
    RUN_TEST(test_config_parse_rejects_bad_expression);
    RUN_TEST(test_config_parse_rejects_unknown_group_and_dupes);
    RUN_TEST(test_config_parse_empty_and_garbage);
    RUN_TEST(test_init_sanitize);
    RUN_TEST(test_config_parse_sanitizes_cmd_and_init);
    RUN_TEST(test_config_parse_mux_param);
    RUN_TEST(test_config_parse_wide_pid);

    RUN_TEST(test_dtc_format_all_letters);
    RUN_TEST(test_dtc_unformat_roundtrip_and_rejects);
    RUN_TEST(test_dtc_parse_codes_shapes);
    RUN_TEST(test_dtc_parse_mil);
    RUN_TEST(test_dtc_clear_condition_matrix);
    RUN_TEST(test_dtc_clear_mode_parse);
    RUN_TEST(test_dtc_merge_codes_dedup);
    RUN_TEST(test_frz_dtc_parse);
    RUN_TEST(test_frz_bitmap_parse);
    RUN_TEST(test_frz_decode_values);

    RUN_TEST(test_dtcdb_csv_variants);
    RUN_TEST(test_dtcdb_json_shapes);
    RUN_TEST(test_dtcdb_dedup_sort_suffix);
    RUN_TEST(test_dtcdb_rejects_and_edges);
    RUN_TEST(test_dtcdb_search_match);

    RUN_TEST(test_dbc_parse_fixture);
    RUN_TEST(test_dbc_expr_crosscheck_matrix);
    RUN_TEST(test_dbc_expr_shapes);
    RUN_TEST(test_dbc_expr_unsupported);
    RUN_TEST(test_dbc_parse_mux_extended_flags);
    RUN_TEST(test_dbc_mux_cond);
    RUN_TEST(test_dbc_muxed_value_crosscheck);

    RUN_TEST(test_client_hold_window);

    UNITY_END();
}
