/**
 * @file test_main.c
 * @brief Host tests for data_logger's pure helpers: strict file-name
 *        parse (both stream prefixes), make/parse roundtrip per
 *        engine, the prefix-filtered directory scan fold, the hex
 *        filter parser, and the record/row encoders for every
 *        non-sqlite engine (.wdl, csv, candump, asc, jsonl rows +
 *        MDF4 prelude/record + BLF builders: byte-level golden
 *        vectors); the recovery decisions (test_recover.c) and the
 *        clock rules (test_clock.c). Expected: 40 Tests 0 Failures
 *        0 Ignored.
 */
#include <string.h>

#include "unity.h"

#include "data_logger_private.h"

/* test_recover.c */
void test_text_keep_whole_when_newline_terminated(void);
void test_text_keep_drops_the_torn_last_line(void);
void test_wdl_scan_counts_complete_records_only(void);
void test_wdl_scan_flags_garbage(void);
void test_corrupt_names_round_trip(void);
void test_record_sanity(void);
void test_ring_sanity_and_fill_recovery(void);
void test_crc32_known_vector(void);

/* test_clock.c */
void test_record_layout_is_32_bytes_with_the_stamps_in_the_padding(void);
void test_rebase_adds_the_steps_that_came_after_the_record(void);
void test_rebase_leaves_absolute_records_alone(void);
void test_rebase_compares_the_stamps_modulo_the_wrap(void);
void test_relative_time_comes_from_the_monotonic_clock(void);
void test_rotation_rule(void);
void test_journal_lines_byte_exact(void);
void test_markers_byte_exact(void);
void test_wdl_scan_accepts_the_step_record(void);
void test_journal_trim_cuts_at_a_line_boundary(void);
void test_relative_rows_take_the_offset(void);
void test_mf4_prelude_keeps_the_start_at_the_patch_offsets(void);

void setUp(void)
{
}

void tearDown(void)
{
}

static void test_parse_accepts_all_engine_extensions(void)
{
    int64_t e = 0;

    TEST_ASSERT_TRUE(dl_files_parse("dl_1783000000.db", &e));
    TEST_ASSERT_EQUAL_INT64(1783000000LL, e);
    TEST_ASSERT_TRUE(dl_files_parse("dl_0000000000.csv", &e));
    TEST_ASSERT_EQUAL_INT64(0, e);
    TEST_ASSERT_TRUE(dl_files_parse("dl_1783000001.wdl", &e));
    TEST_ASSERT_EQUAL_INT64(1783000001LL, e);
}

static void test_parse_accepts_can_prefix(void)
{
    int64_t e = 0;

    TEST_ASSERT_TRUE(dl_files_parse("can_1783000002.wdl", &e));
    TEST_ASSERT_EQUAL_INT64(1783000002LL, e);
    TEST_ASSERT_TRUE(dl_files_parse("can_1783000003.csv", &e));
    TEST_ASSERT_TRUE(dl_files_parse("can_1783000004.db", &e));
}

static void test_parse_rejects_foreign_names(void)
{
    TEST_ASSERT_FALSE(dl_files_parse("dl_1783000000.txt", NULL));
    TEST_ASSERT_FALSE(dl_files_parse("dl_178300000.db", NULL));   /* 9 */
    TEST_ASSERT_FALSE(dl_files_parse("dl_17830000000.db", NULL)); /* 11 */
    TEST_ASSERT_FALSE(dl_files_parse("dl_178300000x.db", NULL));
    TEST_ASSERT_FALSE(dl_files_parse("xl_1783000000.db", NULL));
    TEST_ASSERT_FALSE(dl_files_parse("dl_1783000000.db.bak", NULL));
    TEST_ASSERT_FALSE(dl_files_parse("can_178300000.wdl", NULL)); /* 9 */
    TEST_ASSERT_FALSE(dl_files_parse("canx1783000000.wdl", NULL));
    TEST_ASSERT_FALSE(dl_files_parse("System Volume Information",
                                     NULL));
    TEST_ASSERT_FALSE(dl_files_parse(".", NULL));
    TEST_ASSERT_FALSE(dl_files_parse(NULL, NULL));
    /* the clock journal is not a log file: neither stream folds it */
    TEST_ASSERT_FALSE(dl_files_parse("clock.jsonl", NULL));
}

static void test_make_parse_roundtrip(void)
{
    static const char *const EXTS[] = { ".db", ".csv", ".wdl" };
    static const char *const PFX[] = { DL_PREFIX_PARAM, DL_PREFIX_CAN };

    for (int p = 0; p < 2; p++)
    {
        for (int i = 0; i < 3; i++)
        {
            char name[48];
            int64_t e = 0;

            dl_files_make(name, sizeof(name), PFX[p], 1783000123LL,
                          EXTS[i]);
            TEST_ASSERT_TRUE(dl_files_parse(name, &e));
            TEST_ASSERT_EQUAL_INT64(1783000123LL, e);
        }
    }
}

static void test_make_zero_pads_for_lexical_order(void)
{
    char young[48];
    char old[48];

    dl_files_make(old, sizeof(old), DL_PREFIX_PARAM, 999LL, ".db");
    dl_files_make(young, sizeof(young), DL_PREFIX_PARAM, 1783000000LL,
                  ".db");
    TEST_ASSERT_EQUAL_STRING("dl_0000000999.db", old);
    TEST_ASSERT_TRUE(strcmp(old, young) < 0);
}

static void test_make_clamps_negative_epoch(void)
{
    char name[48];

    dl_files_make(name, sizeof(name), DL_PREFIX_CAN, -5LL, ".csv");
    TEST_ASSERT_EQUAL_STRING("can_0000000000.csv", name);
}

/* where an export starts (2026-11-06): an exact cursor resumes at its
   file with its offset, a retired one at the next newer file from 0; a
   plain epoch starts at the file that was open at that moment */
/* the export's window walk (2026-11-06): the sink of the test */
static char s_walk_out[1024];
static size_t s_walk_len;
static int s_walk_lines;
static int s_walk_bytes;

static bool walk_send(void *ctx, const char *data, size_t len)
{
    (void)ctx;

    if (s_walk_len + len > sizeof(s_walk_out))
    {
        return false;
    }

    memcpy(s_walk_out + s_walk_len, data, len);
    s_walk_len += len;
    return true;
}

static size_t walk(const char *win, size_t n, const char *name, int lines,
                   int bytes, bool *stop, bool *aborted)
{
    s_walk_len = 0;
    s_walk_lines = lines;
    s_walk_bytes = bytes;
    *aborted = false;
    return dl_export_walk(win, n, name, false, 500, &s_walk_lines,
                          &s_walk_bytes, walk_send, NULL, stop, aborted);
}

/* a budget spent exactly at a window start consumes nothing and stops: it
   is not a window without a line boundary (bench run 5, 2026-11-06: such
   a window went out raw and its cut tail row glued to the cursor line) */
static void test_export_walk_budget(void)
{
    static const char WIN[] =
        "{\"ts\":1,\"param\":\"a.x\",\"value\":1}\n"
        "{\"ts\":2,\"param\":\"a.y\",\"value\":2}\n"
        "{\"ts\":3,\"param\":\"a.x\",\"value\":3}\n"
        "{\"ts\":4,\"param\":\"a.x\",\"va";          /* the cut tail */
    const size_t n = sizeof(WIN) - 1;
    const size_t line = strlen("{\"ts\":1,\"param\":\"a.x\",\"value\":1}\n");
    bool stop;
    bool aborted;

    /* a whole window: the three complete lines go out, the tail stays */
    TEST_ASSERT_EQUAL_UINT(3 * line, walk(WIN, n, "", 500, 65536, &stop, &aborted));
    TEST_ASSERT_FALSE(stop);
    TEST_ASSERT_EQUAL_INT(497, s_walk_lines);
    TEST_ASSERT_EQUAL_UINT(3 * line, s_walk_len);

    /* the budget runs out at the window start, a line already sent in this
       request: nothing consumed, nothing sent, stop (the defect's case) */
    TEST_ASSERT_EQUAL_UINT(0, walk(WIN, n, "", 499, (int)line - 1, &stop, &aborted));
    TEST_ASSERT_TRUE(stop);
    TEST_ASSERT_EQUAL_UINT(0, s_walk_len);
    TEST_ASSERT_FALSE(aborted);

    /* the first line of a request always goes out, however small the budget */
    TEST_ASSERT_EQUAL_UINT(line, walk(WIN, n, "", 500, 10, &stop, &aborted));
    TEST_ASSERT_TRUE(stop);
    TEST_ASSERT_EQUAL_UINT(line, s_walk_len);

    /* the budget runs out after the second line: two sent, stop */
    TEST_ASSERT_EQUAL_UINT(2 * line, walk(WIN, n, "", 500, (int)(2 * line), &stop, &aborted));
    TEST_ASSERT_TRUE(stop);
    TEST_ASSERT_EQUAL_UINT(2 * line, s_walk_len);

    /* the name filter: only a.y goes out, the offsets still cover every line */
    TEST_ASSERT_EQUAL_UINT(3 * line, walk(WIN, n, "y", 500, 65536, &stop, &aborted));
    TEST_ASSERT_FALSE(stop);
    TEST_ASSERT_EQUAL_INT(499, s_walk_lines);
    TEST_ASSERT_EQUAL_UINT(line, s_walk_len);
    TEST_ASSERT_EQUAL_STRING_LEN("{\"ts\":2", s_walk_out, 7);

    /* the line cap */
    TEST_ASSERT_EQUAL_UINT(2 * line, walk(WIN, n, "", 2, 65536, &stop, &aborted));
    TEST_ASSERT_TRUE(stop);
    TEST_ASSERT_EQUAL_INT(0, s_walk_lines);

    /* the sink refuses (the client went away): aborted, stop */
    s_walk_lines = 500;
    s_walk_bytes = 65536;
    s_walk_len = sizeof(s_walk_out);
    aborted = false;
    TEST_ASSERT_EQUAL_UINT(0, dl_export_walk(WIN, n, "", false, 500, &s_walk_lines,
                                             &s_walk_bytes, walk_send, NULL, &stop,
                                             &aborted));
    TEST_ASSERT_TRUE(aborted);
    TEST_ASSERT_TRUE(stop);
}

static void test_export_start_rule(void)
{
    const int64_t epochs[] = { 1000, 2000, 3000 };
    long off = 77;

    /* nothing / 0: the oldest file, offset 0 */
    TEST_ASSERT_EQUAL_INT(0, dl_export_start(epochs, 3, 0, false, &off));
    TEST_ASSERT_EQUAL_INT(0, off);
    off = 77;
    TEST_ASSERT_EQUAL_INT(0, dl_export_start(epochs, 3, 0, true, &off));
    TEST_ASSERT_EQUAL_INT(0, off);

    /* exact: a live file keeps its offset */
    off = 77;
    TEST_ASSERT_EQUAL_INT(1, dl_export_start(epochs, 3, 2000, true, &off));
    TEST_ASSERT_EQUAL_INT(77, off);
    /* exact: a retired epoch resumes at the next newer file from 0 */
    off = 77;
    TEST_ASSERT_EQUAL_INT(2, dl_export_start(epochs, 3, 2500, true, &off));
    TEST_ASSERT_EQUAL_INT(0, off);
    /* exact: newer than every file = nothing to read (n) */
    off = 77;
    TEST_ASSERT_EQUAL_INT(3, dl_export_start(epochs, 3, 9000, true, &off));
    TEST_ASSERT_EQUAL_INT(0, off);

    /* plain: the newest file opened at or before the moment, from 0 */
    off = 77;
    TEST_ASSERT_EQUAL_INT(1, dl_export_start(epochs, 3, 2500, false, &off));
    TEST_ASSERT_EQUAL_INT(0, off);
    off = 77;
    TEST_ASSERT_EQUAL_INT(1, dl_export_start(epochs, 3, 2000, false, &off));
    TEST_ASSERT_EQUAL_INT(0, off);
    TEST_ASSERT_EQUAL_INT(2, dl_export_start(epochs, 3, 9000, false, &off));
    /* plain: before every file = the oldest */
    TEST_ASSERT_EQUAL_INT(0, dl_export_start(epochs, 3, 500, false, &off));
    /* no files at all */
    TEST_ASSERT_EQUAL_INT(0, dl_export_start(epochs, 0, 2000, true, &off));
}

static void test_scan_tracks_oldest_newest_count(void)
{
    dl_scan_t s;

    dl_scan_init(&s, DL_PREFIX_PARAM);
    TEST_ASSERT_EQUAL_INT(0, s.count);

    dl_scan_add(&s, "dl_1783000005.db");
    dl_scan_add(&s, "dl_1783000001.csv");
    dl_scan_add(&s, "dl_1783000009.wdl");
    TEST_ASSERT_EQUAL_INT(3, s.count);
    TEST_ASSERT_EQUAL_STRING("dl_1783000001.csv", s.oldest);
    TEST_ASSERT_EQUAL_STRING("dl_1783000009.wdl", s.newest);
}

static void test_scan_ignores_foreign_entries(void)
{
    dl_scan_t s;

    dl_scan_init(&s, DL_PREFIX_PARAM);
    dl_scan_add(&s, ".");
    dl_scan_add(&s, "..");
    dl_scan_add(&s, "notes.txt");
    dl_scan_add(&s, "clock.jsonl");
    dl_scan_add(&s, "dl_1783000005.db");
    dl_scan_add(&s, "dl_bogus.db");
    TEST_ASSERT_EQUAL_INT(1, s.count);
    TEST_ASSERT_EQUAL_STRING("dl_1783000005.db", s.oldest);
    TEST_ASSERT_EQUAL_STRING("dl_1783000005.db", s.newest);
}

static void test_scan_isolates_streams_by_prefix(void)
{
    dl_scan_t s;

    /* a param-stream scan must not count/age the CAN stream's files
     * (retention runs per stream), and vice versa */
    dl_scan_init(&s, DL_PREFIX_PARAM);
    dl_scan_add(&s, "dl_1783000005.db");
    dl_scan_add(&s, "can_1783000001.wdl");
    dl_scan_add(&s, "can_1783000009.wdl");
    TEST_ASSERT_EQUAL_INT(1, s.count);
    TEST_ASSERT_EQUAL_STRING("dl_1783000005.db", s.newest);

    dl_scan_init(&s, DL_PREFIX_CAN);
    dl_scan_add(&s, "dl_1783000005.db");
    dl_scan_add(&s, "can_1783000001.wdl");
    dl_scan_add(&s, "can_1783000009.wdl");
    TEST_ASSERT_EQUAL_INT(2, s.count);
    TEST_ASSERT_EQUAL_STRING("can_1783000001.wdl", s.oldest);
    TEST_ASSERT_EQUAL_STRING("can_1783000009.wdl", s.newest);
}

static void test_hex_parse(void)
{
    uint32_t v = 0;

    TEST_ASSERT_TRUE(dl_parse_hex_u32("7E8", &v));
    TEST_ASSERT_EQUAL_UINT32(0x7E8, v);
    TEST_ASSERT_TRUE(dl_parse_hex_u32("0x7e8", &v));
    TEST_ASSERT_EQUAL_UINT32(0x7E8, v);
    TEST_ASSERT_TRUE(dl_parse_hex_u32("1FFFFFFF", &v));
    TEST_ASSERT_EQUAL_UINT32(0x1FFFFFFFu, v);
    TEST_ASSERT_TRUE(dl_parse_hex_u32("0", &v));
    TEST_ASSERT_EQUAL_UINT32(0, v);

    TEST_ASSERT_FALSE(dl_parse_hex_u32("", &v));
    TEST_ASSERT_FALSE(dl_parse_hex_u32("0x", &v));
    TEST_ASSERT_FALSE(dl_parse_hex_u32("7G8", &v));
    TEST_ASSERT_FALSE(dl_parse_hex_u32("123456789", &v)); /* 9 digits */
    TEST_ASSERT_FALSE(dl_parse_hex_u32(NULL, &v));
}

static void test_wdl_frame_encoding_golden(void)
{
    /* std id 0x7E8, dlc 3, ts 0x0102030405060708 */
    static const uint8_t DATA[] = { 0xAA, 0xBB, 0xCC };
    uint8_t buf[24];
    size_t n = dl_wdl_encode_frame(buf, 0x0102030405060708LL, 0x7E8, 0,
                                   DATA, 3);

    TEST_ASSERT_EQUAL_size_t(1 + 8 + 4 + 1 + 3, n);
    TEST_ASSERT_EQUAL_HEX8(0x03, buf[0]);
    /* ts little-endian */
    TEST_ASSERT_EQUAL_HEX8(0x08, buf[1]);
    TEST_ASSERT_EQUAL_HEX8(0x01, buf[8]);
    /* id word LE: 0x000007E8 */
    TEST_ASSERT_EQUAL_HEX8(0xE8, buf[9]);
    TEST_ASSERT_EQUAL_HEX8(0x07, buf[10]);
    TEST_ASSERT_EQUAL_HEX8(0x00, buf[11]);
    TEST_ASSERT_EQUAL_HEX8(0x00, buf[12]);
    TEST_ASSERT_EQUAL_HEX8(3, buf[13]);
    TEST_ASSERT_EQUAL_HEX8(0xAA, buf[14]);
    TEST_ASSERT_EQUAL_HEX8(0xCC, buf[16]);
}

static void test_wdl_frame_encoding_flags(void)
{
    /* ext+rtr 29-bit id 0x18DAF110, dlc 0 */
    uint8_t buf[24];
    size_t n = dl_wdl_encode_frame(buf, 0, 0x18DAF110,
                                   DL_FRAME_EXT | DL_FRAME_RTR, NULL, 0);

    TEST_ASSERT_EQUAL_size_t(14, n);
    /* id word = 0x18DAF110 | 0x80000000 | 0x40000000 = 0xD8DAF110 */
    TEST_ASSERT_EQUAL_HEX8(0x10, buf[9]);
    TEST_ASSERT_EQUAL_HEX8(0xF1, buf[10]);
    TEST_ASSERT_EQUAL_HEX8(0xDA, buf[11]);
    TEST_ASSERT_EQUAL_HEX8(0xD8, buf[12]);
    TEST_ASSERT_EQUAL_HEX8(0, buf[13]);
}

static void test_csv_frame_row(void)
{
    static const uint8_t DATA[] = { 0x01, 0x02, 0xAB };
    char row[64];
    int n = dl_csv_frame_row(row, sizeof(row), 1783000000123LL, 0x7E8,
                             0, DATA, 3);

    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL_STRING("1783000000123,7E8,0,0,3,0102AB\n", row);

    n = dl_csv_frame_row(row, sizeof(row), 5, 0x18DAF110,
                         DL_FRAME_EXT, DATA, 2);
    TEST_ASSERT_EQUAL_STRING("5,18DAF110,1,0,2,0102\n", row);

    /* too-small buffer refuses instead of truncating */
    TEST_ASSERT_EQUAL_INT(-1,
                          dl_csv_frame_row(row, 10, 5, 0x7E8, 0, DATA,
                                           3));
}

/* ---- addendum-2 formats ------------------------------------------------------ */

static void test_new_extensions_parse(void)
{
    TEST_ASSERT_TRUE(dl_files_parse("can_1783000010.log", NULL));
    TEST_ASSERT_TRUE(dl_files_parse("can_1783000011.asc", NULL));
    TEST_ASSERT_TRUE(dl_files_parse("can_1783000012.mf4", NULL));
    TEST_ASSERT_TRUE(dl_files_parse("can_1783000013.blf", NULL));
    TEST_ASSERT_TRUE(dl_files_parse("dl_1783000014.jsonl", NULL));
    TEST_ASSERT_FALSE(dl_files_parse("can_1783000015.mdf", NULL));
}

static void test_candump_row_golden(void)
{
    static const uint8_t DATA[] = { 0x01, 0x02, 0xAB };
    char row[64];
    int n = dl_candump_row(row, sizeof(row), 1783000000123LL, 0x7E8, 0,
                           DATA, 3);

    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL_STRING("(1783000000.123000) can0 7E8#0102AB\n",
                             row);

    /* extended + RTR */
    n = dl_candump_row(row, sizeof(row), 5000, 0x18DAF110,
                       DL_FRAME_EXT | DL_FRAME_RTR, NULL, 0);
    TEST_ASSERT_EQUAL_STRING("(5.000000) can0 18DAF110#R\n", row);
}

static void test_asc_row_golden(void)
{
    static const uint8_t DATA[] = { 0x01, 0x02, 0xAB };
    char row[80];
    int n = dl_asc_row(row, sizeof(row), 1234, 0x7E8, 0, DATA, 3);

    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL_STRING(
        "   1.234000 1  7E8             Rx   d 3 01 02 AB\n", row);

    n = dl_asc_row(row, sizeof(row), 500, 0x1F334455, DL_FRAME_EXT,
                   DATA, 2);
    TEST_ASSERT_EQUAL_STRING(
        "   0.500000 1  1F334455x       Rx   d 2 01 02\n", row);
}

static void test_jsonl_rows_golden(void)
{
    static const uint8_t DATA[] = { 0xDE, 0xAD };
    char row[128];
    int n = dl_jsonl_param_row(row, sizeof(row), 1783000000123LL,
                               "test", "value", 0.5);

    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL_STRING(
        "{\"ts\":1783000000123,\"param\":\"test.value\","
        "\"value\":0.5}\n", row);

    n = dl_jsonl_frame_row(row, sizeof(row), 7, 0x7E8, DL_FRAME_EXT,
                           DATA, 2);
    TEST_ASSERT_EQUAL_STRING(
        "{\"ts\":7,\"id\":\"7E8\",\"ext\":1,\"rtr\":0,"
        "\"data\":\"DEAD\"}\n", row);
}

static void test_mf4_prelude_structure(void)
{
    static uint8_t buf[DL_MF4_PRELUDE_MAX];
    uint32_t dt_len_off = 0;
    uint32_t cg_cycle_off = 0;
    size_t n = dl_mf4_prelude(buf, sizeof(buf), 1783000000000LL,
                              &dt_len_off, &cg_cycle_off);

    TEST_ASSERT_GREATER_THAN(600, (int)n);
    /* ID block magics */
    TEST_ASSERT_EQUAL_MEMORY("MDF     ", buf, 8);
    TEST_ASSERT_EQUAL_MEMORY("4.10    ", buf + 8, 8);
    /* HD at 64, DT header closes the prelude */
    TEST_ASSERT_EQUAL_MEMORY("##HD", buf + 64, 4);
    TEST_ASSERT_EQUAL_MEMORY("##DT", buf + n - 24, 4);
    /* the patch offsets land inside their blocks */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(n - 24 + 8), dt_len_off);
    TEST_ASSERT_EQUAL_MEMORY("##CG", buf + cg_cycle_off - 8 - 48 - 24,
                             4);
    /* start time ns in HD data section */
    uint64_t ns = 0;

    memcpy(&ns, buf + 64 + 24 + 48, 8);
    TEST_ASSERT_EQUAL_UINT64(1783000000000ULL * 1000000ULL, ns);
}

static void test_mf4_record_golden(void)
{
    dl_record_t rec = { 0 };

    rec.ts_ms = 1001;      /* 1.0 s after start_ms=1 */
    rec.kind = DL_REC_FRAME;
    rec.u.f.id = 0x7E8;
    rec.u.f.dlc = 2;
    rec.u.f.data[0] = 0xAA;
    rec.u.f.data[1] = 0xBB;

    uint8_t buf[DL_MF4_REC_SIZE];
    size_t n = dl_mf4_record(buf, rec.ts_ms - 1, &rec);

    TEST_ASSERT_EQUAL_size_t(DL_MF4_REC_SIZE, n);
    /* t = 1.0 as f64 LE */
    static const uint8_t ONE[] = { 0, 0, 0, 0, 0, 0, 0xF0, 0x3F };

    TEST_ASSERT_EQUAL_MEMORY(ONE, buf, 8);
    TEST_ASSERT_EQUAL_HEX8(0xE8, buf[8]);
    TEST_ASSERT_EQUAL_HEX8(0x07, buf[9]);
    TEST_ASSERT_EQUAL_HEX8(2, buf[13]);
    TEST_ASSERT_EQUAL_HEX8(0xAA, buf[14]);
    TEST_ASSERT_EQUAL_HEX8(0xBB, buf[15]);
    TEST_ASSERT_EQUAL_HEX8(0x00, buf[16]); /* zero-padded */
}

static void test_blf_builders_golden(void)
{
    uint8_t hdr[DL_BLF_HDR_SIZE];
    size_t n = dl_blf_file_header(hdr, 1783000000000LL,
                                  1783000005000LL, 4096, 2048, 42);

    TEST_ASSERT_EQUAL_size_t(DL_BLF_HDR_SIZE, n);
    TEST_ASSERT_EQUAL_MEMORY("LOGG", hdr, 4);
    TEST_ASSERT_EQUAL_HEX8(144, hdr[4]);
    TEST_ASSERT_EQUAL_HEX8(42, hdr[32]);   /* object count LSB */

    uint8_t ch[DL_BLF_CONT_HDR];

    dl_blf_container_header(ch, 96);
    TEST_ASSERT_EQUAL_MEMORY("LOBJ", ch, 4);
    TEST_ASSERT_EQUAL_HEX8(10, ch[12]);    /* LOG_CONTAINER */
    TEST_ASSERT_EQUAL_HEX8(32 + 96, ch[8]); /* object size LSB */
    TEST_ASSERT_EQUAL_HEX8(96, ch[24]);    /* uncompressed LSB */

    dl_record_t rec = { 0 };

    rec.ts_ms = 1500;
    rec.kind = DL_REC_FRAME;
    rec.u.f.id = 0x123;
    rec.u.f.dlc = 1;
    rec.u.f.data[0] = 0x55;

    uint8_t msg[DL_BLF_MSG_SIZE];

    n = dl_blf_message(msg, rec.ts_ms - 1000, &rec);
    TEST_ASSERT_EQUAL_size_t(DL_BLF_MSG_SIZE, n);
    TEST_ASSERT_EQUAL_MEMORY("LOBJ", msg, 4);
    TEST_ASSERT_EQUAL_HEX8(1, msg[12]);    /* CAN_MESSAGE */
    /* 0.5 s -> 500,000,000 ns = 0x1DCD6500 LE at +24 */
    TEST_ASSERT_EQUAL_HEX8(0x00, msg[24]);
    TEST_ASSERT_EQUAL_HEX8(0x65, msg[25]);
    TEST_ASSERT_EQUAL_HEX8(0xCD, msg[26]);
    TEST_ASSERT_EQUAL_HEX8(0x1D, msg[27]);
    TEST_ASSERT_EQUAL_HEX8(1, msg[32]);    /* channel */
    TEST_ASSERT_EQUAL_HEX8(1, msg[35]);    /* dlc */
    TEST_ASSERT_EQUAL_HEX8(0x23, msg[36]); /* id LSB */
    TEST_ASSERT_EQUAL_HEX8(0x55, msg[40]);
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_accepts_all_engine_extensions);
    RUN_TEST(test_parse_accepts_can_prefix);
    RUN_TEST(test_parse_rejects_foreign_names);
    RUN_TEST(test_make_parse_roundtrip);
    RUN_TEST(test_make_zero_pads_for_lexical_order);
    RUN_TEST(test_make_clamps_negative_epoch);
    RUN_TEST(test_export_start_rule);
    RUN_TEST(test_export_walk_budget);
    RUN_TEST(test_scan_tracks_oldest_newest_count);
    RUN_TEST(test_scan_ignores_foreign_entries);
    RUN_TEST(test_scan_isolates_streams_by_prefix);
    RUN_TEST(test_hex_parse);
    RUN_TEST(test_wdl_frame_encoding_golden);
    RUN_TEST(test_wdl_frame_encoding_flags);
    RUN_TEST(test_csv_frame_row);
    RUN_TEST(test_new_extensions_parse);
    RUN_TEST(test_candump_row_golden);
    RUN_TEST(test_asc_row_golden);
    RUN_TEST(test_jsonl_rows_golden);
    RUN_TEST(test_mf4_prelude_structure);
    RUN_TEST(test_mf4_record_golden);
    RUN_TEST(test_blf_builders_golden);
    /* recovery helpers (test_recover.c, ROBUSTNESS.md) */
    RUN_TEST(test_text_keep_whole_when_newline_terminated);
    RUN_TEST(test_text_keep_drops_the_torn_last_line);
    RUN_TEST(test_wdl_scan_counts_complete_records_only);
    RUN_TEST(test_wdl_scan_flags_garbage);
    RUN_TEST(test_corrupt_names_round_trip);
    RUN_TEST(test_record_sanity);
    RUN_TEST(test_ring_sanity_and_fill_recovery);
    RUN_TEST(test_crc32_known_vector);
    /* the clock (test_clock.c, README "Time") */
    RUN_TEST(test_record_layout_is_32_bytes_with_the_stamps_in_the_padding);
    RUN_TEST(test_rebase_adds_the_steps_that_came_after_the_record);
    RUN_TEST(test_rebase_leaves_absolute_records_alone);
    RUN_TEST(test_rebase_compares_the_stamps_modulo_the_wrap);
    RUN_TEST(test_relative_time_comes_from_the_monotonic_clock);
    RUN_TEST(test_rotation_rule);
    RUN_TEST(test_journal_lines_byte_exact);
    RUN_TEST(test_markers_byte_exact);
    RUN_TEST(test_wdl_scan_accepts_the_step_record);
    RUN_TEST(test_journal_trim_cuts_at_a_line_boundary);
    RUN_TEST(test_relative_rows_take_the_offset);
    RUN_TEST(test_mf4_prelude_keeps_the_start_at_the_patch_offsets);
    UNITY_END();
}
