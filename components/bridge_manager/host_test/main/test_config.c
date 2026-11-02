/**
 * @file test_config.c
 * @brief Host tests for the pure bridge-config layer: parse defaults,
 *        registry-name checks, uniqueness, single-consumer rule.
 */
#include <string.h>

#include "unity.h"

#include "bridge_manager.h"
#include "bridge_manager_private.h"

/* obd is a fan-out provider (multi_consumer) like production; the rest
   are single-stream */
static const char *EPS[]   = { "obd", "tcp0", "udp0", "usb_obd" };
static const bool  MULTI[] = { true,  false,  false,  false };
static const char *TRS[]   = { "slcan" };

static esp_err_t validate(const char *json, char *err, size_t err_len)
{
    cJSON *arr = cJSON_Parse(json);
    esp_err_t r = bm_validate_bridges(arr, EPS, MULTI, 4, TRS, 1,
                                      err, err_len);

    cJSON_Delete(arr);
    return r;
}

static void test_parse_defaults_raw_disabled(void)
{
    cJSON *item = cJSON_Parse("{\"name\":\"br0\",\"a\":\"obd\",\"b\":\"tcp0\"}");
    bm_bridge_cfg_t cfg;

    TEST_ASSERT_EQUAL(ESP_OK, bm_parse_bridge(item, &cfg));
    TEST_ASSERT_EQUAL_STRING("raw", cfg.translator); /* code default */
    TEST_ASSERT_FALSE(cfg.enabled);                  /* code default */
    cJSON_Delete(item);
}

static void test_valid_bridge_set_accepted(void)
{
    char err[96] = "";

    TEST_ASSERT_EQUAL(ESP_OK, validate(
        "[{\"name\":\"br0\",\"a\":\"obd\",\"b\":\"tcp0\",\"enabled\":true},"
         "{\"name\":\"br1\",\"a\":\"usb_obd\",\"b\":\"udp0\","
          "\"translator\":\"slcan\",\"enabled\":true}]", err, sizeof(err)));
}

/* A runtime PUT no longer requires endpoint names to be registered
   (2026-10-29, the field report in internal/unable-to-save-config): the
   Connections card creates a socket server or a WebSocket channel and its
   bridge in ONE submit, and the jack registers at the next boot; a row
   left behind by a server renamed or removed must not refuse every later
   save of the list. build_bridge degrades a name that never resolves
   alone (bridge_manager_reason says why). */
static void test_unknown_endpoint_accepted_at_runtime(void)
{
    char err[96] = "";

    TEST_ASSERT_EQUAL(ESP_OK, validate(
        "[{\"name\":\"br0\",\"a\":\"obd\",\"b\":\"nope\",\"enabled\":true}]",
        err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("", err);
}

/* an unregistered name counts as a single-stream jack (every dynamic jack
   is): two ENABLED bridges on it are still refused */
static void test_unknown_endpoint_single_consumer(void)
{
    char err[96] = "";

    TEST_ASSERT_NOT_EQUAL(ESP_OK, validate(
        "[{\"name\":\"br0\",\"a\":\"obd\",\"b\":\"nope\",\"enabled\":true},"
         "{\"name\":\"br1\",\"a\":\"usb_obd\",\"b\":\"nope\",\"enabled\":true}]",
        err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "two enabled"));
}

static void test_unknown_translator_rejected(void)
{
    char err[96] = "";

    TEST_ASSERT_NOT_EQUAL(ESP_OK, validate(
        "[{\"name\":\"br0\",\"a\":\"obd\",\"b\":\"tcp0\","
          "\"translator\":\"gvret\"}]", err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "gvret"));
}

static void test_raw_always_known(void)
{
    char err[96] = "";

    TEST_ASSERT_EQUAL(ESP_OK, validate(
        "[{\"name\":\"br0\",\"a\":\"obd\",\"b\":\"tcp0\","
          "\"translator\":\"raw\"}]", err, sizeof(err)));
}

/* count < 0 = BOOT apply, registry not final: existence checks AND the
   single-consumer rule skipped (endpoint capabilities are unknowable
   before the jacks register; single-stream providers refuse the second
   subscribe at build time). a != b / uniqueness still hold. */
static void test_boot_lenient_mode(void)
{
    char err[96] = "";
    cJSON *unknown_names = cJSON_Parse(
        "[{\"name\":\"br0\",\"a\":\"nope\",\"b\":\"also_nope\","
          "\"translator\":\"mystery\"}]");
    cJSON *still_bad = cJSON_Parse(
        "[{\"name\":\"br0\",\"a\":\"x\",\"b\":\"x\"}]");
    cJSON *dup = cJSON_Parse(
        "[{\"name\":\"br0\",\"a\":\"p\",\"b\":\"q\",\"enabled\":true},"
         "{\"name\":\"br1\",\"a\":\"p\",\"b\":\"r\",\"enabled\":true}]");

    TEST_ASSERT_EQUAL(ESP_OK, bm_validate_bridges(
        unknown_names, NULL, NULL, -1, NULL, -1, err, sizeof(err)));

    /* a == b still rejected in lenient mode */
    TEST_ASSERT_NOT_EQUAL(ESP_OK, bm_validate_bridges(
        still_bad, NULL, NULL, -1, NULL, -1, err, sizeof(err)));

    /* single-consumer rule deferred to build time in lenient mode (the
       default TCP+USB obd pair must boot before capabilities are known) */
    TEST_ASSERT_EQUAL(ESP_OK, bm_validate_bridges(
        dup, NULL, NULL, -1, NULL, -1, err, sizeof(err)));

    cJSON_Delete(unknown_names);
    cJSON_Delete(still_bad);
    cJSON_Delete(dup);
}

static void test_same_endpoint_both_sides_rejected(void)
{
    char err[96] = "";

    TEST_ASSERT_NOT_EQUAL(ESP_OK, validate(
        "[{\"name\":\"br0\",\"a\":\"obd\",\"b\":\"obd\"}]",
        err, sizeof(err)));
}

static void test_single_consumer_rule(void)
{
    char err[96] = "";

    /* a single-stream endpoint consumed by two ENABLED bridges -> rejected */
    TEST_ASSERT_NOT_EQUAL(ESP_OK, validate(
        "[{\"name\":\"br0\",\"a\":\"usb_obd\",\"b\":\"tcp0\",\"enabled\":true},"
         "{\"name\":\"br1\",\"a\":\"usb_obd\",\"b\":\"udp0\",\"enabled\":true}]",
        err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "two enabled"));

    /* same pairing is fine when one is disabled */
    TEST_ASSERT_EQUAL(ESP_OK, validate(
        "[{\"name\":\"br0\",\"a\":\"usb_obd\",\"b\":\"tcp0\",\"enabled\":true},"
         "{\"name\":\"br1\",\"a\":\"usb_obd\",\"b\":\"udp0\",\"enabled\":false}]",
        err, sizeof(err)));
}

static void test_multi_consumer_exempt(void)
{
    char err[96] = "";

    /* obd fans RX out to every subscriber -> two enabled bridges OK
       (the default TCP + USB passthrough pair) */
    TEST_ASSERT_EQUAL(ESP_OK, validate(
        "[{\"name\":\"br0\",\"a\":\"obd\",\"b\":\"tcp0\",\"enabled\":true},"
         "{\"name\":\"br1\",\"a\":\"usb_obd\",\"b\":\"obd\",\"enabled\":true}]",
        err, sizeof(err)));

    /* the exemption is obd's alone: the OTHER shared endpoint still trips */
    TEST_ASSERT_NOT_EQUAL(ESP_OK, validate(
        "[{\"name\":\"br0\",\"a\":\"obd\",\"b\":\"tcp0\",\"enabled\":true},"
         "{\"name\":\"br1\",\"a\":\"tcp0\",\"b\":\"udp0\",\"enabled\":true}]",
        err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "two enabled"));
}

static void test_duplicate_bridge_name_rejected(void)
{
    char err[96] = "";

    TEST_ASSERT_NOT_EQUAL(ESP_OK, validate(
        "[{\"name\":\"br0\",\"a\":\"obd\",\"b\":\"tcp0\"},"
         "{\"name\":\"br0\",\"a\":\"usb_obd\",\"b\":\"udp0\"}]",
        err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "duplicate"));
}

/* v1 -> v2 (2026-11-02): the Bluetooth bridge appended to a stored list
   without one; a list that uses `ble` (even disabled) and a full table are
   left alone */
static void test_ble_default_added_when_absent(void)
{
    cJSON *arr = cJSON_Parse(
        "[{\"name\":\"br_obd\",\"a\":\"obd\",\"b\":\"ws_obd\",\"enabled\":true}]");

    TEST_ASSERT_TRUE(bm_bridges_add_ble_default(arr));
    TEST_ASSERT_EQUAL(2, cJSON_GetArraySize(arr));
    TEST_ASSERT_TRUE(bm_bridges_use_endpoint(arr, "ble"));

    const cJSON *row = cJSON_GetArrayItem(arr, 1);

    TEST_ASSERT_EQUAL_STRING("br_ble_obd",
        cJSON_GetObjectItemCaseSensitive(row, "name")->valuestring);
    TEST_ASSERT_EQUAL_STRING("obd", cJSON_GetObjectItemCaseSensitive(row, "a")->valuestring);
    TEST_ASSERT_EQUAL_STRING("ble", cJSON_GetObjectItemCaseSensitive(row, "b")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(row, "enabled")));
    /* a second call changes nothing */
    TEST_ASSERT_FALSE(bm_bridges_add_ble_default(arr));
    TEST_ASSERT_EQUAL(2, cJSON_GetArraySize(arr));
    cJSON_Delete(arr);
}

static void test_ble_default_left_alone_when_ble_is_used(void)
{
    cJSON *arr = cJSON_Parse(
        "[{\"name\":\"br_ble_can\",\"a\":\"can\",\"b\":\"ble\",\"enabled\":false}]");

    TEST_ASSERT_FALSE(bm_bridges_add_ble_default(arr));
    TEST_ASSERT_EQUAL(1, cJSON_GetArraySize(arr));
    cJSON_Delete(arr);
}

static void test_ble_default_left_alone_when_full(void)
{
    char json[512] = "[";

    for (int i = 0; i < BRIDGE_MANAGER_MAX_BRIDGES; i++)
    {
        char row[80];

        snprintf(row, sizeof(row), "%s{\"name\":\"b%d\",\"a\":\"obd\",\"b\":\"tcp%d\"}",
                 i ? "," : "", i, i);
        strcat(json, row);
    }

    strcat(json, "]");

    cJSON *arr = cJSON_Parse(json);

    TEST_ASSERT_NOT_NULL(arr);
    TEST_ASSERT_FALSE(bm_bridges_add_ble_default(arr));
    TEST_ASSERT_EQUAL(BRIDGE_MANAGER_MAX_BRIDGES, cJSON_GetArraySize(arr));
    cJSON_Delete(arr);
}

void run_config_tests(void)
{
    RUN_TEST(test_ble_default_added_when_absent);
    RUN_TEST(test_ble_default_left_alone_when_ble_is_used);
    RUN_TEST(test_ble_default_left_alone_when_full);
    RUN_TEST(test_parse_defaults_raw_disabled);
    RUN_TEST(test_valid_bridge_set_accepted);
    RUN_TEST(test_unknown_endpoint_accepted_at_runtime);
    RUN_TEST(test_unknown_endpoint_single_consumer);
    RUN_TEST(test_unknown_translator_rejected);
    RUN_TEST(test_raw_always_known);
    RUN_TEST(test_boot_lenient_mode);
    RUN_TEST(test_same_endpoint_both_sides_rejected);
    RUN_TEST(test_single_consumer_rule);
    RUN_TEST(test_multi_consumer_exempt);
    RUN_TEST(test_duplicate_bridge_name_rejected);
}
