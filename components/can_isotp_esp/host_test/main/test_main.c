/**
 * @file test_main.c
 * @brief Unit tests for the provider's pure PDU mailbox ring: FIFO order,
 *        the token contract (put returns "grew"), drop-oldest when full,
 *        the NO_MEM-means-consumed recv contract, refused puts.
 */
#include <string.h>

#include "unity.h"

#include "can_isotp_esp_private.h"

static isotp_mbox_slot_t s_slots[CAN_ISOTP_ESP_MBOX_SLOTS];
static isotp_mbox_t s_m;

static void fresh(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    isotp_mbox_init(&s_m, s_slots, CAN_ISOTP_ESP_MBOX_SLOTS);
}

static void put_byte_pdu(uint8_t tag, size_t len)
{
    uint8_t pdu[CAN_ISOTP_ESP_MAX_PDU];

    memset(pdu, tag, len);
    isotp_mbox_put(&s_m, pdu, len);
}

void test_empty_take_is_not_found(void)
{
    uint8_t buf[8];
    size_t n = 99;

    fresh();
    TEST_ASSERT_EQUAL(0, (int)isotp_mbox_count(&s_m));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, isotp_mbox_take(&s_m, buf,
                                                          sizeof(buf), &n));
    TEST_ASSERT_EQUAL(0, (int)n);
}

void test_fifo_order_and_token_contract(void)
{
    const uint8_t a[] = { 0x62, 0xF1, 0x90, 0x31 };
    const uint8_t b[] = { 0x7F, 0x22, 0x78 };
    uint8_t buf[16];
    size_t n = 0;

    fresh();
    TEST_ASSERT_TRUE(isotp_mbox_put(&s_m, a, sizeof(a)));  /* grew: token */
    TEST_ASSERT_TRUE(isotp_mbox_put(&s_m, b, sizeof(b)));  /* grew: token */
    TEST_ASSERT_EQUAL(2, (int)isotp_mbox_count(&s_m));

    TEST_ASSERT_EQUAL(ESP_OK, isotp_mbox_take(&s_m, buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL(sizeof(a), n);
    TEST_ASSERT_EQUAL_MEMORY(a, buf, sizeof(a));

    TEST_ASSERT_EQUAL(ESP_OK, isotp_mbox_take(&s_m, buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL(sizeof(b), n);
    TEST_ASSERT_EQUAL_MEMORY(b, buf, sizeof(b));

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, isotp_mbox_take(&s_m, buf,
                                                          sizeof(buf), &n));
}

void test_oversize_pdu_is_consumed_with_no_mem(void)
{
    uint8_t small[16];
    size_t n = 0;

    fresh();
    put_byte_pdu(0xAA, 300);              /* a 300 B DID read              */
    put_byte_pdu(0xBB, 5);                /* then a small one behind it    */

    /* the consumer's 16 B buffer cannot hold the 300 B PDU: the contract
       says NO_MEM and CONSUMED (callers drain stale traffic on it) */
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, isotp_mbox_take(&s_m, small,
                                                       sizeof(small), &n));
    TEST_ASSERT_EQUAL(300, (int)n);       /* tells the real size           */
    TEST_ASSERT_EQUAL(1, (int)isotp_mbox_count(&s_m));

    /* the next take is the small one, not the oversize again */
    TEST_ASSERT_EQUAL(ESP_OK, isotp_mbox_take(&s_m, small, sizeof(small), &n));
    TEST_ASSERT_EQUAL(5, (int)n);
    TEST_ASSERT_EQUAL_HEX8(0xBB, small[0]);
}

void test_full_ring_drops_oldest_without_new_token(void)
{
    uint8_t buf[8];
    size_t n = 0;

    fresh();

    for (uint8_t i = 1; i <= CAN_ISOTP_ESP_MBOX_SLOTS; i++)
    {
        uint8_t pdu[2] = { i, i };
        TEST_ASSERT_TRUE(isotp_mbox_put(&s_m, pdu, sizeof(pdu)));
    }

    TEST_ASSERT_EQUAL(CAN_ISOTP_ESP_MBOX_SLOTS, (int)isotp_mbox_count(&s_m));

    /* one more: the OLDEST (tag 1) goes, count stays, no new token */
    const uint8_t extra[2] = { 0xEE, 0xEE };
    TEST_ASSERT_FALSE(isotp_mbox_put(&s_m, extra, sizeof(extra)));
    TEST_ASSERT_EQUAL(CAN_ISOTP_ESP_MBOX_SLOTS, (int)isotp_mbox_count(&s_m));
    TEST_ASSERT_EQUAL_UINT32(1, s_m.dropped);

    TEST_ASSERT_EQUAL(ESP_OK, isotp_mbox_take(&s_m, buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL_HEX8(2, buf[0]);    /* tag 1 is gone                 */

    /* drain: 3, 4, then the extra */
    for (uint8_t i = 3; i <= CAN_ISOTP_ESP_MBOX_SLOTS; i++)
    {
        TEST_ASSERT_EQUAL(ESP_OK, isotp_mbox_take(&s_m, buf, sizeof(buf), &n));
        TEST_ASSERT_EQUAL_HEX8(i, buf[0]);
    }

    TEST_ASSERT_EQUAL(ESP_OK, isotp_mbox_take(&s_m, buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL_HEX8(0xEE, buf[0]);
    TEST_ASSERT_EQUAL(0, (int)isotp_mbox_count(&s_m));
}

void test_refused_puts(void)
{
    uint8_t big[CAN_ISOTP_ESP_MAX_PDU + 1];
    uint8_t one = 1;

    fresh();
    memset(big, 0, sizeof(big));
    TEST_ASSERT_FALSE(isotp_mbox_put(&s_m, big, sizeof(big)));  /* too big */
    TEST_ASSERT_FALSE(isotp_mbox_put(&s_m, &one, 0));           /* empty   */
    TEST_ASSERT_FALSE(isotp_mbox_put(&s_m, NULL, 4));
    TEST_ASSERT_EQUAL(0, (int)isotp_mbox_count(&s_m));
    TEST_ASSERT_EQUAL_UINT32(3, s_m.refused);

    /* exactly the cap is fine */
    TEST_ASSERT_TRUE(isotp_mbox_put(&s_m, big, CAN_ISOTP_ESP_MAX_PDU));
    TEST_ASSERT_EQUAL(1, (int)isotp_mbox_count(&s_m));
}

void test_wraparound_keeps_order(void)
{
    uint8_t buf[4];
    size_t n = 0;

    fresh();

    /* put/take across the ring boundary many times */
    for (uint8_t round = 0; round < 3 * CAN_ISOTP_ESP_MBOX_SLOTS; round++)
    {
        uint8_t pdu[3] = { round, 0x11, 0x22 };
        TEST_ASSERT_TRUE(isotp_mbox_put(&s_m, pdu, sizeof(pdu)));
        TEST_ASSERT_EQUAL(ESP_OK, isotp_mbox_take(&s_m, buf, sizeof(buf), &n));
        TEST_ASSERT_EQUAL(3, (int)n);
        TEST_ASSERT_EQUAL_HEX8(round, buf[0]);
    }

    TEST_ASSERT_EQUAL_UINT32(0, s_m.dropped);
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_empty_take_is_not_found);
    RUN_TEST(test_fifo_order_and_token_contract);
    RUN_TEST(test_oversize_pdu_is_consumed_with_no_mem);
    RUN_TEST(test_full_ring_drops_oldest_without_new_token);
    RUN_TEST(test_refused_puts);
    RUN_TEST(test_wraparound_keeps_order);
    UNITY_END();
}
