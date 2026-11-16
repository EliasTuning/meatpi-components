/**
 * @file test_main.c
 * @brief Wire codec, filters, framing, bus helpers, migration and battery voltage.
 *        See ../README.md. Expected: 32 Tests 0 Failures 0 Ignored.
 */
#include <string.h>

#include "unity.h"

#include "j2534_proto.h"
#include "j2534_server_migrate.h"

void setUp(void) {}
void tearDown(void) {}

void run_voltage_tests(void);   /* test_voltage.c: its four cases */

static void test_hdr_roundtrip(void)
{
    uint8_t buf[J2534_HDR_SIZE];
    TEST_ASSERT_EQUAL(J2534_HDR_SIZE,
        j2534_hdr_encode(buf, J2534_MT_CONNECT, 0x1234, 7, 12));

    j2534_hdr_t h;
    TEST_ASSERT_TRUE(j2534_hdr_decode(buf, sizeof(buf), &h));
    TEST_ASSERT_EQUAL_HEX16(J2534_MAGIC, h.magic);
    TEST_ASSERT_EQUAL(J2534_WIRE_VERSION, h.version);
    TEST_ASSERT_EQUAL(J2534_MT_CONNECT, h.type);
    TEST_ASSERT_EQUAL_HEX16(0x1234, h.seq);
    TEST_ASSERT_EQUAL(7, h.channel);
    TEST_ASSERT_EQUAL(12, h.length);
}

static void test_hdr_rejects(void)
{
    uint8_t buf[J2534_HDR_SIZE];
    j2534_hdr_encode(buf, J2534_MT_HELLO, 1, 0, 0);
    j2534_hdr_t h;

    /* short buffer */
    TEST_ASSERT_FALSE(j2534_hdr_decode(buf, J2534_HDR_SIZE - 1, &h));
    /* bad magic */
    buf[0] ^= 0xFF;
    TEST_ASSERT_FALSE(j2534_hdr_decode(buf, sizeof(buf), &h));
    /* bad version */
    j2534_hdr_encode(buf, J2534_MT_HELLO, 1, 0, 0);
    buf[2] = 99;
    TEST_ASSERT_FALSE(j2534_hdr_decode(buf, sizeof(buf), &h));
}

static void test_msg_roundtrip(void)
{
    j2534_msg_t m = { 0 };
    m.protocol_id = J2534_PROT_ISO15765;
    m.tx_flags = J2534_TX_ISO15765_FRAME_PAD;
    m.timestamp = 0xDEADBEEF;
    m.data_size = 5;
    memcpy(m.data, (uint8_t[]){ 0x22, 0xF1, 0x90, 0xAA, 0xBB }, 5);

    uint8_t buf[64];
    size_t n = j2534_msg_encode(&m, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(24 + 5, n);

    j2534_msg_t out;
    TEST_ASSERT_TRUE(j2534_msg_decode(buf, n, &out));
    TEST_ASSERT_EQUAL(J2534_PROT_ISO15765, out.protocol_id);
    TEST_ASSERT_EQUAL_HEX32(J2534_TX_ISO15765_FRAME_PAD, out.tx_flags);
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEF, out.timestamp);
    TEST_ASSERT_EQUAL(5, out.data_size);
    TEST_ASSERT_EQUAL_HEX8(0x22, out.data[0]);
    TEST_ASSERT_EQUAL_HEX8(0xBB, out.data[4]);
}

static void test_msg_bounds(void)
{
    j2534_msg_t m = { 0 };
    m.data_size = 10;
    uint8_t small[20];
    /* buffer too small for fixed header + data */
    TEST_ASSERT_EQUAL(0, j2534_msg_encode(&m, small, sizeof(small)));

    /* decode with a truncated data region must fail */
    uint8_t buf[64];
    m.data_size = 5;
    size_t n = j2534_msg_encode(&m, buf, sizeof(buf));
    j2534_msg_t out;
    TEST_ASSERT_FALSE(j2534_msg_decode(buf, n - 1, &out));

    /* oversized data_size claim rejected */
    uint8_t fake[24];
    memset(fake, 0, sizeof(fake));
    fake[20] = 0xFF; fake[21] = 0xFF; /* huge data_size */
    TEST_ASSERT_FALSE(j2534_msg_decode(fake, sizeof(fake), &out));
}

static void test_filter_pass_all(void)
{
    const uint8_t msg[] = { 0x07, 0xE8, 0x62 };
    /* zero-length mask = PASS everything */
    TEST_ASSERT_TRUE(j2534_filter_match(NULL, 0, NULL, 0,
                                        msg, sizeof(msg)));
}

static void test_filter_exact_id(void)
{
    /* match a 4-byte CAN id 0x000007E8 */
    const uint8_t mask[]    = { 0xFF, 0xFF, 0xFF, 0xFF };
    const uint8_t pattern[] = { 0x00, 0x00, 0x07, 0xE8 };
    const uint8_t yes[]     = { 0x00, 0x00, 0x07, 0xE8, 0x62, 0xF1 };
    const uint8_t no[]      = { 0x00, 0x00, 0x07, 0xDF, 0x03 };

    TEST_ASSERT_TRUE(j2534_filter_match(mask, 4, pattern, 4,
                                        yes, sizeof(yes)));
    TEST_ASSERT_FALSE(j2534_filter_match(mask, 4, pattern, 4,
                                         no, sizeof(no)));
}

static void test_filter_partial_mask(void)
{
    /* care only about the high nibble of byte 0 */
    const uint8_t mask[]    = { 0xF0 };
    const uint8_t pattern[] = { 0x70 };
    const uint8_t yes[]     = { 0x7A, 0xBC };
    const uint8_t no[]      = { 0x8A };

    TEST_ASSERT_TRUE(j2534_filter_match(mask, 1, pattern, 1,
                                        yes, sizeof(yes)));
    TEST_ASSERT_FALSE(j2534_filter_match(mask, 1, pattern, 1,
                                         no, sizeof(no)));
}

static void test_status_names(void)
{
    TEST_ASSERT_EQUAL_STRING("NOERROR",
        j2534_status_name(J2534_STATUS_NOERROR));
    TEST_ASSERT_EQUAL_STRING("ERR_NOT_SUPPORTED",
        j2534_status_name(J2534_ERR_NOT_SUPPORTED));
    TEST_ASSERT_EQUAL_STRING("ERR_DEVICE_IN_USE",
        j2534_status_name(J2534_ERR_DEVICE_IN_USE));
    TEST_ASSERT_EQUAL_STRING("?", j2534_status_name(0xABCD));
}

/* ---- the transport-agnostic framer (fake read functions) --------------- */

/* a canned WRITE_MSGS frame: ch 1, seq 4, count 1, CAN 7DF [02 01 00] */
static uint8_t s_wire[64];
static size_t s_wire_len;
static size_t s_wire_off;
static size_t s_piece;        /* max bytes one read() hands back */
static int s_zero_reads;      /* return 0 this many times first */
static int s_down_after;      /* return -1 once this many bytes went out (-1 = never) */
static uint32_t s_reads;

static void wire_reset(size_t piece)
{
    j2534_msg_t m = { .protocol_id = J2534_PROT_CAN, .data_size = 7,
                      .data = { 0x00, 0x00, 0x07, 0xDF, 0x02, 0x01, 0x00 } };
    uint8_t payload[64];

    payload[0] = 1; payload[1] = 0; payload[2] = 0; payload[3] = 0;

    size_t n = j2534_msg_encode(&m, payload + 4, sizeof(payload) - 4);

    s_wire_len = j2534_hdr_encode(s_wire, J2534_MT_WRITE_MSGS, 4, 1,
                                  (uint32_t)(4 + n));
    memcpy(s_wire + s_wire_len, payload, 4 + n);
    s_wire_len += 4 + n;
    s_wire_off = 0;
    s_piece = piece;
    s_zero_reads = 0;
    s_down_after = -1;
    s_reads = 0;
}

static int fake_read(void *ctx, uint8_t *buf, size_t n, uint32_t timeout_ms)
{
    (void)ctx;
    (void)timeout_ms;
    s_reads++;

    if (s_zero_reads > 0)
    {
        s_zero_reads--;
        return 0;
    }

    if (s_down_after >= 0 && (int)s_wire_off >= s_down_after)
    {
        return -1;
    }

    size_t left = s_wire_len - s_wire_off;

    if (left == 0)
    {
        return 0; /* nothing more: a timeout */
    }

    if (n > left) n = left;
    if (n > s_piece) n = s_piece;
    memcpy(buf, s_wire + s_wire_off, n);
    s_wire_off += n;
    return (int)n;
}

static void test_frame_read_partial_chunks(void)
{
    static const size_t pieces[] = { 1, 7, 13, 490 };

    for (size_t i = 0; i < sizeof(pieces) / sizeof(pieces[0]); i++)
    {
        j2534_hdr_t h;
        uint8_t payload[J2534_RX_CAP];
        volatile bool run = true;

        wire_reset(pieces[i]);
        TEST_ASSERT_EQUAL(J2534_FR_OK,
            j2534_frame_read(fake_read, NULL, 200, 0, &run, &h, payload,
                             sizeof(payload)));
        TEST_ASSERT_EQUAL(J2534_MT_WRITE_MSGS, h.type);
        TEST_ASSERT_EQUAL(4, h.seq);
        TEST_ASSERT_EQUAL(1, h.channel);
        TEST_ASSERT_EQUAL(4 + 24 + 7, h.length);
        TEST_ASSERT_EQUAL_MEMORY(s_wire + J2534_HDR_SIZE, payload, h.length);
        TEST_ASSERT_EQUAL(s_wire_len, s_wire_off); /* consumed exactly */
    }
}

static void test_frame_read_timeout_then_data(void)
{
    j2534_hdr_t h;
    uint8_t payload[J2534_RX_CAP];

    wire_reset(8);
    s_zero_reads = 3;
    TEST_ASSERT_EQUAL(J2534_FR_OK,
        j2534_frame_read(fake_read, NULL, 200, 0, NULL, &h, payload,
                         sizeof(payload)));

    /* bounded idle: 3 empty polls with a budget of 2 -> TIMEOUT, nothing consumed */
    wire_reset(8);
    s_zero_reads = 3;
    TEST_ASSERT_EQUAL(J2534_FR_TIMEOUT,
        j2534_frame_read(fake_read, NULL, 200, 2, NULL, &h, payload,
                         sizeof(payload)));
    TEST_ASSERT_EQUAL(0, s_wire_off);
}

static void test_frame_read_link_down_mid_payload(void)
{
    j2534_hdr_t h;
    uint8_t payload[J2534_RX_CAP];

    wire_reset(6);
    s_down_after = J2534_HDR_SIZE + 5;
    TEST_ASSERT_EQUAL(J2534_FR_LINK_DOWN,
        j2534_frame_read(fake_read, NULL, 200, 0, NULL, &h, payload,
                         sizeof(payload)));
}

static void test_frame_read_bad_header(void)
{
    j2534_hdr_t h;
    uint8_t payload[J2534_RX_CAP];

    wire_reset(64);
    s_wire[0] = 0x00; /* magic */
    TEST_ASSERT_EQUAL(J2534_FR_BAD_HDR,
        j2534_frame_read(fake_read, NULL, 200, 0, NULL, &h, payload,
                         sizeof(payload)));

    wire_reset(64);
    s_wire[2] = 9; /* version */
    TEST_ASSERT_EQUAL(J2534_FR_BAD_HDR,
        j2534_frame_read(fake_read, NULL, 200, 0, NULL, &h, payload,
                         sizeof(payload)));

    wire_reset(64);
    TEST_ASSERT_EQUAL(J2534_FR_TOO_BIG,
        j2534_frame_read(fake_read, NULL, 200, 0, NULL, &h, payload, 10));
    TEST_ASSERT_EQUAL(J2534_HDR_SIZE, s_wire_off); /* payload untouched */
}

static void test_frame_read_discard_and_stop(void)
{
    j2534_hdr_t h;

    /* discard mode consumes exactly the frame */
    wire_reset(5);
    TEST_ASSERT_EQUAL(J2534_FR_OK,
        j2534_frame_read(fake_read, NULL, 200, 0, NULL, &h, NULL,
                         J2534_RX_CAP));
    TEST_ASSERT_EQUAL(s_wire_len, s_wire_off);
    TEST_ASSERT_EQUAL(4, h.seq);

    /* a cleared run flag stops before the first read */
    wire_reset(64);
    volatile bool run = false;
    TEST_ASSERT_EQUAL(J2534_FR_STOPPED,
        j2534_frame_read(fake_read, NULL, 200, 0, &run, &h, NULL,
                         J2534_RX_CAP));
    TEST_ASSERT_EQUAL(0, s_reads);
}

static void test_ack_encode_vectors(void)
{
    uint8_t buf[24];
    uint32_t ver = 1;

    /* HELLO ACK (seq 1, result = wire version 1): 20 bytes */
    static const uint8_t HELLO_ACK[] =
    {
        0x35, 0x4A, 0x01, 0x80, 0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    };
    TEST_ASSERT_EQUAL(20, j2534_ack_encode(buf, sizeof(buf), 1, 0,
                                           J2534_STATUS_NOERROR, &ver));
    TEST_ASSERT_EQUAL_MEMORY(HELLO_ACK, buf, 20);

    /* refusal of a second tester (seq 4): 16 bytes, status 0x1A */
    static const uint8_t IN_USE[] =
    {
        0x35, 0x4A, 0x01, 0x80, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
        0x1A, 0x00, 0x00, 0x00,
    };
    TEST_ASSERT_EQUAL(16, j2534_ack_encode(buf, sizeof(buf), 4, 0,
                                           J2534_ERR_DEVICE_IN_USE, NULL));
    TEST_ASSERT_EQUAL_MEMORY(IN_USE, buf, 16);

    TEST_ASSERT_EQUAL(0, j2534_ack_encode(buf, 15, 1, 0, 0, NULL));
}

static void test_tcp_gate_predicate(void)
{
    /* allow_lan off: loopback / AP / USB pass, a STA-side landing is refused */
    TEST_ASSERT_TRUE(j2534_tcp_gate_allowed(false, true, false, false));
    TEST_ASSERT_TRUE(j2534_tcp_gate_allowed(false, false, true, false));
    TEST_ASSERT_TRUE(j2534_tcp_gate_allowed(false, false, false, true));
    TEST_ASSERT_FALSE(j2534_tcp_gate_allowed(false, false, false, false));
    /* allow_lan on: everything */
    TEST_ASSERT_TRUE(j2534_tcp_gate_allowed(true, false, false, false));
}

static void test_connect_baud_check(void)
{
    /* the tester asks for what the bus runs at */
    TEST_ASSERT_EQUAL_UINT32(J2534_STATUS_NOERROR,
                             j2534_connect_baud_check(500000, 500));
    TEST_ASSERT_EQUAL_UINT32(J2534_STATUS_NOERROR,
                             j2534_connect_baud_check(250000, 250));
    /* the rates a setting rounds */
    TEST_ASSERT_EQUAL_UINT32(J2534_STATUS_NOERROR,
                             j2534_connect_baud_check(33333, 33));
    TEST_ASSERT_EQUAL_UINT32(J2534_STATUS_NOERROR,
                             j2534_connect_baud_check(83333, 83));
    TEST_ASSERT_EQUAL_UINT32(J2534_STATUS_NOERROR,
                             j2534_connect_baud_check(95238, 95));
    /* ... or for something else: refused, not silently served at 500 */
    TEST_ASSERT_EQUAL_UINT32(J2534_ERR_INVALID_BAUDRATE,
                             j2534_connect_baud_check(250000, 500));
    TEST_ASSERT_EQUAL_UINT32(J2534_ERR_INVALID_BAUDRATE,
                             j2534_connect_baud_check(500000, 250));
    TEST_ASSERT_EQUAL_UINT32(J2534_ERR_INVALID_BAUDRATE,
                             j2534_connect_baud_check(0, 500));
    TEST_ASSERT_EQUAL_UINT32(J2534_ERR_INVALID_BAUDRATE,
                             j2534_connect_baud_check(500, 500));
    /* the bus bitrate is not known: the channel opens */
    TEST_ASSERT_EQUAL_UINT32(J2534_STATUS_NOERROR,
                             j2534_connect_baud_check(500000, 0));
}

/* ---- ISO15765 message layout (SAE J2534-1, 2026-10-19) ----------------- */

/* udsoncan's DiagnosticSessionControl through the DLL: the id, then 10 03 */
static void test_iso_split_layout(void)
{
    j2534_msg_t m = { 0 };
    const uint8_t req[] = { 0x00, 0x00, 0x07, 0xE0, 0x10, 0x03 };
    uint32_t id = 0;
    const uint8_t *pl = NULL;
    size_t n = 99;

    m.protocol_id = J2534_PROT_ISO15765;
    m.tx_flags = J2534_TX_ISO15765_FRAME_PAD;
    m.data_size = sizeof(req);
    memcpy(m.data, req, sizeof(req));

    TEST_ASSERT_TRUE(j2534_iso_split(&m, &id, &pl, &n));
    TEST_ASSERT_EQUAL_HEX32(0x7E0, id);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_HEX8(0x10, pl[0]);
    TEST_ASSERT_EQUAL_HEX8(0x03, pl[1]);

    /* the id alone: nothing to send, still well formed */
    m.data_size = 4;
    TEST_ASSERT_TRUE(j2534_iso_split(&m, &id, &pl, &n));
    TEST_ASSERT_EQUAL(0, n);

    /* too short for an id */
    m.data_size = 3;
    TEST_ASSERT_FALSE(j2534_iso_split(&m, &id, &pl, &n));

    /* extended addressing is not supported */
    m.data_size = 7;
    m.tx_flags = J2534_TX_ISO15765_ADDR_TYPE;
    TEST_ASSERT_FALSE(j2534_iso_split(&m, &id, &pl, &n));

    /* a 29-bit id comes through whole */
    const uint8_t req29[] = { 0x18, 0xDA, 0x10, 0xF1, 0x3E, 0x00 };
    m.tx_flags = J2534_TX_CAN_29BIT_ID;
    m.data_size = sizeof(req29);
    memcpy(m.data, req29, sizeof(req29));
    TEST_ASSERT_TRUE(j2534_iso_split(&m, &id, &pl, &n));
    TEST_ASSERT_EQUAL_HEX32(0x18DA10F1, id);
    TEST_ASSERT_EQUAL(2, n);
}

static void test_iso_join_and_indication(void)
{
    static j2534_msg_t m;
    static uint8_t big[J2534_MAX_DATA];
    const uint8_t vin[] = { 0x62, 0xF1, 0x90, 'W', 'C', 'A', 'N' };
    const uint8_t id7e8[] = { 0x00, 0x00, 0x07, 0xE8 };

    TEST_ASSERT_TRUE(j2534_iso_join(&m, 0x7E8, false, vin, sizeof(vin), 1234));
    TEST_ASSERT_EQUAL(J2534_PROT_ISO15765, m.protocol_id);
    TEST_ASSERT_EQUAL_HEX32(0, m.rx_status);
    TEST_ASSERT_EQUAL(4 + sizeof(vin), m.data_size);
    TEST_ASSERT_EQUAL(m.data_size, m.extra_data_index);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(id7e8, m.data, 4);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(vin, m.data + 4, sizeof(vin));
    TEST_ASSERT_EQUAL_UINT32(1234, m.timestamp);

    /* 29-bit flags the status; an empty payload is still a message */
    TEST_ASSERT_TRUE(j2534_iso_join(&m, 0x18DAF110, true, NULL, 0, 0));
    TEST_ASSERT_EQUAL_HEX32(J2534_RX_CAN_29BIT_ID, m.rx_status);
    TEST_ASSERT_EQUAL(4, m.data_size);

    /* the cap: 4124 payload bytes fit behind the id, 4125 do not */
    TEST_ASSERT_TRUE(j2534_iso_join(&m, 0x7E8, false, big, J2534_MAX_DATA - 4, 0));
    TEST_ASSERT_EQUAL(J2534_MAX_DATA, m.data_size);
    TEST_ASSERT_FALSE(j2534_iso_join(&m, 0x7E8, false, big, J2534_MAX_DATA - 3, 0));

    /* TxDone and START_OF_MESSAGE carry the id only */
    j2534_iso_indication(&m, 0x7E0, false,
                         J2534_RX_TX_MSG_TYPE | J2534_RX_TX_INDICATION, 77);
    TEST_ASSERT_EQUAL_HEX32(0x09, m.rx_status);
    TEST_ASSERT_EQUAL(4, m.data_size);
    TEST_ASSERT_EQUAL(4, m.extra_data_index);
    TEST_ASSERT_EQUAL_HEX8(0xE0, m.data[3]);
    TEST_ASSERT_EQUAL_UINT32(77, m.timestamp);
    j2534_iso_indication(&m, 0x18DAF110, true, J2534_RX_START_OF_MESSAGE, 0);
    TEST_ASSERT_EQUAL_HEX32(J2534_RX_START_OF_MESSAGE | J2534_RX_CAN_29BIT_ID,
                            m.rx_status);
    TEST_ASSERT_EQUAL_HEX8(0x18, m.data[0]);
}

static void test_iso_single_frame_build(void)
{
    uint8_t fr[8];
    const uint8_t req[] = { 0x10, 0x03 };
    const uint8_t seven[7] = { 1, 2, 3, 4, 5, 6, 7 };
    const uint8_t eight[8] = { 0 };

    TEST_ASSERT_EQUAL(3, j2534_iso_single_frame(req, 2, false, fr));
    TEST_ASSERT_EQUAL_HEX8(0x02, fr[0]);
    TEST_ASSERT_EQUAL_HEX8(0x10, fr[1]);
    TEST_ASSERT_EQUAL_HEX8(0x03, fr[2]);

    memset(fr, 0, sizeof(fr));
    TEST_ASSERT_EQUAL(8, j2534_iso_single_frame(req, 2, true, fr));
    TEST_ASSERT_EQUAL_HEX8(0x02, fr[0]);
    TEST_ASSERT_EQUAL_HEX8(0xAA, fr[3]);
    TEST_ASSERT_EQUAL_HEX8(0xAA, fr[7]);

    TEST_ASSERT_EQUAL(8, j2534_iso_single_frame(seven, 7, false, fr));
    TEST_ASSERT_EQUAL_HEX8(0x07, fr[0]);
    TEST_ASSERT_EQUAL_HEX8(7, fr[7]);
    TEST_ASSERT_EQUAL(0, j2534_iso_single_frame(eight, 8, true, fr));
    TEST_ASSERT_EQUAL(0, j2534_iso_single_frame(eight, 0, true, fr));
}

static void test_iso_single_frame_parse(void)
{
    size_t n = 0;
    const uint8_t sf[8] = { 0x06, 0x50, 0x03, 0x00, 0x32, 0x01, 0xF4, 0x00 };
    const uint8_t short_sf[3] = { 0x02, 0x10, 0x03 };
    const uint8_t ff[8] = { 0x10, 0x14, 0x62, 0xF1, 0x90, 0x31, 0x57, 0x43 };
    const uint8_t zero[8] = { 0 };
    const uint8_t cf[8] = { 0x21, 0, 0, 0, 0, 0, 0, 0 };

    TEST_ASSERT_TRUE(j2534_iso_parse_single_frame(sf, 8, &n));
    TEST_ASSERT_EQUAL(6, n);
    /* unpadded frames are fine, a length past the DLC is not */
    TEST_ASSERT_TRUE(j2534_iso_parse_single_frame(short_sf, 3, &n));
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_FALSE(j2534_iso_parse_single_frame(short_sf, 2, &n));
    /* first and consecutive frames, and an empty single frame, are not */
    TEST_ASSERT_FALSE(j2534_iso_parse_single_frame(ff, 8, &n));
    TEST_ASSERT_FALSE(j2534_iso_parse_single_frame(zero, 8, &n));
    TEST_ASSERT_FALSE(j2534_iso_parse_single_frame(cf, 8, &n));
    TEST_ASSERT_TRUE(j2534_iso_is_first_frame(ff, 8));
    TEST_ASSERT_FALSE(j2534_iso_is_first_frame(sf, 8));
    TEST_ASSERT_FALSE(j2534_iso_is_first_frame(cf, 8));
    TEST_ASSERT_FALSE(j2534_iso_is_first_frame(ff, 1));
}

static void test_status_name_exceeded_limit(void)
{
    TEST_ASSERT_EQUAL_STRING("ERR_EXCEEDED_LIMIT",
                             j2534_status_name(J2534_ERR_EXCEEDED_LIMIT));
}

/* TP2.0 timing byte: 40 ms is the 0x68 MED9 puts on the wire today. */
static void test_tp20_time_byte(void)
{
    TEST_ASSERT_EQUAL_HEX8(0x68, j2534_tp20_encode_ms(40));
    TEST_ASSERT_EQUAL_HEX8(0x00, j2534_tp20_encode_ms(0));
    TEST_ASSERT_EQUAL_HEX8(0x0A, j2534_tp20_encode_ms(1));
    TEST_ASSERT_EQUAL_UINT32(40000, j2534_tp20_decode_us(0x68));
    TEST_ASSERT_EQUAL_UINT32(0, j2534_tp20_round_gap_us(0));
    TEST_ASSERT_EQUAL_UINT32(500, j2534_tp20_round_gap_us(100));
    TEST_ASSERT_EQUAL_UINT32(500, j2534_tp20_round_gap_us(500));
    TEST_ASSERT_EQUAL_UINT32(1000, j2534_tp20_round_gap_us(501));
    TEST_ASSERT_EQUAL_STRING("ERR_INVALID_IOCTL_VALUE",
                             j2534_status_name(J2534_ERR_INVALID_IOCTL_VALUE));
    TEST_ASSERT_EQUAL_STRING("ERR_NO_CONNECTION_ESTABLISHED",
                             j2534_status_name(J2534_ERR_NO_CONNECTION_ESTABLISHED));
}

/* ---- J1850 VPW (legacy, OBD chip) helpers ------------------------------ */

static void test_legacy_split(void)
{
    j2534_msg_t m = { 0 };
    const uint8_t sps[] = { 0x68, 0x6A, 0xF1, 0x22, 0xF1, 0x90 };
    const uint8_t *hdr = NULL;
    const uint8_t *pl = NULL;
    size_t n = 99;

    m.protocol_id = J2534_PROT_J1850VPW;
    m.data_size = sizeof(sps);
    memcpy(m.data, sps, sizeof(sps));

    TEST_ASSERT_TRUE(j2534_legacy_split(&m, &hdr, &pl, &n));
    TEST_ASSERT_EQUAL_HEX8(0x68, hdr[0]);
    TEST_ASSERT_EQUAL_HEX8(0xF1, hdr[2]);
    TEST_ASSERT_EQUAL(3, n);
    TEST_ASSERT_EQUAL_HEX8(0x22, pl[0]);
    TEST_ASSERT_EQUAL_HEX8(0x90, pl[2]);

    /* header + 1 payload byte is the minimum; header-only is rejected */
    m.data_size = 4;
    TEST_ASSERT_TRUE(j2534_legacy_split(&m, &hdr, &pl, &n));
    TEST_ASSERT_EQUAL(1, n);
    m.data_size = 3;
    TEST_ASSERT_FALSE(j2534_legacy_split(&m, &hdr, &pl, &n));
}

static void test_legacy_header_and_checksum(void)
{
    const uint8_t hdr[] = { 0x68, 0x6A, 0xF1 };
    const uint8_t pay[] = { 0x36, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
    char out[16];

    TEST_ASSERT_EQUAL(10, j2534_legacy_header_cmd(hdr, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("ATSH686AF1", out);
    /* too small a buffer is refused, not truncated */
    TEST_ASSERT_EQUAL(0, j2534_legacy_header_cmd(hdr, out, 10));

    /* additive 16-bit sum: 0x36 + 0+1+2+3+4+5+6 = 0x4B */
    TEST_ASSERT_EQUAL_HEX16(0x004B, j2534_legacy_checksum(pay, sizeof(pay)));
    TEST_ASSERT_EQUAL_HEX16(0, j2534_legacy_checksum(NULL, 0));
}

static void test_legacy_build_request(void)
{
    const uint8_t sf[] = { 0x22, 0xF1, 0x90 };
    const uint8_t seven[] = { 0x3E, 0, 0, 0, 0, 0, 0 };
    const uint8_t mf[] = { 0x36, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
    char out[64];

    /* short payload: a bare hex line */
    TEST_ASSERT_EQUAL(6, j2534_legacy_build_request(sf, 3, J2534_J1850_SF_MAX,
                                                    out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("22F190", out);
    /* exactly seven bytes is still a single frame */
    TEST_ASSERT_EQUAL(14, j2534_legacy_build_request(seven, 7,
                              J2534_J1850_SF_MAX, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("3E000000000000", out);

    /* eight bytes: VTFullyRequestCk<LLLL><hex><CCCC> for the chip to segment */
    TEST_ASSERT_TRUE(j2534_legacy_build_request(mf, 8, J2534_J1850_SF_MAX,
                                                out, sizeof(out)) > 0);
    TEST_ASSERT_EQUAL_STRING("VTFullyRequestCk00083600010203040506004B", out);

    /* empty payload, and an output buffer too small, are refused */
    TEST_ASSERT_EQUAL(0, j2534_legacy_build_request(sf, 0, J2534_J1850_SF_MAX,
                                                    out, sizeof(out)));
    TEST_ASSERT_EQUAL(0, j2534_legacy_build_request(mf, 8, J2534_J1850_SF_MAX,
                                                    out, 10));
}

static void test_legacy_decode_and_status(void)
{
    uint8_t out[16];

    /* a reply with headers (ATH1), no spaces */
    TEST_ASSERT_EQUAL(6, j2534_legacy_decode_line("C4F5D062F190", 12,
                                                  out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8(0xC4, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x90, out[5]);
    /* spaces are ignored */
    TEST_ASSERT_EQUAL(3, j2534_legacy_decode_line("48 6B 10", 8,
                                                  out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8(0x6B, out[1]);

    /* status / error words are not data */
    TEST_ASSERT_TRUE(j2534_legacy_line_is_status("NO DATA", 7));
    TEST_ASSERT_TRUE(j2534_legacy_line_is_status("SEARCHING...", 12));
    TEST_ASSERT_TRUE(j2534_legacy_line_is_status("OK", 2));
    TEST_ASSERT_TRUE(j2534_legacy_line_is_status("?", 1));
    TEST_ASSERT_TRUE(j2534_legacy_line_is_status("BUS INIT", 8));
    TEST_ASSERT_FALSE(j2534_legacy_line_is_status("C4F5D07F3678", 12));
    TEST_ASSERT_FALSE(j2534_legacy_line_is_status("48 6B 10 41", 11));
}

static void test_legacy_pending(void)
{
    const uint8_t pending[] = { 0xC4, 0xF5, 0xD0, 0x7F, 0x36, 0x78 };
    const uint8_t final[]   = { 0xC4, 0xF5, 0xD0, 0x76, 0x00, 0x22 };
    const uint8_t shortmsg[] = { 0xC4, 0xF5, 0xD0 };

    TEST_ASSERT_TRUE(j2534_legacy_is_pending(pending, sizeof(pending)));
    TEST_ASSERT_FALSE(j2534_legacy_is_pending(final, sizeof(final)));
    TEST_ASSERT_FALSE(j2534_legacy_is_pending(shortmsg, sizeof(shortmsg)));
}

/* settings v1 -> v2 (2026-10-27): allow_reflash goes, nothing else moves */
static void test_migrate_v1_drops_allow_reflash(void)
{
    cJSON *doc = cJSON_Parse("{\"enabled\":true,\"port\":6810,"
                             "\"allow_reflash\":true,\"allow_lan\":true,"
                             "\"exclusive\":false,\"cli\":false}");

    TEST_ASSERT_NOT_NULL(doc);
    TEST_ASSERT_EQUAL(ESP_OK, j2534_settings_migrate(1, doc));
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(doc, "allow_reflash"));
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(doc, "enabled")));
    TEST_ASSERT_EQUAL(6810, cJSON_GetObjectItemCaseSensitive(doc, "port")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(doc, "allow_lan")));
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(doc, "exclusive")));
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(doc, "cli")));
    TEST_ASSERT_EQUAL(5, cJSON_GetArraySize(doc));
    cJSON_Delete(doc);
}

/* a v1 object that never had the key, a current object, and bad input */
static void test_migrate_v1_without_key_and_v2_untouched(void)
{
    cJSON *v1 = cJSON_Parse("{\"enabled\":false,\"port\":6809,\"cli\":true}");
    cJSON *v2 = cJSON_Parse("{\"enabled\":false,\"port\":6809,"
                            "\"allow_lan\":false,\"exclusive\":true,"
                            "\"cli\":true}");
    cJSON *arr = cJSON_Parse("[1,2]");

    TEST_ASSERT_NOT_NULL(v1);
    TEST_ASSERT_NOT_NULL(v2);
    TEST_ASSERT_EQUAL(ESP_OK, j2534_settings_migrate(1, v1));
    TEST_ASSERT_EQUAL(3, cJSON_GetArraySize(v1));
    TEST_ASSERT_EQUAL(ESP_OK, j2534_settings_migrate(2, v2));
    TEST_ASSERT_EQUAL(5, cJSON_GetArraySize(v2));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, j2534_settings_migrate(1, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, j2534_settings_migrate(1, arr));
    cJSON_Delete(v1);
    cJSON_Delete(v2);
    cJSON_Delete(arr);
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_hdr_roundtrip);
    RUN_TEST(test_hdr_rejects);
    RUN_TEST(test_msg_roundtrip);
    RUN_TEST(test_msg_bounds);
    RUN_TEST(test_filter_pass_all);
    RUN_TEST(test_filter_exact_id);
    RUN_TEST(test_filter_partial_mask);
    RUN_TEST(test_status_names);
    RUN_TEST(test_frame_read_partial_chunks);
    RUN_TEST(test_frame_read_timeout_then_data);
    RUN_TEST(test_frame_read_link_down_mid_payload);
    RUN_TEST(test_frame_read_bad_header);
    RUN_TEST(test_frame_read_discard_and_stop);
    RUN_TEST(test_ack_encode_vectors);
    RUN_TEST(test_tcp_gate_predicate);
    RUN_TEST(test_connect_baud_check);
    RUN_TEST(test_iso_split_layout);
    RUN_TEST(test_iso_join_and_indication);
    RUN_TEST(test_iso_single_frame_build);
    RUN_TEST(test_iso_single_frame_parse);
    RUN_TEST(test_status_name_exceeded_limit);
    RUN_TEST(test_tp20_time_byte);
    RUN_TEST(test_legacy_split);
    RUN_TEST(test_legacy_header_and_checksum);
    RUN_TEST(test_legacy_build_request);
    RUN_TEST(test_legacy_decode_and_status);
    RUN_TEST(test_legacy_pending);
    RUN_TEST(test_migrate_v1_drops_allow_reflash);
    RUN_TEST(test_migrate_v1_without_key_and_v2_untouched);
    run_voltage_tests();
    UNITY_END();
}
