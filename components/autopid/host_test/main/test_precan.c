/**
 * @file test_precan.c
 * @brief The pre-CAN protocols (SAE J1850, ISO 9141-2, ISO 14230-4): the
 *        protocol table and the reply layout (autopid_proto.c,
 *        autopid_resp_precan.c).
 *
 * The replies are the chip's own prints: GitHub issue #1045 (a 2000 Toyota
 * 4Runner on ISO 9141-2: `48 6B 10 41 0C 0A D3 ED`; a 2005 Porsche Cayman S,
 * its engine ECU at address 0x11: `48 6B 11 41 0C 0A 96 B1`, `48 6B 11 41 05
 * 7E 88`, and `41 0C 0A 9A` with headers off) and the bench (the MIC3624
 * V2.3.24 against ECU sim v2, 2026-11-11 to 2026-11-13). Read the CAN way,
 * the engine speed of the bench's car came out as 15428 where it was 800.
 */
#include <string.h>

#include "unity.h"

#include "expression_parser.h"

#include "autopid_private.h"

#define SIM_VIN "1WCAN0FW0P0000001"
#define RPM     "[B3:B4]/4"

static double eval(const char *expr, const uint8_t *bytes, size_t n)
{
    double v = 0;

    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK,
                              expression_parser_eval(expr, bytes, n, 0.0, &v),
                              expr);
    return v;
}

static size_t frames(const char *resp, uint8_t *out, size_t cap)
{
    size_t n = 0;

    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, ap_resp_frames(resp, out, cap, &n),
                              resp);
    return n;
}

static size_t payload(const char *resp, uint8_t *out, size_t cap)
{
    size_t n = 0;

    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, ap_resp_to_payload(resp, out, cap, &n),
                              resp);
    return n;
}

void test_precan_protocol_table(void)
{
    /* every ELM number, and nothing else */
    for (const char *c = "123456789ABC"; *c != '\0'; c++)
    {
        TEST_ASSERT_NOT_NULL(ap_proto(*c));
        TEST_ASSERT_EQUAL(*c, ap_proto(*c)->id);
    }

    TEST_ASSERT_EQUAL(12, (int)ap_proto_count());
    TEST_ASSERT_NULL(ap_proto('0'));    /* the search is no protocol */
    TEST_ASSERT_NULL(ap_proto('D'));
    TEST_ASSERT_NULL(ap_proto('\0'));
    TEST_ASSERT_EQUAL('A', ap_proto('a')->id);
    TEST_ASSERT_NULL(ap_proto_at(12));
    TEST_ASSERT_EQUAL('1', ap_proto_at(0)->id);

    /* in words (2026-11-15, the push's can_protocol): name and detail; ""
       for an id that is no protocol; cut, never overrun */
    char text[48];

    TEST_ASSERT_EQUAL(35, (int)ap_proto_text('6', text, sizeof(text)));
    TEST_ASSERT_EQUAL_STRING("ISO 15765-4 CAN, 11-bit, 500 kbit/s", text);
    ap_proto_text('a', text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("SAE J1939, 29-bit, 250 kbit/s", text);
    TEST_ASSERT_EQUAL(0, (int)ap_proto_text('0', text, sizeof(text)));
    TEST_ASSERT_EQUAL_STRING("", text);
    TEST_ASSERT_EQUAL(0, (int)ap_proto_text('\0', text, sizeof(text)));
    TEST_ASSERT_EQUAL(7, (int)ap_proto_text('6', text, 8));
    TEST_ASSERT_EQUAL_STRING("ISO 157", text);

    for (size_t i = 0; i < ap_proto_count(); i++)
    {
        /* every row fits a 48 byte field whole (the push's can_protocol) */
        TEST_ASSERT_TRUE(ap_proto_text(ap_proto_at(i)->id, text, sizeof(text))
                         < sizeof(text) - 1);
    }

    /* the kinds */
    TEST_ASSERT_TRUE(ap_proto_precan('1'));
    TEST_ASSERT_TRUE(ap_proto_precan('5'));
    TEST_ASSERT_FALSE(ap_proto_precan('6'));
    TEST_ASSERT_FALSE(ap_proto_precan('A'));
    TEST_ASSERT_FALSE(ap_proto_precan('0'));
    TEST_ASSERT_FALSE(ap_proto_kline('2'));
    TEST_ASSERT_TRUE(ap_proto_kline('3'));
    TEST_ASSERT_TRUE(ap_proto_kline('4'));
    TEST_ASSERT_TRUE(ap_proto_kline('5'));
    TEST_ASSERT_FALSE(ap_proto_kline('6'));

    /* what the setting may name: the legislated nine */
    TEST_ASSERT_TRUE(ap_proto_pinnable('1'));
    TEST_ASSERT_TRUE(ap_proto_pinnable('9'));
    TEST_ASSERT_FALSE(ap_proto_pinnable('0'));
    TEST_ASSERT_FALSE(ap_proto_pinnable('A'));

    /* a K-line bus is started and its session ends by itself; the others
       are just there */
    TEST_ASSERT_TRUE(ap_proto('3')->start_ms >= 3000);
    TEST_ASSERT_TRUE(ap_proto('5')->start_ms >= 1500);
    TEST_ASSERT_TRUE(ap_proto('3')->session_ms < 5000);
    TEST_ASSERT_EQUAL(0, ap_proto('2')->start_ms);
    TEST_ASSERT_EQUAL(0, ap_proto('6')->session_ms);

    /* the facts the other maps held: the bit rate and the id width */
    TEST_ASSERT_EQUAL_UINT32(500000, ap_proto('6')->bps);
    TEST_ASSERT_EQUAL_UINT32(250000, ap_proto('9')->bps);
    TEST_ASSERT_EQUAL(11, ap_proto('8')->id_bits);
    TEST_ASSERT_EQUAL(29, ap_proto('7')->id_bits);
    TEST_ASSERT_EQUAL(0, ap_proto('3')->id_bits);
    TEST_ASSERT_TRUE(ap_veh_proto_is_29bit('B'));
    TEST_ASSERT_FALSE(ap_veh_proto_is_29bit('3'));

    /* the walk behind the CAN protocols, the cheapest first: SAE J1850 (no
       start of the bus, 0.7 s to say nobody is there), the K-line's fast
       init (1.0 s), then its two 5 baud starts (2.8 s each) */
    TEST_ASSERT_EQUAL_STRING("21534", ap_proto_walk_precan());

    for (const char *k = ap_proto_walk_precan(); *k != '\0'; k++)
    {
        TEST_ASSERT_TRUE(ap_proto_precan(*k));
    }

    /* the poller's one short request that keeps a K-line session: when the
       bus has not answered for 3.5 s, nothing else is due soon, and it did
       not just send one */
    TEST_ASSERT_FALSE(ap_proto_kline_keep(UINT32_MAX, 10000, UINT32_MAX));
    TEST_ASSERT_FALSE(ap_proto_kline_keep(1000, 10000, UINT32_MAX));
    TEST_ASSERT_TRUE(ap_proto_kline_keep(3600, 10000, UINT32_MAX));
    TEST_ASSERT_TRUE(ap_proto_kline_keep(3600, 10000, 3700));
    TEST_ASSERT_FALSE(ap_proto_kline_keep(3600, 300, UINT32_MAX));
    /* sent half a second ago and not answered (the car is off): not again,
       and not for a session that is past its time: the next row starts the
       bus */
    TEST_ASSERT_FALSE(ap_proto_kline_keep(4000, 10000, 500));
    TEST_ASSERT_FALSE(ap_proto_kline_keep(4600, 10000, UINT32_MAX));

    /* a name and a detail for the page, for every one */
    for (size_t i = 0; i < ap_proto_count(); i++)
    {
        TEST_ASSERT_TRUE(ap_proto_at(i)->name[0] != '\0');
        TEST_ASSERT_TRUE(ap_proto_at(i)->detail[0] != '\0');
        TEST_ASSERT_TRUE(ap_proto_at(i)->pins[0] != '\0');
    }

    /* the two checksums, on the chip's own prints */
    const uint8_t k9141[] = { 0x48, 0x6B, 0x10, 0x41, 0x0C, 0x0A, 0xD3 };
    const uint8_t kwp[] = { 0x84, 0xF1, 0x10, 0x41, 0x0C, 0x0C, 0x80 };
    const uint8_t vpw[] = { 0x48, 0x6B, 0x10, 0x41, 0x0C, 0x0C, 0x80 };
    const uint8_t pwm[] = { 0x41, 0x6B, 0x10, 0x41, 0x0C, 0x0C, 0x80 };

    TEST_ASSERT_EQUAL_HEX8(0xED, ap_proto_sum8(k9141, sizeof(k9141)));
    TEST_ASSERT_EQUAL_HEX8(0x5E, ap_proto_sum8(kwp, sizeof(kwp)));
    TEST_ASSERT_EQUAL_HEX8(0x02, ap_proto_crc_j1850(vpw, sizeof(vpw)));
    TEST_ASSERT_EQUAL_HEX8(0x8D, ap_proto_crc_j1850(pwm, sizeof(pwm)));
}

/* The chip keeps one request header across a change of protocol (bench
   2026-11-12: `7DF` standing, ISO 9141-2 started and read NO DATA, the KWP
   fast init failed with ERROR). So every pre-CAN prelude names its own, and
   an init of the tables that takes the chip to such a protocol gets it. */
void test_precan_request_header(void)
{
    TEST_ASSERT_EQUAL_STRING("ATSH616AF1", ap_veh_precan_header('1'));
    TEST_ASSERT_EQUAL_STRING("ATSH686AF1", ap_veh_precan_header('2'));
    TEST_ASSERT_EQUAL_STRING("ATSH686AF1", ap_veh_precan_header('3'));
    TEST_ASSERT_EQUAL_STRING("ATSHC133F1", ap_veh_precan_header('4'));
    TEST_ASSERT_EQUAL_STRING("ATSHC133F1", ap_veh_precan_header('5'));
    TEST_ASSERT_NULL(ap_veh_precan_header('6'));
    TEST_ASSERT_NULL(ap_veh_precan_header('A'));
    TEST_ASSERT_NULL(ap_veh_precan_header('0'));
    TEST_ASSERT_NULL(ap_veh_precan_header('\0'));

    /* the functional header of the CAN protocols stays what tells the
       identity check that a UDS fallback is possible: none here */
    TEST_ASSERT_NULL(ap_veh_func_header('3'));

    /* every pre-CAN prelude ends in its protocol's header, behind the
       protocol word */
    for (const char *c = "12345"; *c != '\0'; c++)
    {
        const char *prelude = ap_veh_prelude_for(*c);
        const char *hdr = ap_veh_precan_header(*c);
        size_t np = strlen(prelude), nh = strlen(hdr);

        TEST_ASSERT_TRUE(np > nh + 1);
        TEST_ASSERT_EQUAL_STRING(hdr, prelude + np - nh);
        TEST_ASSERT_EQUAL(';', prelude[np - nh - 1]);
        TEST_ASSERT_NOT_NULL(strstr(prelude, "ATTP"));
        TEST_ASSERT_TRUE(strstr(prelude, "ATTP") < prelude + np - nh);
    }

    /* issue #1045's Test call: the car's own protocol is CAN, the row's
       init says ATSP3 and no header */
    TEST_ASSERT_EQUAL_STRING("ATSH686AF1", ap_veh_pin_header('6', '3', false));
    TEST_ASSERT_EQUAL_STRING("ATSHC133F1", ap_veh_pin_header('6', '5', false));
    TEST_ASSERT_EQUAL_STRING("ATSH616AF1", ap_veh_pin_header('0', '1', false));
    /* from one pre-CAN protocol to another */
    TEST_ASSERT_EQUAL_STRING("ATSHC133F1", ap_veh_pin_header('3', '4', false));
    /* the init's own header stands */
    TEST_ASSERT_NULL(ap_veh_pin_header('6', '3', true));
    /* the chip was there already (the second call, the car's own protocol):
       what the rows before it set is kept, as within a type */
    TEST_ASSERT_NULL(ap_veh_pin_header('3', '3', false));
    /* the init names no protocol, or a CAN one, or the search */
    TEST_ASSERT_NULL(ap_veh_pin_header('6', '\0', false));
    TEST_ASSERT_NULL(ap_veh_pin_header('3', '\0', false));
    TEST_ASSERT_NULL(ap_veh_pin_header('3', '6', false));
    TEST_ASSERT_NULL(ap_veh_pin_header('6', '8', false));
    TEST_ASSERT_NULL(ap_veh_pin_header('3', '0', false));
}

/* The reproduction, then the fix: the report's replies and the bench's. */
void test_precan_reply_decodes_as_on_can(void)
{
    uint8_t f[AP_FRAMES_MAX], p[AP_PAYLOAD_MAX];
    size_t n;

    /* read the CAN way (no hint): the header and the checksum are taken for
       data and the engine speed is nonsense (the baseline of 2026-11-11) */
    ap_resp_set_proto_hint('6');
    n = frames("48 6B 10 41 0C 0C 80 9C \r", f, sizeof(f));
    TEST_ASSERT_EQUAL_HEX8(0x10, f[0]);
    TEST_ASSERT_TRUE(eval(RPM, f, n) != 800.0);

    /* ISO 9141-2, the bench's car at 800 rpm */
    const uint8_t want[] = { 0x04, 0x41, 0x0C, 0x0C, 0x80 };

    ap_resp_set_proto_hint('3');
    n = frames("48 6B 10 41 0C 0C 80 9C \r", f, sizeof(f));
    TEST_ASSERT_EQUAL_size_t(sizeof(want), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f, sizeof(want));
    TEST_ASSERT_EQUAL_DOUBLE(800.0, eval(RPM, f, n));
    n = payload("48 6B 10 41 0C 0C 80 9C \r", p, sizeof(p));
    TEST_ASSERT_EQUAL_size_t(4, n);
    TEST_ASSERT_EQUAL_HEX8(0x41, p[0]);
    TEST_ASSERT_TRUE(ap_payload_matches_cmd("010C1", p, n));

    /* behind the line of the bus start, as the first request reads */
    n = frames("BUS INIT: ...OK\r48 6B 10 41 0C 0C 80 9C \r\r", f, sizeof(f));
    TEST_ASSERT_EQUAL_DOUBLE(800.0, eval(RPM, f, n));

    /* the cars of issue #1045. The 4Runner: 692.75 rpm */
    n = frames("48 6B 10 41 0C 0A D3 ED \r", f, sizeof(f));
    TEST_ASSERT_EQUAL_DOUBLE(692.75, eval(RPM, f, n));

    /* the Cayman S, its engine ECU at address 0x11: 677.5 rpm and 86
       degrees; then what its chip printed with headers off, where the
       checksum is gone too */
    n = frames("48 6B 11 41 0C 0A 96 B1 \r", f, sizeof(f));
    TEST_ASSERT_EQUAL_DOUBLE(677.5, eval(RPM, f, n));
    n = frames("48 6B 11 41 05 7E 88 \r", f, sizeof(f));
    TEST_ASSERT_EQUAL_DOUBLE(86.0, eval("B3-40", f, n));
    n = frames("41 0C 0A 9A \r", f, sizeof(f));
    TEST_ASSERT_EQUAL_DOUBLE(678.5, eval(RPM, f, n));
    n = frames("41 05 7E \r", f, sizeof(f));
    TEST_ASSERT_EQUAL_DOUBLE(86.0, eval("B3-40", f, n));

    /* headers off (the chip after a reset): the same bytes for the row */
    n = frames("41 0C 0C 80 \r", f, sizeof(f));
    TEST_ASSERT_EQUAL_size_t(sizeof(want), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f, sizeof(want));

    /* ISO 14230: the length in the format byte, and in a byte of its own */
    ap_resp_set_proto_hint('4');
    n = frames("84 F1 10 41 0C 0C 80 5E \r", f, sizeof(f));
    TEST_ASSERT_EQUAL_DOUBLE(800.0, eval(RPM, f, n));
    ap_resp_set_proto_hint('5');
    n = frames("BUS INIT: OK\r80 F1 10 04 41 0C 0C 80 5E \r", f, sizeof(f));
    TEST_ASSERT_EQUAL_size_t(sizeof(want), n);
    TEST_ASSERT_EQUAL_DOUBLE(800.0, eval(RPM, f, n));
    n = frames("41 0C 0C 80 \r", f, sizeof(f));
    TEST_ASSERT_EQUAL_DOUBLE(800.0, eval(RPM, f, n));

    /* J1850: VPW and PWM, the same header with the SAE CRC */
    ap_resp_set_proto_hint('2');
    n = frames("48 6B 10 41 0C 0C 80 02 \r", f, sizeof(f));
    TEST_ASSERT_EQUAL_DOUBLE(800.0, eval(RPM, f, n));
    ap_resp_set_proto_hint('1');
    n = frames("41 6B 10 41 0C 0C 80 8D \r", f, sizeof(f));
    TEST_ASSERT_EQUAL_DOUBLE(800.0, eval(RPM, f, n));

    /* a negative answer is an answer too */
    ap_resp_set_proto_hint('3');
    n = payload("48 6B 10 7F 01 12 55 \r", p, sizeof(p));
    TEST_ASSERT_EQUAL_size_t(3, n);
    TEST_ASSERT_EQUAL_HEX8(0x7F, p[0]);

    ap_resp_set_proto_hint('\0');
}

void test_precan_ecus_by_address(void)
{
    static ap_resp_ecu_t e[AP_RESP_ECUS_MAX];
    ap_veh_ecu_t v[AP_VEH_ECUS_MAX];
    uint8_t p[AP_PAYLOAD_MAX];
    size_t n;

    /* the transmission answers first, the engine second: the lowest
       address is the one a row reads, as the lowest id is on CAN */
    const char *two = "48 6B 18 41 00 80 00 00 00 8C \r"
                      "48 6B 10 41 00 18 3F 80 03 DE \r";

    ap_resp_set_proto_hint('3');
    TEST_ASSERT_EQUAL(2, ap_resp_to_payloads(two, e, AP_RESP_ECUS_MAX));
    TEST_ASSERT_EQUAL_HEX32(0x10, e[0].header);
    TEST_ASSERT_EQUAL_HEX32(0x18, e[1].header);
    TEST_ASSERT_EQUAL_size_t(6, e[0].len);
    TEST_ASSERT_EQUAL_HEX8(0x18, e[0].payload[2]);
    TEST_ASSERT_EQUAL_HEX8(0x80, e[1].payload[2]);
    n = payload(two, p, sizeof(p));
    TEST_ASSERT_EQUAL_size_t(6, n);
    TEST_ASSERT_EQUAL_HEX8(0x3F, p[3]);

    /* the detection's responder set: an address each, a bitmap each */
    TEST_ASSERT_EQUAL(2, ap_dialect_bitmaps(AP_DIALECT_OBD2, two, 0x00, v,
                                            AP_VEH_ECUS_MAX));
    TEST_ASSERT_EQUAL_HEX32(0x18, v[0].id);
    TEST_ASSERT_EQUAL_HEX32(0x80000000, v[0].bitmap);
    TEST_ASSERT_EQUAL_HEX32(0x10, v[1].id);
    TEST_ASSERT_EQUAL_HEX32(0x183F8003, v[1].bitmap);

    /* ISO 14230: the ECU is the third byte there too */
    ap_resp_set_proto_hint('5');
    TEST_ASSERT_EQUAL(1, ap_dialect_bitmaps(
        AP_DIALECT_OBD2, "BUS INIT: OK\r86 F1 10 41 00 18 3F 80 03 A2 \r",
        0x00, v, AP_VEH_ECUS_MAX));
    TEST_ASSERT_EQUAL_HEX32(0x10, v[0].id);
    TEST_ASSERT_EQUAL_HEX32(0x183F8003, v[0].bitmap);

    /* headers off: nobody is told apart, the first answer stands (and is
       not glued to the second, as plain lines were) */
    ap_resp_set_proto_hint('3');
    TEST_ASSERT_EQUAL(1, ap_resp_to_payloads("41 00 18 3F 80 03 \r"
                                             "41 00 80 00 00 00 \r", e,
                                             AP_RESP_ECUS_MAX));
    TEST_ASSERT_EQUAL_HEX32(UINT32_MAX, e[0].header);
    TEST_ASSERT_EQUAL_size_t(6, e[0].len);

    /* a message with a wrong checksum among good ones is dropped */
    TEST_ASSERT_EQUAL(1, ap_resp_to_payloads("48 6B 10 41 00 18 3F 80 03 DE \r"
                                             "48 6B 18 41 00 80 00 00 00 FF \r",
                                             e, AP_RESP_ECUS_MAX));
    TEST_ASSERT_EQUAL_HEX32(0x10, e[0].header);

    ap_resp_set_proto_hint('\0');
}

void test_precan_vin_in_five_messages(void)
{
    char vin[AP_VIN_LEN];
    uint8_t p[AP_PAYLOAD_MAX];
    size_t n;

    /* ISO 9141-2 (and J1850): four bytes a message, the first filled up in
       front. The first three lines are the bench's print, the last two
       carry the rest of the simulator's VIN */
    const char *five = "48 6B 10 49 02 01 00 00 00 31 40 \r"
                       "48 6B 10 49 02 02 57 43 41 4E 39 \r"
                       "48 6B 10 49 02 03 30 46 57 30 0E \r"
                       "48 6B 10 49 02 04 50 30 30 30 F2 \r"
                       "48 6B 10 49 02 05 30 30 30 31 D4 \r";

    ap_resp_set_proto_hint('3');
    n = payload(five, p, sizeof(p));
    TEST_ASSERT_EQUAL_size_t(20, n);    /* 49 02 01 and seventeen characters */
    TEST_ASSERT_EQUAL_HEX8(0x49, p[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01, p[2]);
    TEST_ASSERT_EQUAL_HEX8('1', p[3]);
    TEST_ASSERT_EQUAL_HEX8('1', p[19]);
    TEST_ASSERT_TRUE(ap_veh_parse_vin_0902(five, vin));
    TEST_ASSERT_EQUAL_STRING(SIM_VIN, vin);

    /* headers off */
    TEST_ASSERT_TRUE(ap_veh_parse_vin_0902("49 02 01 00 00 00 31 \r"
                                           "49 02 02 57 43 41 4E \r"
                                           "49 02 03 30 46 57 30 \r"
                                           "49 02 04 50 30 30 30 \r"
                                           "49 02 05 30 30 30 31 \r", vin));
    TEST_ASSERT_EQUAL_STRING(SIM_VIN, vin);

    /* ISO 14230 in one message, the way a CAN ECU sends it (ECU sim v2
       until 2026-11-13) */
    ap_resp_set_proto_hint('4');
    TEST_ASSERT_TRUE(ap_veh_parse_vin_0902(
        "94 F1 10 49 02 01 31 57 43 41 4E 30 46 57 30 50 30 30 30 30 30 30 31 "
        "D9 \r", vin));
    TEST_ASSERT_EQUAL_STRING(SIM_VIN, vin);

    /* and as the standard has it for a car: seven data bytes a message,
       five messages (ISO 14230-4; ECU sim v2 since 2026-11-13) */
    vin[0] = '\0';
    TEST_ASSERT_TRUE(ap_veh_parse_vin_0902(
        "87 F1 10 49 02 01 00 00 00 31 05 \r"
        "87 F1 10 49 02 02 57 43 41 4E FE \r"
        "87 F1 10 49 02 03 30 46 57 30 D3 \r"
        "87 F1 10 49 02 04 50 30 30 30 B7 \r"
        "87 F1 10 49 02 05 30 30 30 31 99 \r", vin));
    TEST_ASSERT_EQUAL_STRING(SIM_VIN, vin);

    ap_resp_set_proto_hint('\0');
}

static int codes_of(const char *resp, uint8_t svc,
                    char out[][AP_DTC_CODE_LEN], size_t max)
{
    uint8_t p[AP_PAYLOAD_MAX];
    size_t n = payload(resp, p, sizeof(p));

    return ap_dtc_parse_codes(p, n, svc, out, max);
}

/* Trouble codes: three a message on these protocols, no count, as many
 * messages as the codes need. The lines are the bench's (ECU sim v2 with
 * five and with seven stored codes, 2026-11-12). Until then the first
 * message stood alone: a car with more than three codes showed three. */
void test_precan_trouble_codes_in_several_messages(void)
{
    static ap_resp_ecu_t e[AP_RESP_ECUS_MAX];
    static char many[45 * 34 + 1];
    char c[12][AP_DTC_CODE_LEN];
    uint8_t p[AP_PAYLOAD_MAX];
    size_t n;

    const char *five = "48 6B 10 43 03 00 04 20 01 71 9F \r"
                       "48 6B 10 43 01 01 04 42 00 00 4E \r";
    const uint8_t want5[] = { 0x43, 0x05, 0x03, 0x00, 0x04, 0x20, 0x01, 0x71,
                              0x01, 0x01, 0x04, 0x42 };

    /* ISO 9141-2: what a CAN ECU would have sent, the count in front */
    ap_resp_set_proto_hint('3');
    n = payload(five, p, sizeof(p));
    TEST_ASSERT_EQUAL_size_t(sizeof(want5), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want5, p, sizeof(want5));
    TEST_ASSERT_TRUE(ap_payload_matches_cmd("03", p, n));
    TEST_ASSERT_EQUAL(5, codes_of(five, 0x43, c, 12));
    TEST_ASSERT_EQUAL_STRING("P0300", c[0]);
    TEST_ASSERT_EQUAL_STRING("P0420", c[1]);
    TEST_ASSERT_EQUAL_STRING("P0171", c[2]);
    TEST_ASSERT_EQUAL_STRING("P0101", c[3]);
    TEST_ASSERT_EQUAL_STRING("P0442", c[4]);

    /* seven in three messages, a chassis and a network code among them */
    TEST_ASSERT_EQUAL(7, codes_of("48 6B 10 43 03 00 04 20 01 71 9F \r"
                                  "48 6B 10 43 01 01 04 42 40 35 C3 \r"
                                  "48 6B 10 43 C1 00 00 00 00 00 C7 \r",
                                  0x43, c, 12));
    TEST_ASSERT_EQUAL_STRING("P0442", c[4]);
    TEST_ASSERT_EQUAL_STRING("C0035", c[5]);
    TEST_ASSERT_EQUAL_STRING("U0100", c[6]);

    /* one message: two codes and an unused place, then no code at all */
    n = payload("48 6B 10 43 03 00 04 20 00 00 2D \r", p, sizeof(p));
    TEST_ASSERT_EQUAL_size_t(6, n);
    TEST_ASSERT_EQUAL_HEX8(0x02, p[1]);
    TEST_ASSERT_EQUAL(2, ap_dtc_parse_codes(p, n, 0x43, c, 12));
    TEST_ASSERT_EQUAL_STRING("P0420", c[1]);
    n = payload("48 6B 10 43 00 00 00 00 00 00 06 \r", p, sizeof(p));
    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_EQUAL_HEX8(0x00, p[1]);
    TEST_ASSERT_EQUAL(0, ap_dtc_parse_codes(p, n, 0x43, c, 12));

    /* service 07 the same way */
    TEST_ASSERT_EQUAL(4, codes_of("48 6B 10 47 01 72 03 01 03 02 86 \r"
                                  "48 6B 10 47 03 03 00 00 00 00 10 \r",
                                  0x47, c, 12));
    TEST_ASSERT_EQUAL_STRING("P0172", c[0]);
    TEST_ASSERT_EQUAL_STRING("P0303", c[3]);

    /* behind the line of the bus start, and with headers off */
    TEST_ASSERT_EQUAL(5, codes_of("BUS INIT: ...OK\r"
                                  "48 6B 10 43 03 00 04 20 01 71 9F \r"
                                  "48 6B 10 43 01 01 04 42 00 00 4E \r\r",
                                  0x43, c, 12));
    TEST_ASSERT_EQUAL(5, codes_of("43 03 00 04 20 01 71 \r"
                                  "43 01 01 04 42 00 00 \r", 0x43, c, 12));
    TEST_ASSERT_EQUAL_STRING("P0442", c[4]);

    /* two ECUs, their messages mixed: each keeps its own list */
    TEST_ASSERT_EQUAL(2, ap_resp_to_payloads(
        "48 6B 10 43 03 00 04 20 01 71 9F \r"
        "48 6B 18 43 07 00 00 00 00 00 15 \r"
        "48 6B 10 43 01 01 04 42 00 00 4E \r", e, AP_RESP_ECUS_MAX));
    TEST_ASSERT_EQUAL_HEX32(0x10, e[0].header);
    TEST_ASSERT_EQUAL_size_t(sizeof(want5), e[0].len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want5, e[0].payload, sizeof(want5));
    TEST_ASSERT_EQUAL_HEX32(0x18, e[1].header);
    TEST_ASSERT_EQUAL(1, ap_dtc_parse_codes(e[1].payload, e[1].len, 0x43, c,
                                            12));
    TEST_ASSERT_EQUAL_STRING("P0700", c[0]);

    /* a refusal behind a list is no part of it */
    TEST_ASSERT_EQUAL(3, codes_of("48 6B 10 43 03 00 04 20 01 71 9F \r"
                                  "48 6B 10 7F 03 11 56 \r", 0x43, c, 12));

    /* more than a payload holds: what fits, and the count says so */
    for (int i = 0; i < 45; i++)
    {
        memcpy(many + i * 34, "48 6B 10 43 01 01 01 02 01 03 0F \r", 34);
    }

    many[45 * 34] = '\0';
    n = payload(many, p, sizeof(p));
    TEST_ASSERT_EQUAL_size_t(AP_PAYLOAD_MAX, n);
    TEST_ASSERT_EQUAL_HEX8((AP_PAYLOAD_MAX - 2) / 2, p[1]);
    TEST_ASSERT_EQUAL(12, ap_dtc_parse_codes(p, n, 0x43, c, 12));

    /* SAE J1850 VPW: the same layout under its own checksum */
    ap_resp_set_proto_hint('2');
    TEST_ASSERT_EQUAL(5, codes_of("48 6B 10 43 03 00 04 20 01 71 0C \r"
                                  "48 6B 10 43 01 01 04 42 00 00 B1 \r",
                                  0x43, c, 12));
    TEST_ASSERT_EQUAL_STRING("P0442", c[4]);

    /* ISO 14230-4 as the standard has it: seven data bytes a message */
    ap_resp_set_proto_hint('5');
    n = payload("87 F1 10 43 03 00 04 20 01 71 64 \r"
                "87 F1 10 43 01 01 04 42 00 00 13 \r", p, sizeof(p));
    TEST_ASSERT_EQUAL_size_t(sizeof(want5), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want5, p, sizeof(want5));

    /* and the CAN way, one long message with the count in front (ECU sim
       v2 until 2026-11-13; an even length says so): read as it is */
    n = payload("8C F1 10 43 05 03 00 04 20 01 71 01 01 04 42 B6 \r", p,
                sizeof(p));
    TEST_ASSERT_EQUAL_size_t(sizeof(want5), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want5, p, sizeof(want5));
    n = payload("82 F1 10 43 00 C6 \r", p, sizeof(p));
    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_EQUAL(0, ap_dtc_parse_codes(p, n, 0x43, c, 12));

    ap_resp_set_proto_hint('\0');
}

/* The hint is a hint: what is plainly a CAN reply is read the CAN way under
 * it, and a line that is no message with a header is read as data. */
void test_precan_hint_does_not_take_what_is_not_its(void)
{
    uint8_t f[AP_FRAMES_MAX];
    char vin[AP_VIN_LEN];
    size_t n;
    const uint8_t want[] = { 0x04, 0x41, 0x0C, 0x0C, 0x80 };

    ap_resp_set_proto_hint('3');

    /* a CAN id in front */
    n = frames("7E8 04 41 0C 0C 80 \r", f, sizeof(f));
    TEST_ASSERT_EQUAL_size_t(sizeof(want), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f, sizeof(want));

    /* the rows of a long reply with headers off */
    TEST_ASSERT_TRUE(ap_veh_parse_vin_0902("014\r0: 49 02 01 31 57 43\r"
                                           "1: 41 4E 30 46 57 30 50\r"
                                           "2: 30 30 30 30 30 30 31\r", vin));
    TEST_ASSERT_EQUAL_STRING(SIM_VIN, vin);

    /* eight bytes of data whose last is not the sum of the others, and five
       whose fourth is no response byte: data, headers off */
    n = frames("41 00 BE 3F A8 13 00 00 \r", f, sizeof(f));
    TEST_ASSERT_EQUAL_HEX8(0x10, f[0]);     /* eight bytes: a first frame */
    TEST_ASSERT_EQUAL_HEX8(0x41, f[2]);
    n = frames("62 F1 90 05 5A \r", f, sizeof(f));
    TEST_ASSERT_EQUAL_HEX8(0x05, f[0]);
    TEST_ASSERT_EQUAL_HEX8(0x62, f[1]);

    /* the hint takes the legislated numbers 1 to 5 and nothing else */
    ap_resp_set_proto_hint('7');
    TEST_ASSERT_EQUAL('\0', ap_resp_proto_hint());
    ap_resp_set_proto_hint('0');
    TEST_ASSERT_EQUAL('\0', ap_resp_proto_hint());
    ap_resp_set_proto_hint('2');
    TEST_ASSERT_EQUAL('2', ap_resp_proto_hint());
    ap_resp_set_proto_hint('\0');
}

void run_precan_tests(void)
{
    RUN_TEST(test_precan_protocol_table);
    RUN_TEST(test_precan_request_header);
    RUN_TEST(test_precan_reply_decodes_as_on_can);
    RUN_TEST(test_precan_ecus_by_address);
    RUN_TEST(test_precan_vin_in_five_messages);
    RUN_TEST(test_precan_trouble_codes_in_several_messages);
    RUN_TEST(test_precan_hint_does_not_take_what_is_not_its);
}
