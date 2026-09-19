/**
 * @file test_util.c
 * @brief Host tests for the pure api_http utility layer: password
 *        redaction/unredaction, settings route parsing, level mapping.
 */
#include <string.h>

#include "esp_log.h"
#include "unity.h"

#include "api_http_private.h"

/* ---- redaction ----------------------------------------------------------------- */

static void test_redact_passwords(void)
{
    cJSON *obj = cJSON_Parse(
        "{\"sta_ssid\":\"Home\",\"sta_password\":\"secret\","
        "\"fallback1_password\":\"other\",\"ap_channel\":6,"
        "\"password_hint\":\"keepme\"}");

    api_util_redact(obj);

    TEST_ASSERT_EQUAL_STRING("", cJSON_GetObjectItem(obj,
                                 "sta_password")->valuestring);
    TEST_ASSERT_EQUAL_STRING("", cJSON_GetObjectItem(obj,
                                 "fallback1_password")->valuestring);
    /* only secret-suffixed keys are redacted */
    TEST_ASSERT_EQUAL_STRING("Home", cJSON_GetObjectItem(obj,
                                 "sta_ssid")->valuestring);
    TEST_ASSERT_EQUAL_STRING("keepme", cJSON_GetObjectItem(obj,
                                 "password_hint")->valuestring);
    TEST_ASSERT_EQUAL(6, cJSON_GetObjectItem(obj, "ap_channel")->valueint);
    cJSON_Delete(obj);
}

static void test_redact_wireguard_keys(void)
{
    /* the 2026-07-07 suffixes (vpn_manager): private/preshared keys are
       secrets, the peer's PUBLIC key is display data */
    cJSON *obj = cJSON_Parse(
        "{\"private_key\":\"wg-priv\",\"preshared_key\":\"wg-psk\","
        "\"peer_public_key\":\"wg-pub\",\"endpoint\":\"vpn.host\"}");

    api_util_redact(obj);

    TEST_ASSERT_EQUAL_STRING("", cJSON_GetObjectItem(obj,
                                 "private_key")->valuestring);
    TEST_ASSERT_EQUAL_STRING("", cJSON_GetObjectItem(obj,
                                 "preshared_key")->valuestring);
    TEST_ASSERT_EQUAL_STRING("wg-pub", cJSON_GetObjectItem(obj,
                                 "peer_public_key")->valuestring);
    TEST_ASSERT_EQUAL_STRING("vpn.host", cJSON_GetObjectItem(obj,
                                 "endpoint")->valuestring);
    cJSON_Delete(obj);
}

static void test_unredact_keeps_stored_on_empty(void)
{
    cJSON *stored = cJSON_Parse(
        "{\"sta_password\":\"stored-secret\",\"ap_password\":\"ap-secret\"}");
    cJSON *in = cJSON_Parse(
        "{\"sta_password\":\"\",\"ap_password\":\"new-pass\"}");

    api_util_unredact(in, stored);

    /* "" means keep the stored value; a real value passes through */
    TEST_ASSERT_EQUAL_STRING("stored-secret",
        cJSON_GetObjectItem(in, "sta_password")->valuestring);
    TEST_ASSERT_EQUAL_STRING("new-pass",
        cJSON_GetObjectItem(in, "ap_password")->valuestring);
    cJSON_Delete(in);
    cJSON_Delete(stored);
}

static void test_unredact_no_stored_value(void)
{
    cJSON *stored = cJSON_Parse("{}");
    cJSON *in = cJSON_Parse("{\"sta_password\":\"\"}");

    api_util_unredact(in, stored);

    /* nothing stored: the empty string stands (validates as empty) */
    TEST_ASSERT_EQUAL_STRING("",
        cJSON_GetObjectItem(in, "sta_password")->valuestring);
    cJSON_Delete(in);
    cJSON_Delete(stored);
}

static void test_redact_array_items(void)
{
    cJSON *obj = cJSON_Parse(
        "{\"enabled\":true,\"destinations\":["
        "{\"name\":\"d1\",\"url\":\"http://x\",\"auth_token\":\"tok\","
        "\"api_key\":\"key\",\"basic_password\":\"pw\"},"
        "{\"name\":\"d2\",\"auth_token\":\"\"}],\"nums\":[1,2]}");

    api_util_redact(obj);

    const cJSON *arr = cJSON_GetObjectItem(obj, "destinations");
    const cJSON *d1 = cJSON_GetArrayItem(arr, 0);

    /* every secret-suffixed key inside the rows is blanked; the rest and
       non-object arrays are untouched */
    TEST_ASSERT_EQUAL_STRING("", cJSON_GetObjectItem(d1, "auth_token")->valuestring);
    TEST_ASSERT_EQUAL_STRING("", cJSON_GetObjectItem(d1, "api_key")->valuestring);
    TEST_ASSERT_EQUAL_STRING("", cJSON_GetObjectItem(d1, "basic_password")->valuestring);
    TEST_ASSERT_EQUAL_STRING("http://x", cJSON_GetObjectItem(d1, "url")->valuestring);
    TEST_ASSERT_EQUAL(2, cJSON_GetArraySize(cJSON_GetObjectItem(obj, "nums")));
    cJSON_Delete(obj);
}

static void test_unredact_array_items_by_name(void)
{
    cJSON *stored = cJSON_Parse(
        "{\"destinations\":["
        "{\"name\":\"d1\",\"auth_token\":\"tok1\",\"api_key\":\"key1\"},"
        "{\"name\":\"d2\",\"auth_token\":\"tok2\"}]}");
    /* the UI deleted d1 and added d3 in front: rows match by NAME, not
       by position, so d2 keeps ITS token and d3 stays empty */
    cJSON *in = cJSON_Parse(
        "{\"destinations\":["
        "{\"name\":\"d3\",\"auth_token\":\"\"},"
        "{\"name\":\"d2\",\"auth_token\":\"\",\"api_key\":\"new\"}]}");

    api_util_unredact(in, stored);

    const cJSON *arr = cJSON_GetObjectItem(in, "destinations");
    const cJSON *d3 = cJSON_GetArrayItem(arr, 0);
    const cJSON *d2 = cJSON_GetArrayItem(arr, 1);

    TEST_ASSERT_EQUAL_STRING("", cJSON_GetObjectItem(d3, "auth_token")->valuestring);
    TEST_ASSERT_EQUAL_STRING("tok2", cJSON_GetObjectItem(d2, "auth_token")->valuestring);
    TEST_ASSERT_EQUAL_STRING("new", cJSON_GetObjectItem(d2, "api_key")->valuestring);
    cJSON_Delete(in);
    cJSON_Delete(stored);
}

static void test_unredact_array_items_by_index(void)
{
    /* rows without a name fall back to positional matching */
    cJSON *stored = cJSON_Parse(
        "{\"peers\":[{\"preshared_key\":\"psk0\"},{\"preshared_key\":\"psk1\"}]}");
    cJSON *in = cJSON_Parse(
        "{\"peers\":[{\"preshared_key\":\"\"},{\"preshared_key\":\"\"},"
        "{\"preshared_key\":\"\"}]}");

    api_util_unredact(in, stored);

    const cJSON *arr = cJSON_GetObjectItem(in, "peers");

    TEST_ASSERT_EQUAL_STRING("psk0",
        cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 0), "preshared_key")->valuestring);
    TEST_ASSERT_EQUAL_STRING("psk1",
        cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 1), "preshared_key")->valuestring);
    TEST_ASSERT_EQUAL_STRING("",
        cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 2), "preshared_key")->valuestring);
    cJSON_Delete(in);
    cJSON_Delete(stored);
}

/* ---- settings route parsing ------------------------------------------------------ */

static void test_path_plain_name(void)
{
    char name[40];
    bool is_schema = true;

    TEST_ASSERT_EQUAL(ESP_OK, api_util_settings_path(
        "/api/settings/wifi_manager", name, sizeof(name), &is_schema));
    TEST_ASSERT_EQUAL_STRING("wifi_manager", name);
    TEST_ASSERT_FALSE(is_schema);
}

static void test_path_schema(void)
{
    char name[40];
    bool is_schema = false;

    TEST_ASSERT_EQUAL(ESP_OK, api_util_settings_path(
        "/api/settings/log_manager/schema", name, sizeof(name), &is_schema));
    TEST_ASSERT_EQUAL_STRING("log_manager", name);
    TEST_ASSERT_TRUE(is_schema);
}

static void test_path_rejects_bad(void)
{
    char name[40];
    bool is_schema;

    /* empty name */
    TEST_ASSERT_NOT_EQUAL(ESP_OK, api_util_settings_path(
        "/api/settings/", name, sizeof(name), &is_schema));
    /* extra segment */
    TEST_ASSERT_NOT_EQUAL(ESP_OK, api_util_settings_path(
        "/api/settings/a/b", name, sizeof(name), &is_schema));
    /* bare "/schema" (empty name + suffix) */
    TEST_ASSERT_NOT_EQUAL(ESP_OK, api_util_settings_path(
        "/api/settings//schema", name, sizeof(name), &is_schema));
    /* wrong prefix */
    TEST_ASSERT_NOT_EQUAL(ESP_OK, api_util_settings_path(
        "/api/wifi/status", name, sizeof(name), &is_schema));
    /* name overflow */
    char tiny[4];

    TEST_ASSERT_NOT_EQUAL(ESP_OK, api_util_settings_path(
        "/api/settings/wifi_manager", tiny, sizeof(tiny), &is_schema));
}

/* ---- level mapping ---------------------------------------------------------------- */

static void test_level_mapping(void)
{
    int lvl = -1;

    TEST_ASSERT_EQUAL(ESP_OK, api_util_level_from_str("debug", &lvl));
    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, lvl);
    TEST_ASSERT_EQUAL(ESP_OK, api_util_level_from_str("none", &lvl));
    TEST_ASSERT_EQUAL(ESP_LOG_NONE, lvl);
    TEST_ASSERT_EQUAL(ESP_OK, api_util_level_from_str("verbose", &lvl));
    TEST_ASSERT_EQUAL(ESP_LOG_VERBOSE, lvl);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, api_util_level_from_str("loud", &lvl));
    TEST_ASSERT_NOT_EQUAL(ESP_OK, api_util_level_from_str(NULL, &lvl));
}

void run_util_tests(void)
{
    RUN_TEST(test_redact_passwords);
    RUN_TEST(test_redact_wireguard_keys);
    RUN_TEST(test_unredact_keeps_stored_on_empty);
    RUN_TEST(test_unredact_no_stored_value);
    RUN_TEST(test_redact_array_items);
    RUN_TEST(test_unredact_array_items_by_name);
    RUN_TEST(test_unredact_array_items_by_index);
    RUN_TEST(test_path_plain_name);
    RUN_TEST(test_path_schema);
    RUN_TEST(test_path_rejects_bad);
    RUN_TEST(test_level_mapping);
}
