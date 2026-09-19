/**
 * @file test_main.c
 * @brief Host suite for data_destinations' pure core: settings-item
 *        parse/normalize (defaults, schemes, rejections), the scheduler
 *        with backoff (fake µs clock), percent-encoding, URL composition,
 *        `~/` topic expansion and the ABRP mapping / form / response
 *        rules.
 */
#include <string.h>

#include "unity.h"

#include "data_destinations_private.h"

static dd_dest_t s_d;
static char s_err[DD_ERR_LEN];

static esp_err_t parse(const char *json)
{
    cJSON *o = cJSON_Parse(json);

    s_err[0] = '\0';

    esp_err_t rc = dd_parse_dest(o, &s_d, s_err, sizeof(s_err));

    cJSON_Delete(o);
    return rc;
}

/* ---- parse ------------------------------------------------------------------ */

static void test_parse_defaults_mqtt(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, parse("{\"name\":\"d1\"}"));
    TEST_ASSERT_EQUAL(DD_TYPE_MQTT, s_d.type);
    TEST_ASSERT_TRUE(s_d.enabled);
    TEST_ASSERT_TRUE(s_d.retain);
    TEST_ASSERT_TRUE(s_d.full_first);
    TEST_ASSERT_EQUAL_UINT32(5, s_d.period_s);
    TEST_ASSERT_EQUAL_STRING("~/autopid", s_d.url);
    TEST_ASSERT_EQUAL(DD_AUTH_NONE, s_d.auth);
}

static void test_parse_http_scheme_prepended(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, parse("{\"name\":\"h\",\"type\":\"http\","
                                    "\"url\":\"10.42.1.1:8199/p\"}"));
    TEST_ASSERT_EQUAL_STRING("http://10.42.1.1:8199/p", s_d.url);
    TEST_ASSERT_EQUAL(ESP_OK, parse("{\"name\":\"s\",\"type\":\"https\","
                                    "\"url\":\"host/x\"}"));
    TEST_ASSERT_EQUAL_STRING("https://host/x", s_d.url);
    /* an explicit scheme is kept as-is */
    TEST_ASSERT_EQUAL(ESP_OK, parse("{\"name\":\"h2\",\"type\":\"http\","
                                    "\"url\":\"https://x/y\"}"));
    TEST_ASSERT_EQUAL_STRING("https://x/y", s_d.url);
}

static void test_parse_rejections(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse("{\"type\":\"http\"}"));
    TEST_ASSERT_NOT_NULL(strstr(s_err, "name"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      parse("{\"name\":\"h\",\"type\":\"http\"}"));
    TEST_ASSERT_EQUAL_STRING("h: url required", s_err);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      parse("{\"name\":\"s\",\"type\":\"https\","
                            "\"url\":\"http://x\"}"));
    TEST_ASSERT_NOT_NULL(strstr(s_err, "http://"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      parse("{\"name\":\"a\",\"type\":\"abrp\"}"));
    TEST_ASSERT_NOT_NULL(strstr(s_err, "token"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      parse("{\"name\":\"h\",\"type\":\"http\",\"url\":"
                            "\"http://x\",\"auth\":\"bearer\"}"));
    TEST_ASSERT_NOT_NULL(strstr(s_err, "auth_token"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      parse("{\"name\":\"h\",\"type\":\"http\",\"url\":"
                            "\"http://x\",\"auth\":\"basic\"}"));
    TEST_ASSERT_NOT_NULL(strstr(s_err, "basic_username"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      parse("{\"name\":\"z\",\"type\":\"ftp\"}"));
    /* a DISABLED half-filled entry is fine */
    TEST_ASSERT_EQUAL(ESP_OK, parse("{\"name\":\"a\",\"type\":\"abrp\","
                                    "\"enabled\":false}"));
}

static void test_parse_abrp_defaults(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, parse("{\"name\":\"a\",\"type\":\"abrp\","
                                    "\"auth_token\":\"tok\"}"));
    TEST_ASSERT_EQUAL_STRING(DD_ABRP_DEFAULT_URL, s_d.url);
    TEST_ASSERT_EQUAL(ESP_OK, parse("{\"name\":\"a\",\"type\":\"abrp\","
                                    "\"auth_token\":\"tok\",\"auth\":"
                                    "\"api_key_header\"}"));
    TEST_ASSERT_EQUAL_STRING("Authorization", s_d.auth_name);
    TEST_ASSERT_EQUAL(ESP_OK, parse("{\"name\":\"h\",\"type\":\"http\","
                                    "\"url\":\"http://x\",\"auth\":"
                                    "\"api_key_header\",\"auth_token\":\"k\"}"));
    TEST_ASSERT_EQUAL_STRING("x-api-key", s_d.auth_name);
    TEST_ASSERT_EQUAL(ESP_OK, parse("{\"name\":\"h\",\"type\":\"http\","
                                    "\"url\":\"http://x\",\"auth\":"
                                    "\"api_key_query\",\"auth_token\":\"k\"}"));
    TEST_ASSERT_EQUAL_STRING("api_key", s_d.auth_name);
}

static void test_config_parse_duplicates_and_cap(void)
{
    static dd_config_t cfg;
    cJSON *o = cJSON_Parse("{\"enabled\":false,\"destinations\":["
                           "{\"name\":\"a\"},{\"name\":\"a\"}]}");

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      dd_config_parse(o, &cfg, s_err, sizeof(s_err)));
    TEST_ASSERT_EQUAL_STRING("a: duplicate name", s_err);
    cJSON_Delete(o);

    o = cJSON_Parse("{\"destinations\":[{\"name\":\"a\"},{\"name\":\"b\","
                    "\"type\":\"http\",\"url\":\"h/x\"}]}");
    TEST_ASSERT_EQUAL(ESP_OK, dd_config_parse(o, &cfg, s_err, sizeof(s_err)));
    TEST_ASSERT_TRUE(cfg.enabled);
    TEST_ASSERT_EQUAL(2, cfg.n);
    TEST_ASSERT_EQUAL_STRING("http://h/x", cfg.dest[1].url);
    cJSON_Delete(o);

    o = cJSON_Parse("{}");
    TEST_ASSERT_EQUAL(ESP_OK, dd_config_parse(o, &cfg, s_err, sizeof(s_err)));
    TEST_ASSERT_EQUAL(0, cfg.n);
    cJSON_Delete(o);
}

/* ---- scheduler ----------------------------------------------------------------- */

static void test_sched_success_keeps_period(void)
{
    dd_state_t st = { 0 };
    int64_t now = 1000000;

    TEST_ASSERT_TRUE(dd_sched_due(&st, now)); /* fresh = due now */
    dd_sched_after(&st, 5, true, now);
    TEST_ASSERT_EQUAL_UINT32(1, st.success);
    TEST_ASSERT_FALSE(dd_sched_due(&st, now + 4999999));
    TEST_ASSERT_TRUE(dd_sched_due(&st, now + 5000000));
    TEST_ASSERT_EQUAL_UINT32(0, st.backoff_ms);
}

static void test_sched_backoff_after_three(void)
{
    dd_state_t st = { 0 };
    int64_t now = 0;

    dd_sched_after(&st, 5, false, now);
    dd_sched_after(&st, 5, false, now);
    TEST_ASSERT_EQUAL_UINT32(0, st.backoff_ms);        /* 2 failures */
    TEST_ASSERT_EQUAL_INT64(5000000, st.next_due_us);
    dd_sched_after(&st, 5, false, now);                /* 3rd -> 10 s */
    TEST_ASSERT_EQUAL_UINT32(10000, st.backoff_ms);
    TEST_ASSERT_EQUAL_INT64(10000000, st.next_due_us);
    dd_sched_after(&st, 5, false, now);                /* 20 s */
    TEST_ASSERT_EQUAL_UINT32(20000, st.backoff_ms);
    dd_sched_after(&st, 5, false, now);                /* 40 s */
    dd_sched_after(&st, 5, false, now);                /* cap 60 s */
    dd_sched_after(&st, 5, false, now);
    TEST_ASSERT_EQUAL_UINT32(60000, st.backoff_ms);
    TEST_ASSERT_EQUAL_UINT32(7, st.fail);
    TEST_ASSERT_EQUAL_UINT32(7, st.consec_failures);
    /* one success clears it all */
    dd_sched_after(&st, 5, true, now);
    TEST_ASSERT_EQUAL_UINT32(0, st.backoff_ms);
    TEST_ASSERT_EQUAL_UINT32(0, st.consec_failures);
    TEST_ASSERT_EQUAL_INT64(5000000, st.next_due_us);
}

static void test_backoff_caps(void)
{
    TEST_ASSERT_EQUAL_UINT32(10000, dd_backoff_next(0, 1000));
    TEST_ASSERT_EQUAL_UINT32(60000, dd_backoff_next(40000, 1000));
    /* a long period: cap = 8 x period, never above 10 min */
    TEST_ASSERT_EQUAL_UINT32(240000, dd_backoff_next(0, 120000));
    TEST_ASSERT_EQUAL_UINT32(480000, dd_backoff_next(240000, 120000));
    TEST_ASSERT_EQUAL_UINT32(600000, dd_backoff_next(480000, 120000));
}

static void test_sched_skip_counts_no_backoff(void)
{
    dd_state_t st = { 0 };

    dd_sched_skip(&st, 5, 0);
    dd_sched_skip(&st, 5, 5000000);
    TEST_ASSERT_EQUAL_UINT32(2, st.skipped_offline);
    TEST_ASSERT_EQUAL_UINT32(0, st.fail);
    TEST_ASSERT_EQUAL_UINT32(0, st.backoff_ms);
    TEST_ASSERT_EQUAL_INT64(10000000, st.next_due_us);
}

/* ---- url helpers --------------------------------------------------------------- */

static void test_url_encode(void)
{
    char out[64];

    TEST_ASSERT_TRUE(dd_url_encode("a b&c=d/é~", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("a%20b%26c%3Dd%2F%C3%A9~", out);
    TEST_ASSERT_TRUE(dd_url_encode("", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_FALSE(dd_url_encode("abcdef", out, 4));
}

static void test_url_compose(void)
{
    char out[256];

    TEST_ASSERT_TRUE(dd_url_compose("http://h/p", "", NULL, NULL, out,
                                    sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("http://h/p", out);
    TEST_ASSERT_TRUE(dd_url_compose("http://h/p", "a=1&b=2", "api_key",
                                    "k y", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("http://h/p?a=1&b=2&api_key=k%20y", out);
    TEST_ASSERT_TRUE(dd_url_compose("http://h/p?x=1", "?a=1", NULL, NULL,
                                    out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("http://h/p?x=1&a=1", out);
    TEST_ASSERT_TRUE(dd_url_compose("http://h/p?", NULL, "k", "v", out,
                                    sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("http://h/p?k=v", out);
    TEST_ASSERT_FALSE(dd_url_compose("http://h/p", "a=1", NULL, NULL, out,
                                     12));
}

static void test_url_host_is_ip(void)
{
    TEST_ASSERT_TRUE(dd_url_host_is_ip("https://10.42.1.1:8443/x"));
    TEST_ASSERT_TRUE(dd_url_host_is_ip("http://192.168.0.1"));
    TEST_ASSERT_FALSE(dd_url_host_is_ip("https://api.iternio.com/1"));
    TEST_ASSERT_FALSE(dd_url_host_is_ip("https://"));
    TEST_ASSERT_FALSE(dd_url_host_is_ip(NULL));
}

static void test_topic_expand(void)
{
    char out[64];

    TEST_ASSERT_TRUE(dd_topic_expand("~/autopid", "wican/abc", out,
                                     sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("wican/abc/autopid", out);
    TEST_ASSERT_TRUE(dd_topic_expand("car/data", "wican/abc", out,
                                     sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("car/data", out);
    TEST_ASSERT_FALSE(dd_topic_expand("~/autopid", "wican/abc", out, 8));
}

/* ---- ABRP ------------------------------------------------------------------------ */

static void test_abrp_tlm_mapping(void)
{
    cJSON *snap = cJSON_Parse("{\"SOC\":71.5,\"HV_W\":-12.3,\"CHARGING\":"
                              "\"on\",\"PARK_BRAKE\":true,\"HV_V\":\"398.2\","
                              "\"gps_latitude\":-37.9,\"gps_longitude\":"
                              "145.1,\"gps_altitude\":42,\"gps_speed\":0,"
                              "\"SPEED\":33,\"RPM\":800}");
    cJSON *tlm = dd_abrp_tlm(snap, "hyundai:ioniq5:22:77", 1700000000);
    char *s = cJSON_PrintUnformatted(tlm);

    TEST_ASSERT_EQUAL_DOUBLE(71.5, cJSON_GetObjectItem(tlm, "soc")->valuedouble);
    TEST_ASSERT_EQUAL_DOUBLE(-12.3,
                             cJSON_GetObjectItem(tlm, "power")->valuedouble);
    TEST_ASSERT_EQUAL_DOUBLE(1, cJSON_GetObjectItem(tlm, "is_charging")
                                    ->valuedouble);
    TEST_ASSERT_EQUAL_DOUBLE(1, cJSON_GetObjectItem(tlm, "is_parked")
                                    ->valuedouble);
    TEST_ASSERT_EQUAL_DOUBLE(398.2,
                             cJSON_GetObjectItem(tlm, "voltage")->valuedouble);
    TEST_ASSERT_EQUAL_DOUBLE(-37.9, cJSON_GetObjectItem(tlm, "lat")->valuedouble);
    TEST_ASSERT_EQUAL_DOUBLE(145.1, cJSON_GetObjectItem(tlm, "lon")->valuedouble);
    TEST_ASSERT_EQUAL_DOUBLE(42, cJSON_GetObjectItem(tlm, "elevation")
                                     ->valuedouble);
    /* SPEED (vehicle) wins over the GPS speed */
    TEST_ASSERT_EQUAL_DOUBLE(33, cJSON_GetObjectItem(tlm, "speed")->valuedouble);
    TEST_ASSERT_EQUAL_DOUBLE(1700000000,
                             cJSON_GetObjectItem(tlm, "utc")->valuedouble);
    TEST_ASSERT_EQUAL_STRING("hyundai:ioniq5:22:77",
                             cJSON_GetObjectItem(tlm, "car_model")->valuestring);
    TEST_ASSERT_NULL(cJSON_GetObjectItem(tlm, "RPM")); /* unmapped dropped */
    TEST_ASSERT_NULL(cJSON_GetObjectItem(tlm, "rpm"));
    cJSON_free(s);
    cJSON_Delete(tlm);
    cJSON_Delete(snap);

    /* lowercase profile names map too (case-insensitive lookup); a
       snapshot utc is kept; no car_model when empty */
    snap = cJSON_Parse("{\"soc\":50,\"utc\":123}");
    tlm = dd_abrp_tlm(snap, "", 999);
    TEST_ASSERT_EQUAL_DOUBLE(50, cJSON_GetObjectItem(tlm, "soc")->valuedouble);
    TEST_ASSERT_EQUAL_DOUBLE(123, cJSON_GetObjectItem(tlm, "utc")->valuedouble);
    TEST_ASSERT_NULL(cJSON_GetObjectItem(tlm, "car_model"));
    cJSON_Delete(tlm);
    cJSON_Delete(snap);
}

static void test_abrp_form_and_header(void)
{
    char out[256];

    TEST_ASSERT_TRUE(dd_abrp_form("t k", "{\"soc\":5}", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("token=t%20k&tlm=%7B%22soc%22%3A5%7D", out);
    TEST_ASSERT_FALSE(dd_abrp_form("token", "{\"soc\":5}", out, 20));
    TEST_ASSERT_TRUE(dd_abrp_auth_value("abc", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("APIKEY abc", out);
    TEST_ASSERT_TRUE(dd_abrp_auth_value("apikey xyz", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("apikey xyz", out);
    TEST_ASSERT_FALSE(dd_abrp_auth_value("", out, sizeof(out)));
}

static void test_abrp_response(void)
{
    char err[DD_ERR_LEN] = "";

    TEST_ASSERT_TRUE(dd_abrp_response_ok("{\"status\":\"ok\"}", true, err,
                                         sizeof(err)));
    TEST_ASSERT_FALSE(dd_abrp_response_ok("{\"status\":\"error\",\"missing\":"
                                          "[\"utc\"]}", true, err,
                                          sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("abrp: error [\"utc\"]", err);
    /* not JSON / empty: the HTTP outcome decides */
    TEST_ASSERT_TRUE(dd_abrp_response_ok("OK", true, err, sizeof(err)));
    TEST_ASSERT_FALSE(dd_abrp_response_ok("", false, err, sizeof(err)));
    TEST_ASSERT_TRUE(dd_abrp_response_ok(NULL, true, err, sizeof(err)));
    /* a JSON body without status keeps the HTTP verdict */
    TEST_ASSERT_FALSE(dd_abrp_response_ok("{\"x\":1}", false, err,
                                          sizeof(err)));
}

static void test_enum_strings(void)
{
    dd_type_t t;
    dd_auth_t a;

    TEST_ASSERT_TRUE(dd_type_parse("abrp", &t));
    TEST_ASSERT_EQUAL(DD_TYPE_ABRP, t);
    TEST_ASSERT_FALSE(dd_type_parse("ABRP", &t));
    TEST_ASSERT_EQUAL_STRING("https", dd_type_str(DD_TYPE_HTTPS));
    TEST_ASSERT_TRUE(dd_auth_parse("basic", &a));
    TEST_ASSERT_EQUAL_STRING("api_key_query",
                             dd_auth_str(DD_AUTH_API_KEY_QUERY));
}

void setUp(void)
{
}

void tearDown(void)
{
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_defaults_mqtt);
    RUN_TEST(test_parse_http_scheme_prepended);
    RUN_TEST(test_parse_rejections);
    RUN_TEST(test_parse_abrp_defaults);
    RUN_TEST(test_config_parse_duplicates_and_cap);
    RUN_TEST(test_sched_success_keeps_period);
    RUN_TEST(test_sched_backoff_after_three);
    RUN_TEST(test_backoff_caps);
    RUN_TEST(test_sched_skip_counts_no_backoff);
    RUN_TEST(test_url_encode);
    RUN_TEST(test_url_compose);
    RUN_TEST(test_url_host_is_ip);
    RUN_TEST(test_topic_expand);
    RUN_TEST(test_abrp_tlm_mapping);
    RUN_TEST(test_abrp_form_and_header);
    RUN_TEST(test_abrp_response);
    RUN_TEST(test_enum_strings);
    UNITY_END();
}
