/**
 * @file test_ram.c
 * @brief Host tests for the Bluetooth memory profile's pure layer (settings
 *        v5, 2026-10-31): the profile words, the profile-to-controller
 *        mapping, the validation rule and the v4 -> v5 migration verdict.
 */
#include <string.h>

#include "unity.h"

#include "ble_manager_private.h"

/* the build's Kconfig values as BT_CONTROLLER_INIT_CONFIG_DEFAULT() carries
   them on the WiCAN PRO (sdkconfig.defaults, 2026-09-22) */
static const blm_ram_cfg_t FULL = { 4, 12, 100, true };

static void test_ram_profile_words(void)
{
    TEST_ASSERT_EQUAL(BLM_RAM_FULL, blm_ident_ram_profile("full"));
    TEST_ASSERT_EQUAL(BLM_RAM_LEAN, blm_ident_ram_profile("lean"));
    TEST_ASSERT_EQUAL(BLM_RAM_CUSTOM, blm_ident_ram_profile("custom"));
    TEST_ASSERT_EQUAL(BLM_RAM_LEAN, blm_ident_ram_profile(NULL));    /* the default */
    TEST_ASSERT_EQUAL(BLM_RAM_LEAN, blm_ident_ram_profile("bogus"));

    TEST_ASSERT_EQUAL_STRING("full", blm_ident_ram_profile_word(BLM_RAM_FULL));
    TEST_ASSERT_EQUAL_STRING("lean", blm_ident_ram_profile_word(BLM_RAM_LEAN));
    TEST_ASSERT_EQUAL_STRING("custom", blm_ident_ram_profile_word(BLM_RAM_CUSTOM));
    TEST_ASSERT_EQUAL_STRING("lean", blm_ident_ram_profile_word((blm_ram_profile_t)99));
}

static void test_ram_config_full_keeps_the_build(void)
{
    blm_ram_cfg_t custom = { 2, 0, 0, false };
    blm_ram_cfg_t out;

    memset(&out, 0xAA, sizeof(out));
    blm_ident_ram_config(BLM_RAM_FULL, &FULL, &custom, &out);
    TEST_ASSERT_EQUAL(4, out.max_act);
    TEST_ASSERT_EQUAL(12, out.acl_tx_bufs);
    TEST_ASSERT_EQUAL(100, out.scan_cache);
    TEST_ASSERT_TRUE(out.ble5);
}

static void test_ram_config_lean_is_the_measured_set(void)
{
    blm_ram_cfg_t custom = { 10, 0, 1000, false };
    blm_ram_cfg_t out;

    blm_ident_ram_config(BLM_RAM_LEAN, &FULL, &custom, &out);
    TEST_ASSERT_EQUAL(BLM_RAM_LEAN_MAX_ACT, out.max_act);
    TEST_ASSERT_EQUAL(BLM_RAM_LEAN_ACL_TX_BUFS, out.acl_tx_bufs);
    TEST_ASSERT_EQUAL(BLM_RAM_LEAN_SCAN_CACHE, out.scan_cache);
    TEST_ASSERT_TRUE(out.ble5); /* the build's feature set, kept */
    /* one phone plus the legacy advertising set, nothing to scan with; the
       static ACL TX buffers kept (the notification-loss fix of 2026-09-22) */
    TEST_ASSERT_EQUAL(2, out.max_act);
    TEST_ASSERT_EQUAL(12, out.acl_tx_bufs);
    TEST_ASSERT_EQUAL(0, out.scan_cache);
}

static void test_ram_config_custom_copies_and_clamps(void)
{
    blm_ram_cfg_t custom = { 3, 6, 50, false };
    blm_ram_cfg_t out;

    blm_ident_ram_config(BLM_RAM_CUSTOM, &FULL, &custom, &out);
    TEST_ASSERT_EQUAL(3, out.max_act);
    TEST_ASSERT_EQUAL(6, out.acl_tx_bufs);
    TEST_ASSERT_EQUAL(50, out.scan_cache);
    /* the BLE 5 feature set is never a knob: a controller without it under
       the host built with it crashed the device (2026-10-31) */
    TEST_ASSERT_TRUE(out.ble5);

    /* out of the controller's ranges: clamped, never refused */
    blm_ram_cfg_t wild = { 1, 20, 5, true };

    blm_ident_ram_config(BLM_RAM_CUSTOM, &FULL, &wild, &out);
    TEST_ASSERT_EQUAL(2, out.max_act);      /* a connection AND advertising */
    TEST_ASSERT_EQUAL(12, out.acl_tx_bufs);
    TEST_ASSERT_EQUAL(10, out.scan_cache);  /* the smallest usable cache */
    TEST_ASSERT_TRUE(out.ble5);

    /* 0 = scanning off stays 0 */
    blm_ram_cfg_t off = { 2, 0, 0, true };

    blm_ident_ram_config(BLM_RAM_CUSTOM, &FULL, &off, &out);
    TEST_ASSERT_EQUAL(0, out.scan_cache);
    TEST_ASSERT_EQUAL(0, out.acl_tx_bufs);
}

static void test_ram_check_lean_needs_legacy_on_1m(void)
{
    /* Lean with the 4.2 words: fine */
    TEST_ASSERT_NULL(blm_ident_ram_check(BLM_RAM_LEAN, BLM_ADV_LEGACY, BLM_PHY_1M, 100));

    /* Lean with a BLE 5 word: refused, and the reason names the way out */
    const char *why = blm_ident_ram_check(BLM_RAM_LEAN, BLM_ADV_EXTENDED, BLM_PHY_1M, 100);

    TEST_ASSERT_NOT_NULL(why);
    TEST_ASSERT_NOT_NULL(strstr(why, "legacy"));
    TEST_ASSERT_NOT_NULL(strstr(why, "Full"));
    TEST_ASSERT_NOT_NULL(blm_ident_ram_check(BLM_RAM_LEAN, BLM_ADV_BOTH, BLM_PHY_1M, 100));
    TEST_ASSERT_NOT_NULL(blm_ident_ram_check(BLM_RAM_LEAN, BLM_ADV_LEGACY, BLM_PHY_2M, 100));
    TEST_ASSERT_NOT_NULL(blm_ident_ram_check(BLM_RAM_LEAN, BLM_ADV_LEGACY,
                                             BLM_PHY_1M | BLM_PHY_2M, 100));

    /* Full and Custom keep every BLE 5 combination */
    TEST_ASSERT_NULL(blm_ident_ram_check(BLM_RAM_FULL, BLM_ADV_BOTH, BLM_PHY_CODED, 100));
    TEST_ASSERT_NULL(blm_ident_ram_check(BLM_RAM_CUSTOM, BLM_ADV_EXTENDED, BLM_PHY_2M, 100));

    /* a custom scan cache: 0 (off) or a usable size */
    TEST_ASSERT_NULL(blm_ident_ram_check(BLM_RAM_CUSTOM, BLM_ADV_LEGACY, BLM_PHY_1M, 0));
    TEST_ASSERT_NULL(blm_ident_ram_check(BLM_RAM_CUSTOM, BLM_ADV_LEGACY, BLM_PHY_1M, 10));
    TEST_ASSERT_NOT_NULL(blm_ident_ram_check(BLM_RAM_CUSTOM, BLM_ADV_LEGACY, BLM_PHY_1M, 5));
    /* the same number is nobody's business on the other profiles */
    TEST_ASSERT_NULL(blm_ident_ram_check(BLM_RAM_FULL, BLM_ADV_LEGACY, BLM_PHY_1M, 5));
    TEST_ASSERT_NULL(blm_ident_ram_check(BLM_RAM_LEAN, BLM_ADV_LEGACY, BLM_PHY_1M, 5));
}

static void test_ram_migrated_keeps_a_ble5_choice(void)
{
    /* the 4.2 defaults: the schema default (Lean) through fill-missing */
    TEST_ASSERT_NULL(blm_ident_ram_migrated(BLM_ADV_LEGACY, BLM_PHY_1M));

    /* a BLE 5 choice on v4 is kept: Full */
    TEST_ASSERT_EQUAL_STRING("full", blm_ident_ram_migrated(BLM_ADV_BOTH, BLM_PHY_1M));
    TEST_ASSERT_EQUAL_STRING("full", blm_ident_ram_migrated(BLM_ADV_EXTENDED, BLM_PHY_1M));
    TEST_ASSERT_EQUAL_STRING("full", blm_ident_ram_migrated(BLM_ADV_LEGACY, BLM_PHY_2M));
    TEST_ASSERT_EQUAL_STRING("full", blm_ident_ram_migrated(BLM_ADV_LEGACY, BLM_PHY_CODED));
    TEST_ASSERT_EQUAL_STRING("full", blm_ident_ram_migrated(BLM_ADV_LEGACY,
                                                            BLM_PHY_1M | BLM_PHY_2M));
}

/* v6 (2026-11-02): the advertised address word, public unless "private" */
static void test_address_words_default_public(void)
{
    TEST_ASSERT_FALSE(blm_ident_address_private("public"));
    TEST_ASSERT_FALSE(blm_ident_address_private(NULL));
    TEST_ASSERT_FALSE(blm_ident_address_private(""));
    TEST_ASSERT_FALSE(blm_ident_address_private("Private"));
    TEST_ASSERT_FALSE(blm_ident_address_private("random"));
    TEST_ASSERT_TRUE(blm_ident_address_private("private"));

    TEST_ASSERT_EQUAL_STRING("public", blm_ident_address_word(false));
    TEST_ASSERT_EQUAL_STRING("private", blm_ident_address_word(true));
}

void run_ram_tests(void)
{
    RUN_TEST(test_address_words_default_public);
    RUN_TEST(test_ram_profile_words);
    RUN_TEST(test_ram_config_full_keeps_the_build);
    RUN_TEST(test_ram_config_lean_is_the_measured_set);
    RUN_TEST(test_ram_config_custom_copies_and_clamps);
    RUN_TEST(test_ram_check_lean_needs_legacy_on_1m);
    RUN_TEST(test_ram_migrated_keeps_a_ble5_choice);
}
