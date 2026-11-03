#include "ml_disco_pong.h"

#include <string.h>

#include "unity.h"
#include "esp_random.h"
#include "nacl_box.h"

static microlink_t s_ml;
static ml_peer_t s_peer;
static ml_rx_packet_t s_packet;
static uint8_t s_txid[12];
static uint8_t s_plaintext[32];
static uint8_t s_sent[110];
static uint32_t s_ip;
static uint16_t s_port;
static unsigned s_udp_calls;
static unsigned s_derp_calls;
static unsigned s_crypto_calls;
static int s_crypto_result;
static int s_udp_result;
static esp_err_t s_derp_result;

void setUp(void)
{
    memset(&s_ml, 0, sizeof(s_ml));
    memset(&s_peer, 0, sizeof(s_peer));
    memset(&s_packet, 0, sizeof(s_packet));
    memset(s_txid, 0x12, sizeof(s_txid));
    memset(s_plaintext, 0, sizeof(s_plaintext));
    memset(s_sent, 0, sizeof(s_sent));
    memset(s_ml.disco_public_key, 0x34, 32);
    memset(s_ml.disco_private_key, 0x56, 32);
    memset(s_peer.disco_key, 0x78, 32);
    memset(s_peer.public_key, 0x9a, 32);
    strcpy(s_peer.hostname, "test-peer");
    s_packet.src_ip = 0xc0000201;
    s_packet.src_port = 41641;
    s_udp_calls = s_derp_calls = s_crypto_calls = 0;
    s_crypto_result = 0;
    s_udp_result = sizeof(s_sent);
    s_derp_result = ESP_OK;
    s_ip = 0;
    s_port = 0;
}

void tearDown(void)
{
}

void esp_fill_random(void *buf, size_t len)
{
    memset(buf, 0xab, len);
}

int nacl_box(uint8_t *ciphertext, const uint8_t *plaintext, size_t plaintext_len,
             const uint8_t *nonce, const uint8_t *recipient_pk, const uint8_t *sender_sk)
{
    s_crypto_calls++;
    TEST_ASSERT_EQUAL_UINT(sizeof(s_plaintext), plaintext_len);
    TEST_ASSERT_EQUAL_MEMORY(s_peer.disco_key, recipient_pk, 32);
    TEST_ASSERT_EQUAL_MEMORY(s_ml.disco_private_key, sender_sk, 32);
    TEST_ASSERT_EACH_EQUAL_UINT8(0xab, nonce, NACL_BOX_NONCEBYTES);
    memcpy(s_plaintext, plaintext, plaintext_len);
    memset(ciphertext, 0xcd, plaintext_len + NACL_BOX_MACBYTES);
    return s_crypto_result;
}

int ml_disco_udp_sendto(microlink_t *ml, const uint8_t *data, size_t len,
                       uint32_t dest_ip, uint16_t dest_port)
{
    TEST_ASSERT_EQUAL_PTR(&s_ml, ml);
    TEST_ASSERT_EQUAL_UINT(sizeof(s_sent), len);
    memcpy(s_sent, data, len);
    s_ip = dest_ip;
    s_port = dest_port;
    s_udp_calls++;
    return s_udp_result;
}

esp_err_t ml_derp_queue_send(microlink_t *ml, const uint8_t *key,
                            const uint8_t *data, size_t len)
{
    TEST_ASSERT_EQUAL_PTR(&s_ml, ml);
    TEST_ASSERT_EQUAL_MEMORY(s_peer.public_key, key, 32);
    TEST_ASSERT_EQUAL_UINT(sizeof(s_sent), len);
    memcpy(s_sent, data, len);
    s_derp_calls++;
    return s_derp_result;
}

static void test_udp_reply_returns_only_to_source(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, ml_disco_reply_ping(&s_ml, &s_peer, &s_packet, s_txid));
    TEST_ASSERT_EQUAL_UINT(1, s_udp_calls);
    TEST_ASSERT_EQUAL_UINT(0, s_derp_calls);
    TEST_ASSERT_EQUAL_HEX32(s_packet.src_ip, s_ip);
    TEST_ASSERT_EQUAL_UINT(s_packet.src_port, s_port);
}

static void test_relay_reply_never_uses_udp(void)
{
    s_packet.via_derp = true;
    TEST_ASSERT_EQUAL(ESP_OK, ml_disco_reply_ping(&s_ml, &s_peer, &s_packet, s_txid));
    TEST_ASSERT_EQUAL_UINT(0, s_udp_calls);
    TEST_ASSERT_EQUAL_UINT(1, s_derp_calls);
}

static void test_relay_reply_needs_no_udp_address(void)
{
    s_packet.via_derp = true;
    s_packet.src_ip = 0;
    s_packet.src_port = 0;
    TEST_ASSERT_EQUAL(ESP_OK, ml_disco_reply_ping(&s_ml, &s_peer, &s_packet, s_txid));
    TEST_ASSERT_EQUAL_UINT(0, s_udp_calls);
    TEST_ASSERT_EQUAL_UINT(1, s_derp_calls);
}

static void test_pong_wire_format(void)
{
    const uint8_t magic[] = { 'T', 'S', 0xf0, 0x9f, 0x92, 0xac };
    const uint8_t address[] = { 0xff, 0xff, 192, 0, 2, 1, 0xa2, 0xa9 };
    TEST_ASSERT_EQUAL(ESP_OK, ml_disco_reply_ping(&s_ml, &s_peer, &s_packet, s_txid));
    TEST_ASSERT_EQUAL_MEMORY(magic, s_sent, sizeof(magic));
    TEST_ASSERT_EQUAL_MEMORY(s_ml.disco_public_key, s_sent + 6, 32);
    TEST_ASSERT_EACH_EQUAL_UINT8(0xab, s_sent + 38, 24);
    TEST_ASSERT_EACH_EQUAL_UINT8(0xcd, s_sent + 62, 48);
    TEST_ASSERT_EQUAL_UINT8(2, s_plaintext[0]);
    TEST_ASSERT_EQUAL_UINT8(0, s_plaintext[1]);
    TEST_ASSERT_EQUAL_MEMORY(s_txid, s_plaintext + 2, 12);
    TEST_ASSERT_EACH_EQUAL_UINT8(0, s_plaintext + 14, 10);
    TEST_ASSERT_EQUAL_MEMORY(address, s_plaintext + 24, sizeof(address));
}

static void test_missing_udp_address_does_not_fall_back(void)
{
    s_packet.src_ip = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                     ml_disco_reply_ping(&s_ml, &s_peer, &s_packet, s_txid));
    s_packet.src_ip = 0xc0000201;
    s_packet.src_port = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                     ml_disco_reply_ping(&s_ml, &s_peer, &s_packet, s_txid));
    TEST_ASSERT_EQUAL_UINT(0, s_crypto_calls);
    TEST_ASSERT_EQUAL_UINT(0, s_udp_calls + s_derp_calls);
}

static void test_crypto_failure_is_not_sent(void)
{
    s_crypto_result = -1;
    TEST_ASSERT_EQUAL(ESP_FAIL, ml_disco_reply_ping(&s_ml, &s_peer, &s_packet, s_txid));
    TEST_ASSERT_EQUAL_UINT(0, s_udp_calls + s_derp_calls);
}

static void test_udp_failure_does_not_fall_back(void)
{
    s_udp_result = -1;
    TEST_ASSERT_EQUAL(ESP_FAIL, ml_disco_reply_ping(&s_ml, &s_peer, &s_packet, s_txid));
    s_udp_result = 1;
    TEST_ASSERT_EQUAL(ESP_FAIL, ml_disco_reply_ping(&s_ml, &s_peer, &s_packet, s_txid));
    TEST_ASSERT_EQUAL_UINT(2, s_udp_calls);
    TEST_ASSERT_EQUAL_UINT(0, s_derp_calls);
}

static void test_relay_failure_does_not_fall_back(void)
{
    s_packet.via_derp = true;
    s_derp_result = ESP_ERR_NO_MEM;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM,
                     ml_disco_reply_ping(&s_ml, &s_peer, &s_packet, s_txid));
    TEST_ASSERT_EQUAL_UINT(0, s_udp_calls);
    TEST_ASSERT_EQUAL_UINT(1, s_derp_calls);
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_udp_reply_returns_only_to_source);
    RUN_TEST(test_relay_reply_never_uses_udp);
    RUN_TEST(test_relay_reply_needs_no_udp_address);
    RUN_TEST(test_pong_wire_format);
    RUN_TEST(test_missing_udp_address_does_not_fall_back);
    RUN_TEST(test_crypto_failure_is_not_sent);
    RUN_TEST(test_udp_failure_does_not_fall_back);
    RUN_TEST(test_relay_failure_does_not_fall_back);
    UNITY_END();
}
