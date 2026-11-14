/**
 * @file test_main.c
 * @brief Host tests for ha_webhooks' pure rules (ha_webhooks_reg.c): the
 *        revision and its fingerprint, the registration object with and
 *        without a revision, the trust rule for a push's reply and the
 *        decision on a reply body (2026-11-14, settings in the push reply).
 *        Expected: 9 Tests 0 Failures 0 Ignored.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "ha_webhooks_private.h"

#define URL_A "http://192.168.1.10:8123/api/webhook/abc"
#define URL_B "https://ha.example.org/api/webhook/abc"

void setUp(void)
{
}

void tearDown(void)
{
}

static hw_config_t base_config(void)
{
    hw_config_t c;

    memset(&c, 0, sizeof(c));
    c.enabled = true;
    strlcpy(c.url, URL_A, sizeof(c.url));
    c.interval_s = 60;
    c.gzip = true;
    strlcpy(c.cert_set, "home", sizeof(c.cert_set));
    strlcpy(c.networks, "Home WiFi", sizeof(c.networks));
    return c;
}

static hw_reg_err_t parse(const char *json, const hw_config_t *cur,
                          bool need_rev, hw_config_t *out)
{
    cJSON *o = cJSON_Parse(json);

    TEST_ASSERT_NOT_NULL_MESSAGE(o, json);

    hw_reg_err_t e = hw_reg_parse(o, cur, need_rev, out);

    cJSON_Delete(o);
    return e;
}

static hw_reply_t decide(const char *body, const hw_config_t *cur,
                         bool trusted, hw_config_t *out, hw_reg_err_t *err)
{
    return hw_reply_decide(body, strlen(body), false, cur, trusted, out, err);
}

/* ---- the revision --------------------------------------------------------- */

static void test_rev_valid(void)
{
    TEST_ASSERT_TRUE(hw_rev_valid("9f8e7d6c"));
    TEST_ASSERT_TRUE(hw_rev_valid("A"));
    TEST_ASSERT_TRUE(hw_rev_valid("Az09_-"));
    TEST_ASSERT_TRUE(hw_rev_valid("0123456789abcdef"));   /* 16: the cap */
    TEST_ASSERT_FALSE(hw_rev_valid("0123456789abcdef0")); /* 17 */
    TEST_ASSERT_FALSE(hw_rev_valid(""));
    TEST_ASSERT_FALSE(hw_rev_valid(NULL));
    TEST_ASSERT_FALSE(hw_rev_valid("9f 8e"));
    TEST_ASSERT_FALSE(hw_rev_valid("9f.8e"));
    TEST_ASSERT_FALSE(hw_rev_valid("9f/8e"));
    TEST_ASSERT_FALSE(hw_rev_valid("9f\"8e"));
}

static void test_rev_follows_the_settings(void)
{
    hw_config_t c = base_config();

    /* none stored: nothing to report */
    TEST_ASSERT_EQUAL_STRING("", hw_rev_reported(&c));

    hw_rev_stamp(&c, "1a2b3c4d");
    TEST_ASSERT_EQUAL_STRING("1a2b3c4d", c.rev);
    TEST_ASSERT_EQUAL_size_t(8, strlen(c.rev_sum));
    TEST_ASSERT_EQUAL_STRING("1a2b3c4d", hw_rev_reported(&c));

    /* each of the four settings a revision names: changed by a route that
       knows nothing of revisions (the web interface, a restore), it is
       gone; put back, it is there again */
    hw_config_t d = c;

    d.interval_s = 20;
    TEST_ASSERT_EQUAL_STRING("", hw_rev_reported(&d));
    d = c;
    d.enabled = false;
    TEST_ASSERT_EQUAL_STRING("", hw_rev_reported(&d));
    d = c;
    strlcpy(d.url, URL_B, sizeof(d.url));
    TEST_ASSERT_EQUAL_STRING("", hw_rev_reported(&d));
    d = c;
    strlcpy(d.url2, URL_B, sizeof(d.url2));
    TEST_ASSERT_EQUAL_STRING("", hw_rev_reported(&d));
    d.url2[0] = '\0';
    TEST_ASSERT_EQUAL_STRING("1a2b3c4d", hw_rev_reported(&d));

    /* the user's own settings are not part of it */
    d = c;
    d.gzip = false;
    d.data_mode_full = true;
    d.networks[0] = '\0';
    d.cert_set[0] = '\0';
    TEST_ASSERT_EQUAL_STRING("1a2b3c4d", hw_rev_reported(&d));

    /* a revision with no fingerprint, or a broken one, reports nothing */
    d = c;
    d.rev_sum[0] = '\0';
    TEST_ASSERT_EQUAL_STRING("", hw_rev_reported(&d));
    d = c;
    strlcpy(d.rev, "not a rev", sizeof(d.rev));
    TEST_ASSERT_EQUAL_STRING("", hw_rev_reported(&d));

    /* stamping nothing clears both */
    hw_rev_stamp(&c, NULL);
    TEST_ASSERT_EQUAL_STRING("", c.rev);
    TEST_ASSERT_EQUAL_STRING("", c.rev_sum);
    hw_rev_stamp(&c, "bad rev");
    TEST_ASSERT_EQUAL_STRING("", c.rev);
}

static void test_rev_sum_keeps_the_fields_apart(void)
{
    hw_config_t a = base_config();
    hw_config_t b = base_config();
    char sa[HW_REV_SUM_LEN];
    char sb[HW_REV_SUM_LEN];

    strlcpy(a.url, "http://ab", sizeof(a.url));
    strlcpy(a.url2, "http://c", sizeof(a.url2));
    strlcpy(b.url, "http://abhttp://c", sizeof(b.url));
    b.url2[0] = '\0';
    hw_rev_sum(&a, sa);
    hw_rev_sum(&b, sb);
    TEST_ASSERT_TRUE(strcmp(sa, sb) != 0);

    /* and it is a function of the settings alone */
    hw_config_t a2 = a;

    a2.gzip = !a.gzip;
    hw_rev_sum(&a2, sb);
    TEST_ASSERT_EQUAL_STRING(sa, sb);
}

/* ---- the registration object ---------------------------------------------- */

static void test_registration_without_a_revision(void)
{
    hw_config_t cur = base_config();
    hw_config_t out;

    hw_rev_stamp(&cur, "1a2b3c4d");

    /* the body an older integration sends */
    TEST_ASSERT_EQUAL(HW_REG_OK, parse(
        "{\"url\":\"" URL_A "\",\"urls\":[\"" URL_A "\",\"" URL_B "\"],"
        "\"enabled\":true,\"interval\":15}", &cur, false, &out));
    TEST_ASSERT_EQUAL_STRING(URL_A, out.url);
    TEST_ASSERT_EQUAL_STRING(URL_B, out.url2);
    TEST_ASSERT_TRUE(out.enabled);
    TEST_ASSERT_EQUAL_UINT32(15, out.interval_s);
    /* no revision came with it: the device reports none from now on */
    TEST_ASSERT_EQUAL_STRING("", out.rev);
    TEST_ASSERT_EQUAL_STRING("", hw_rev_reported(&out));
    /* the user's settings ride along untouched */
    TEST_ASSERT_TRUE(out.gzip);
    TEST_ASSERT_EQUAL_STRING("home", out.cert_set);
    TEST_ASSERT_EQUAL_STRING("Home WiFi", out.networks);

    /* forgiving, as it always was: no interval, or one out of range, keeps
       the current one; enabled defaults to true; one address clears the
       second */
    TEST_ASSERT_EQUAL(HW_REG_OK, parse("{\"url\":\"" URL_B "\"}", &cur,
                                       false, &out));
    TEST_ASSERT_EQUAL_UINT32(60, out.interval_s);
    TEST_ASSERT_TRUE(out.enabled);
    TEST_ASSERT_EQUAL_STRING("", out.url2);
    TEST_ASSERT_EQUAL(HW_REG_OK, parse(
        "{\"url\":\"" URL_B "\",\"interval\":0,\"enabled\":false}", &cur,
        false, &out));
    TEST_ASSERT_EQUAL_UINT32(60, out.interval_s);
    TEST_ASSERT_FALSE(out.enabled);
    TEST_ASSERT_EQUAL(HW_REG_OK, parse(
        "{\"url\":\"" URL_B "\",\"interval\":3601}", &cur, false, &out));
    TEST_ASSERT_EQUAL_UINT32(60, out.interval_s);

    /* the refusals, with the texts the route has always answered */
    TEST_ASSERT_EQUAL(HW_REG_URL, parse("{\"enabled\":true}", &cur, false,
                                        &out));
    TEST_ASSERT_EQUAL(HW_REG_URL, parse("{\"url\":\"ftp://x/y\"}", &cur,
                                        false, &out));
    TEST_ASSERT_EQUAL(HW_REG_URL, parse("{\"url\":5}", &cur, false, &out));
    TEST_ASSERT_EQUAL(HW_REG_URLS_MANY, parse(
        "{\"url\":\"" URL_A "\",\"urls\":[\"" URL_A "\",\"" URL_B "\",\""
        URL_B "\"]}", &cur, false, &out));
    TEST_ASSERT_EQUAL(HW_REG_URLS_FIRST, parse(
        "{\"url\":\"" URL_A "\",\"urls\":[\"" URL_B "\",\"" URL_A "\"]}",
        &cur, false, &out));
    TEST_ASSERT_EQUAL(HW_REG_URLS_FIRST, parse(
        "{\"url\":\"" URL_A "\",\"urls\":[]}", &cur, false, &out));
    TEST_ASSERT_EQUAL(HW_REG_URLS_ENTRY, parse(
        "{\"url\":\"" URL_A "\",\"urls\":[\"" URL_A "\",\"mqtt://x\"]}",
        &cur, false, &out));
    TEST_ASSERT_EQUAL_STRING("url", hw_reg_err_str(HW_REG_URL));
    TEST_ASSERT_EQUAL_STRING("too many urls",
                             hw_reg_err_str(HW_REG_URLS_MANY));
    TEST_ASSERT_EQUAL_STRING("url must match urls[0]",
                             hw_reg_err_str(HW_REG_URLS_FIRST));
    TEST_ASSERT_EQUAL_STRING("url entry", hw_reg_err_str(HW_REG_URLS_ENTRY));
}

static void test_registration_with_a_revision(void)
{
    hw_config_t cur = base_config();
    hw_config_t out;
    char body[700];
    char longurl[HW_URL_LEN + 8];

    /* everything right: stored, and reported from then on */
    TEST_ASSERT_EQUAL(HW_REG_OK, parse(
        "{\"rev\":\"9f8e7d6c\",\"url\":\"" URL_A "\",\"urls\":[\"" URL_A
        "\",\"" URL_B "\"],\"enabled\":true,\"interval\":20,"
        "\"later\":\"key\"}", &cur, false, &out));
    TEST_ASSERT_EQUAL_STRING("9f8e7d6c", out.rev);
    TEST_ASSERT_EQUAL_STRING("9f8e7d6c", hw_rev_reported(&out));
    TEST_ASSERT_EQUAL_UINT32(20, out.interval_s);
    TEST_ASSERT_EQUAL_STRING(URL_B, out.url2);

    /* with a revision every field has to be right: a stored revision must
       stand for exactly what Home Assistant sent */
    TEST_ASSERT_EQUAL(HW_REG_INTERVAL, parse(
        "{\"rev\":\"r1\",\"url\":\"" URL_A "\"}", &cur, false, &out));
    TEST_ASSERT_EQUAL(HW_REG_INTERVAL, parse(
        "{\"rev\":\"r1\",\"url\":\"" URL_A "\",\"interval\":0}", &cur,
        false, &out));
    TEST_ASSERT_EQUAL(HW_REG_INTERVAL, parse(
        "{\"rev\":\"r1\",\"url\":\"" URL_A "\",\"interval\":3601}", &cur,
        false, &out));
    TEST_ASSERT_EQUAL(HW_REG_INTERVAL, parse(
        "{\"rev\":\"r1\",\"url\":\"" URL_A "\",\"interval\":20.5}", &cur,
        false, &out));
    TEST_ASSERT_EQUAL(HW_REG_INTERVAL, parse(
        "{\"rev\":\"r1\",\"url\":\"" URL_A "\",\"interval\":\"20\"}", &cur,
        false, &out));
    TEST_ASSERT_EQUAL(HW_REG_ENABLED, parse(
        "{\"rev\":\"r1\",\"url\":\"" URL_A "\",\"interval\":20,"
        "\"enabled\":\"yes\"}", &cur, false, &out));
    TEST_ASSERT_EQUAL(HW_REG_URLS_ENTRY, parse(
        "{\"rev\":\"r1\",\"url\":\"" URL_A "\",\"interval\":20,"
        "\"urls\":\"" URL_A "\"}", &cur, false, &out));
    TEST_ASSERT_EQUAL(HW_REG_URLS_ENTRY, parse(
        "{\"rev\":\"r1\",\"url\":\"" URL_A "\",\"interval\":20,"
        "\"urls\":[\"" URL_A "\",7]}", &cur, false, &out));
    TEST_ASSERT_EQUAL(HW_REG_REV, parse(
        "{\"rev\":\"\",\"url\":\"" URL_A "\",\"interval\":20}", &cur,
        false, &out));
    TEST_ASSERT_EQUAL(HW_REG_REV, parse(
        "{\"rev\":\"has space\",\"url\":\"" URL_A "\",\"interval\":20}",
        &cur, false, &out));
    TEST_ASSERT_EQUAL(HW_REG_REV, parse(
        "{\"rev\":\"0123456789abcdef0\",\"url\":\"" URL_A
        "\",\"interval\":20}", &cur, false, &out));
    TEST_ASSERT_EQUAL(HW_REG_REV, parse(
        "{\"rev\":12,\"url\":\"" URL_A "\",\"interval\":20}", &cur, false,
        &out));

    /* an address is not cut to fit: 191 characters pass, 192 do not */
    memset(longurl, 'a', sizeof(longurl));
    memcpy(longurl, "http://", 7);
    longurl[HW_URL_LEN - 1] = '\0';
    snprintf(body, sizeof(body),
             "{\"rev\":\"r1\",\"url\":\"%s\",\"interval\":20}", longurl);
    TEST_ASSERT_EQUAL(HW_REG_OK, parse(body, &cur, false, &out));
    TEST_ASSERT_EQUAL_STRING(longurl, out.url);
    longurl[HW_URL_LEN - 1] = 'a';
    longurl[HW_URL_LEN] = '\0';
    snprintf(body, sizeof(body),
             "{\"rev\":\"r1\",\"url\":\"%s\",\"interval\":20}", longurl);
    TEST_ASSERT_EQUAL(HW_REG_URL_LONG, parse(body, &cur, false, &out));
    snprintf(body, sizeof(body),
             "{\"rev\":\"r1\",\"url\":\"" URL_A "\",\"urls\":[\"" URL_A
             "\",\"%s\"],\"interval\":20}", longurl);
    TEST_ASSERT_EQUAL(HW_REG_URL_LONG, parse(body, &cur, false, &out));
    /* while a body without a revision still has it cut, as before */
    snprintf(body, sizeof(body), "{\"url\":\"%s\"}", longurl);
    TEST_ASSERT_EQUAL(HW_REG_OK, parse(body, &cur, false, &out));
    TEST_ASSERT_EQUAL_size_t(HW_URL_LEN - 1, strlen(out.url));

    /* a reply's object has to carry a revision */
    TEST_ASSERT_EQUAL(HW_REG_REV, parse(
        "{\"url\":\"" URL_A "\",\"interval\":20}", &cur, true, &out));
    TEST_ASSERT_EQUAL_STRING("rev", hw_reg_err_str(HW_REG_REV));
    TEST_ASSERT_EQUAL_STRING("interval", hw_reg_err_str(HW_REG_INTERVAL));
    TEST_ASSERT_EQUAL_STRING("enabled", hw_reg_err_str(HW_REG_ENABLED));
    TEST_ASSERT_EQUAL_STRING("url too long",
                             hw_reg_err_str(HW_REG_URL_LONG));
    TEST_ASSERT_EQUAL_STRING("", hw_reg_err_str(HW_REG_OK));
}

/* ---- the reply ------------------------------------------------------------ */

static void test_reply_trust_rule(void)
{
    /* https to a host name: the chain and the name were verified, wherever
       the device is */
    TEST_ASSERT_TRUE(hw_reply_trusted(true, false, false, true));
    TEST_ASSERT_TRUE(hw_reply_trusted(true, true, false, false));
    /* plain http, or https to an address by IP (the name not compared):
       only a private address, and not through a station network marked
       untrusted */
    TEST_ASSERT_TRUE(hw_reply_trusted(false, true, true, false));
    TEST_ASSERT_FALSE(hw_reply_trusted(false, true, true, true));
    /* a public address: across the internet, never */
    TEST_ASSERT_FALSE(hw_reply_trusted(false, true, false, false));
    /* the connection's ends could not be read: no */
    TEST_ASSERT_FALSE(hw_reply_trusted(false, false, true, false));
    TEST_ASSERT_FALSE(hw_reply_trusted(false, false, false, false));
}

#define REPLY_OK \
    "{\"webhook\":{\"rev\":\"9f8e7d6c\",\"url\":\"" URL_A "\",\"urls\":[\"" \
    URL_A "\",\"" URL_B "\"],\"enabled\":true,\"interval\":20}}"

static void test_reply_plain_delivered(void)
{
    hw_config_t cur = base_config();
    hw_config_t out;
    hw_reg_err_t err;

    /* an empty body, `{}`, or keys this firmware does not know: the push
       was delivered and that is all */
    TEST_ASSERT_EQUAL(HW_REPLY_NONE,
                      hw_reply_decide(NULL, 0, false, &cur, true, &out, &err));
    TEST_ASSERT_EQUAL(HW_REPLY_NONE,
                      hw_reply_decide("", 0, false, &cur, true, &out, &err));
    TEST_ASSERT_EQUAL(HW_REPLY_NONE, decide("{}", &cur, true, &out, &err));
    TEST_ASSERT_EQUAL(HW_REPLY_NONE,
                      decide("{\"commands\":[1,2]}", &cur, true, &out, &err));
    TEST_ASSERT_EQUAL_STRING("", hw_reply_str(HW_REPLY_NONE));
}

static void test_reply_applied(void)
{
    hw_config_t cur = base_config();
    hw_config_t out;
    hw_reg_err_t err;

    hw_rev_stamp(&cur, "1a2b3c4d");

    TEST_ASSERT_EQUAL(HW_REPLY_APPLY, decide(REPLY_OK, &cur, true, &out,
                                             &err));
    TEST_ASSERT_EQUAL(HW_REG_OK, err);
    TEST_ASSERT_EQUAL_STRING(URL_A, out.url);
    TEST_ASSERT_EQUAL_STRING(URL_B, out.url2);
    TEST_ASSERT_EQUAL_UINT32(20, out.interval_s);
    TEST_ASSERT_TRUE(out.enabled);
    TEST_ASSERT_EQUAL_STRING("9f8e7d6c", hw_rev_reported(&out));
    TEST_ASSERT_TRUE(out.gzip); /* the user's settings stay */
    TEST_ASSERT_EQUAL_STRING("applied", hw_reply_str(HW_REPLY_APPLY));

    /* the same reply once more: nothing to do, trusted or not */
    hw_config_t again;

    TEST_ASSERT_EQUAL(HW_REPLY_SAME, decide(REPLY_OK, &out, true, &again,
                                            &err));
    TEST_ASSERT_EQUAL(HW_REPLY_SAME, decide(REPLY_OK, &out, false, &again,
                                            &err));

    /* the same revision, but the settings were changed on the device
       since: Home Assistant's settings go in again */
    hw_config_t edited = out;

    edited.interval_s = 5;
    TEST_ASSERT_EQUAL(HW_REPLY_APPLY, decide(REPLY_OK, &edited, true, &again,
                                             &err));
    TEST_ASSERT_EQUAL_UINT32(20, again.interval_s);

    /* only `len` bytes are read: what follows in the buffer is not part of
       the body */
    char buf[400];

    snprintf(buf, sizeof(buf), "%s garbage after the body", REPLY_OK);
    TEST_ASSERT_EQUAL(HW_REPLY_APPLY,
                      hw_reply_decide(buf, strlen(REPLY_OK), false, &cur,
                                      true, &out, &err));

    /* unknown keys around and inside the object are ignored */
    TEST_ASSERT_EQUAL(HW_REPLY_APPLY, decide(
        "{\"later\":1,\"webhook\":{\"rev\":\"r2\",\"url\":\"" URL_B "\","
        "\"interval\":300,\"manual_override\":true,\"x\":[]}}", &cur, true,
        &out, &err));
    TEST_ASSERT_EQUAL_STRING(URL_B, out.url);
    TEST_ASSERT_EQUAL_STRING("", out.url2);
    TEST_ASSERT_FALSE(out.manual_override); /* a reply cannot pin */
}

/* ---- a 410: removed in Home Assistant (2026-11-15) ------------------------- */

#define GONE "{\"webhook\":{\"removed\":true}}"

static hw_reply_t gone(const char *body, const hw_config_t *cur, bool trusted)
{
    return hw_gone_decide(body, strlen(body), false, cur, trusted);
}

static void test_gone_clears_the_webhook(void)
{
    hw_config_t cur = base_config();

    strlcpy(cur.url2, URL_B, sizeof(cur.url2));
    hw_rev_stamp(&cur, "1a2b3c4d");

    TEST_ASSERT_EQUAL(HW_REPLY_REMOVED, gone(GONE, &cur, true));
    /* keys of a later integration beside the marker change nothing */
    TEST_ASSERT_EQUAL(HW_REPLY_REMOVED,
                      gone("{\"webhook\":{\"removed\":true,\"why\":\"x\"},"
                           "\"more\":1}", &cur, true));
    TEST_ASSERT_EQUAL_STRING("removed", hw_reply_str(HW_REPLY_REMOVED));

    /* what is left: no address, off, no revision, the user's own settings */
    hw_config_unregister(&cur);
    TEST_ASSERT_EQUAL_STRING("", cur.url);
    TEST_ASSERT_EQUAL_STRING("", cur.url2);
    TEST_ASSERT_FALSE(cur.enabled);
    TEST_ASSERT_EQUAL_STRING("", cur.rev);
    TEST_ASSERT_EQUAL_STRING("", hw_rev_reported(&cur));
    TEST_ASSERT_EQUAL(60, cur.interval_s);
    TEST_ASSERT_TRUE(cur.gzip);
    TEST_ASSERT_EQUAL_STRING("home", cur.cert_set);
    TEST_ASSERT_EQUAL_STRING("Home WiFi", cur.networks);
}

static void test_gone_left_alone(void)
{
    hw_config_t cur = base_config();
    char big[HW_REPLY_MAX + 64];

    /* some other server's 410: no body, a page, JSON without the marker,
       the marker not true, the marker somewhere else */
    TEST_ASSERT_EQUAL(HW_REPLY_NONE, gone("", &cur, true));
    TEST_ASSERT_EQUAL(HW_REPLY_NONE,
                      hw_gone_decide(NULL, 0, false, &cur, true));
    TEST_ASSERT_EQUAL(HW_REPLY_NONE, gone("<html>Gone</html>", &cur, true));
    TEST_ASSERT_EQUAL(HW_REPLY_NONE, gone("[true]", &cur, true));
    TEST_ASSERT_EQUAL(HW_REPLY_NONE, gone("{}", &cur, true));
    TEST_ASSERT_EQUAL(HW_REPLY_NONE, gone("{\"webhook\":{}}", &cur, true));
    TEST_ASSERT_EQUAL(HW_REPLY_NONE,
                      gone("{\"webhook\":{\"removed\":false}}", &cur, true));
    TEST_ASSERT_EQUAL(HW_REPLY_NONE,
                      gone("{\"webhook\":{\"removed\":1}}", &cur, true));
    TEST_ASSERT_EQUAL(HW_REPLY_NONE,
                      gone("{\"webhook\":\"removed\"}", &cur, true));
    TEST_ASSERT_EQUAL(HW_REPLY_NONE, gone("{\"removed\":true}", &cur, true));

    /* cut off by the read limit, or over it: not Home Assistant's */
    TEST_ASSERT_EQUAL(HW_REPLY_NONE,
                      hw_gone_decide(GONE, strlen(GONE), true, &cur, true));
    memset(big, ' ', sizeof(big));
    memcpy(big, GONE, strlen(GONE));
    TEST_ASSERT_EQUAL(HW_REPLY_NONE,
                      hw_gone_decide(big, sizeof(big), false, &cur, true));

    /* the marker from a server no registration would be taken from */
    TEST_ASSERT_EQUAL(HW_REPLY_UNTRUSTED, gone(GONE, &cur, false));

    /* the user pinned the webhook on the device: from nobody */
    cur.manual_override = true;
    TEST_ASSERT_EQUAL(HW_REPLY_PINNED, gone(GONE, &cur, true));
    TEST_ASSERT_EQUAL(HW_REPLY_PINNED, gone(GONE, &cur, false));
}

static void test_reply_refused(void)
{
    hw_config_t cur = base_config();
    hw_config_t out;
    hw_reg_err_t err;
    char big[HW_REPLY_MAX + 64];

    hw_rev_stamp(&cur, "1a2b3c4d");

    /* not from where a registration would be taken */
    TEST_ASSERT_EQUAL(HW_REPLY_UNTRUSTED, decide(REPLY_OK, &cur, false, &out,
                                                 &err));

    /* the user pinned the webhook on the device */
    hw_config_t pinned = cur;

    pinned.manual_override = true;
    TEST_ASSERT_EQUAL(HW_REPLY_PINNED, decide(REPLY_OK, &pinned, true, &out,
                                              &err));

    /* not a JSON object */
    TEST_ASSERT_EQUAL(HW_REPLY_NOT_JSON, decide("OK", &cur, true, &out,
                                                &err));
    TEST_ASSERT_EQUAL(HW_REPLY_NOT_JSON,
                      decide("<html>login</html>", &cur, true, &out, &err));
    TEST_ASSERT_EQUAL(HW_REPLY_NOT_JSON, decide("[1,2]", &cur, true, &out,
                                                &err));
    TEST_ASSERT_EQUAL(HW_REPLY_NOT_JSON,
                      decide("{\"webhook\":{\"rev\":", &cur, true, &out,
                             &err));

    /* too long: cut off by the read limit, or simply over it */
    TEST_ASSERT_EQUAL(HW_REPLY_TOO_LONG,
                      hw_reply_decide(REPLY_OK, strlen(REPLY_OK), true, &cur,
                                      true, &out, &err));
    memset(big, ' ', sizeof(big));
    memcpy(big, REPLY_OK, strlen(REPLY_OK));
    TEST_ASSERT_EQUAL(HW_REPLY_TOO_LONG,
                      hw_reply_decide(big, HW_REPLY_MAX + 1, false, &cur,
                                      true, &out, &err));
    TEST_ASSERT_EQUAL(HW_REPLY_APPLY,
                      hw_reply_decide(big, HW_REPLY_MAX, false, &cur, true,
                                      &out, &err));

    /* the whole object is discarded when one field is off */
    TEST_ASSERT_EQUAL(HW_REPLY_INVALID, decide(
        "{\"webhook\":{\"rev\":\"r2\",\"url\":\"" URL_A "\","
        "\"interval\":0}}", &cur, true, &out, &err));
    TEST_ASSERT_EQUAL(HW_REG_INTERVAL, err);
    TEST_ASSERT_EQUAL(HW_REPLY_INVALID, decide(
        "{\"webhook\":{\"url\":\"" URL_A "\",\"interval\":20}}", &cur, true,
        &out, &err));
    TEST_ASSERT_EQUAL(HW_REG_REV, err);
    TEST_ASSERT_EQUAL(HW_REPLY_INVALID, decide(
        "{\"webhook\":{\"rev\":\"r2\",\"url\":\"ftp://x\","
        "\"interval\":20}}", &cur, true, &out, &err));
    TEST_ASSERT_EQUAL(HW_REG_URL, err);
    TEST_ASSERT_EQUAL(HW_REPLY_INVALID,
                      decide("{\"webhook\":5}", &cur, true, &out, &err));

    TEST_ASSERT_EQUAL_STRING("untrusted", hw_reply_str(HW_REPLY_UNTRUSTED));
    TEST_ASSERT_EQUAL_STRING("pinned", hw_reply_str(HW_REPLY_PINNED));
    TEST_ASSERT_EQUAL_STRING("not_json", hw_reply_str(HW_REPLY_NOT_JSON));
    TEST_ASSERT_EQUAL_STRING("too_long", hw_reply_str(HW_REPLY_TOO_LONG));
    TEST_ASSERT_EQUAL_STRING("invalid", hw_reply_str(HW_REPLY_INVALID));
    TEST_ASSERT_EQUAL_STRING("failed", hw_reply_str(HW_REPLY_FAILED));
    TEST_ASSERT_EQUAL_STRING("same", hw_reply_str(HW_REPLY_SAME));
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_rev_valid);
    RUN_TEST(test_rev_follows_the_settings);
    RUN_TEST(test_rev_sum_keeps_the_fields_apart);
    RUN_TEST(test_registration_without_a_revision);
    RUN_TEST(test_registration_with_a_revision);
    RUN_TEST(test_reply_trust_rule);
    RUN_TEST(test_reply_plain_delivered);
    RUN_TEST(test_reply_applied);
    RUN_TEST(test_reply_refused);
    RUN_TEST(test_gone_clears_the_webhook);
    RUN_TEST(test_gone_left_alone);
    UNITY_END();
}
