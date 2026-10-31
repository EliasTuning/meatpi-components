/**
 * @file test_main.c
 * @brief Host tests for vpn_manager's pure config checks: WireGuard
 *        key shape, endpoint sanity, the enabled-config ladder, and
 *        the tailscale validation branch.
 *        Since 2026-10-11 also the routes list (issue #975) and the v1
 *        allowed_ip + mask conversion (since 2026-10-16 the network the
 *        tunnel address names, issue #988), and the state words every
 *        surface shares (issue #985). Expected output: 11 Tests 0
 *        Failures 0 Ignored.
 */
#include <string.h>

#include "unity.h"

#include "vpn_manager_private.h"

/* a shape-valid (not cryptographically meaningful) 44-char b64 key */
#define GOOD_KEY "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOP8="

static vpn_config_t s_cfg;

void setUp(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.enabled = true;
    strcpy(s_cfg.private_key, GOOD_KEY);
    strcpy(s_cfg.peer_public_key, GOOD_KEY);
    strcpy(s_cfg.address, "10.6.0.2");
    strcpy(s_cfg.routes, "10.6.0.0/24");
    strcpy(s_cfg.endpoint, "vpn.example.com");
    s_cfg.port = 51820;
    s_cfg.keepalive_s = 25;
}

void tearDown(void)
{
}

static void test_key_shape(void)
{
    TEST_ASSERT_TRUE(vpn_check_wg_key(GOOD_KEY));
    TEST_ASSERT_FALSE(vpn_check_wg_key(NULL));
    TEST_ASSERT_FALSE(vpn_check_wg_key(""));
    TEST_ASSERT_FALSE(vpn_check_wg_key("too-short="));
    /* right length, missing '=' terminator */
    TEST_ASSERT_FALSE(vpn_check_wg_key(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOP88"));
    /* illegal char in the body */
    TEST_ASSERT_FALSE(vpn_check_wg_key(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMN!P8="));
}

static void test_endpoint_shape(void)
{
    TEST_ASSERT_TRUE(vpn_check_endpoint("vpn.example.com"));
    TEST_ASSERT_TRUE(vpn_check_endpoint("192.168.1.10"));
    TEST_ASSERT_FALSE(vpn_check_endpoint(""));
    TEST_ASSERT_FALSE(vpn_check_endpoint(NULL));
    TEST_ASSERT_FALSE(vpn_check_endpoint("bad host"));
    TEST_ASSERT_FALSE(vpn_check_endpoint("host;rm"));
}

static void test_disabled_config_always_ok(void)
{
    vpn_config_t cfg;

    memset(&cfg, 0, sizeof(cfg)); /* empty AND disabled */
    TEST_ASSERT_NULL(vpn_check_config(&cfg));
}

static void test_enabled_config_valid(void)
{
    TEST_ASSERT_NULL(vpn_check_config(&s_cfg));
    /* preshared key is optional... */
    s_cfg.preshared_key[0] = '\0';
    TEST_ASSERT_NULL(vpn_check_config(&s_cfg));
    /* ...but must be well-formed when present */
    strcpy(s_cfg.preshared_key, "nonsense");
    TEST_ASSERT_NOT_NULL(vpn_check_config(&s_cfg));
}

static void test_enabled_config_requires_keys(void)
{
    s_cfg.private_key[0] = '\0';
    TEST_ASSERT_NOT_NULL(vpn_check_config(&s_cfg));
    setUp();
    s_cfg.peer_public_key[0] = '\0';
    TEST_ASSERT_NOT_NULL(vpn_check_config(&s_cfg));
}

static void test_enabled_config_requires_endpoint_and_address(void)
{
    s_cfg.endpoint[0] = '\0';
    TEST_ASSERT_NOT_NULL(vpn_check_config(&s_cfg));
    setUp();
    s_cfg.address[0] = '\0';
    TEST_ASSERT_NOT_NULL(vpn_check_config(&s_cfg));
    setUp();
    s_cfg.port = 0;
    TEST_ASSERT_NOT_NULL(vpn_check_config(&s_cfg));
}

static void ts_setup(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.enabled = true;
    s_cfg.tailscale = true;
    strcpy(s_cfg.ts_auth_key,
           "tskey-auth-k0123456789abcdef0123456789abcdef");
}

static void test_tailscale_valid_configs(void)
{
    /* official coordinator: auth key alone is enough, no WG fields */
    ts_setup();
    TEST_ASSERT_NULL(vpn_check_config(&s_cfg));

    /* headscale hex preauth key */
    ts_setup();
    strcpy(s_cfg.ts_auth_key,
           "1f34a1e2b3c4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6");
    TEST_ASSERT_NULL(vpn_check_config(&s_cfg));

    /* custom coordinator: bare host, host:port, bare IP:port */
    ts_setup();
    strcpy(s_cfg.ts_control_url, "headscale.example.com");
    TEST_ASSERT_NULL(vpn_check_config(&s_cfg));
    strcpy(s_cfg.ts_control_url, "10.42.0.1:8080");
    TEST_ASSERT_NULL(vpn_check_config(&s_cfg));

    /* device name is free-form/optional */
    ts_setup();
    strcpy(s_cfg.ts_device_name, "wican-garage");
    TEST_ASSERT_NULL(vpn_check_config(&s_cfg));
}

static void test_tailscale_rejects(void)
{
    /* auth key required (shape check: min length) */
    ts_setup();
    s_cfg.ts_auth_key[0] = '\0';
    TEST_ASSERT_NOT_NULL(vpn_check_config(&s_cfg));
    ts_setup();
    strcpy(s_cfg.ts_auth_key, "short");
    TEST_ASSERT_NOT_NULL(vpn_check_config(&s_cfg));

    /* control_url must be a clean host[:port]: no scheme, no spaces */
    ts_setup();
    strcpy(s_cfg.ts_control_url, "http://10.42.0.1:8080");
    TEST_ASSERT_NOT_NULL(vpn_check_config(&s_cfg));
    ts_setup();
    strcpy(s_cfg.ts_control_url, "bad host");
    TEST_ASSERT_NOT_NULL(vpn_check_config(&s_cfg));

    /* type=tailscale must NOT demand WG fields, and a disabled
     * tailscale config is always storable */
    ts_setup();
    s_cfg.enabled = false;
    s_cfg.ts_auth_key[0] = '\0';
    TEST_ASSERT_NULL(vpn_check_config(&s_cfg));
}

static void test_routes_list_rules(void)
{
    char err[96];

    TEST_ASSERT_TRUE(vpn_check_routes("", err, sizeof(err)));
    TEST_ASSERT_TRUE(vpn_check_routes("10.66.0.0/24, 192.168.1.0/24", err,
                                      sizeof(err)));
    TEST_ASSERT_FALSE(vpn_check_routes("10.66.0.0/24, nonsense", err,
                                       sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "nonsense"));
    /* nine networks: one too many */
    TEST_ASSERT_FALSE(vpn_check_routes(
        "10.1.0.0/16 10.2.0.0/16 10.3.0.0/16 10.4.0.0/16 10.5.0.0/16 "
        "10.6.0.0/16 10.7.0.0/16 10.8.0.0/16 10.9.0.0/16", err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "more than 8"));
    /* the list does not make or break an enabled config */
    s_cfg.routes[0] = '\0';
    TEST_ASSERT_NULL(vpn_check_config(&s_cfg));
}

static void test_legacy_allowed_ip_converts(void)
{
    char out[24];

    /* AllowedIPs = the tunnel subnet: the same network either way */
    TEST_ASSERT_TRUE(vpn_check_legacy_route("10.66.0.2", "10.66.0.0",
                                            "255.255.255.0", out,
                                            sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("10.66.0.0/24", out);
    /* issue #988: AllowedIPs names another network. v1 routed the tunnel
       address under the mask (the netif's own network), never the
       AllowedIPs network: the migration keeps what the device did */
    TEST_ASSERT_TRUE(vpn_check_legacy_route("10.6.0.2", "192.168.86.0",
                                            "255.255.255.0", out,
                                            sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("10.6.0.0/24", out);
    /* the address may carry its own /cidr: stripped, the host bits kept */
    TEST_ASSERT_TRUE(vpn_check_legacy_route("10.6.0.2/8", "192.168.1.5",
                                            "255.255.0.0", out,
                                            sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("10.6.0.0/16", out);
    /* no usable address: the pair's own network, as before */
    TEST_ASSERT_TRUE(vpn_check_legacy_route(NULL, "192.168.1.5",
                                            "255.255.0.0", out,
                                            sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("192.168.0.0/16", out);
    TEST_ASSERT_TRUE(vpn_check_legacy_route("", "10.66.0.0",
                                            "255.255.255.0", out,
                                            sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("10.66.0.0/24", out);
    TEST_ASSERT_TRUE(vpn_check_legacy_route("junk", "10.66.0.0",
                                            "255.255.255.0", out,
                                            sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("10.66.0.0/24", out);
    /* the 0.0.0.0 idiom (nothing but default_route), a mask with holes
       and garbage are dropped */
    TEST_ASSERT_FALSE(vpn_check_legacy_route("10.6.0.2", "0.0.0.0",
                                             "0.0.0.0", out, sizeof(out)));
    TEST_ASSERT_FALSE(vpn_check_legacy_route("10.6.0.2", "10.66.0.0",
                                             "0.0.0.0", out, sizeof(out)));
    TEST_ASSERT_FALSE(vpn_check_legacy_route("10.6.0.2", "10.66.0.0",
                                             "255.0.255.0", out,
                                             sizeof(out)));
    TEST_ASSERT_FALSE(vpn_check_legacy_route("10.6.0.2", "nonsense",
                                             "255.255.255.0", out,
                                             sizeof(out)));
    TEST_ASSERT_FALSE(vpn_check_legacy_route("10.6.0.2", NULL,
                                             "255.255.255.0", out,
                                             sizeof(out)));
}

static void test_state_words_cover_every_state(void)
{
    /* issue #985: the HA push indexed a 4-entry table with the 5-value
       enum; the words live in one place now and every state has one */
    TEST_ASSERT_EQUAL_STRING("disabled",
                             vpn_manager_state_str(VPN_STATE_DISABLED));
    TEST_ASSERT_EQUAL_STRING("waiting",
                             vpn_manager_state_str(VPN_STATE_WAITING));
    TEST_ASSERT_EQUAL_STRING("paused", vpn_manager_state_str(VPN_STATE_PAUSED));
    TEST_ASSERT_EQUAL_STRING("connecting",
                             vpn_manager_state_str(VPN_STATE_CONNECTING));
    TEST_ASSERT_EQUAL_STRING("connected",
                             vpn_manager_state_str(VPN_STATE_CONNECTED));

    for (int a = 0; a <= (int)VPN_STATE_CONNECTED; a++)
    {
        for (int b = a + 1; b <= (int)VPN_STATE_CONNECTED; b++)
        {
            TEST_ASSERT_NOT_EQUAL(0, strcmp(vpn_manager_state_str(a),
                                            vpn_manager_state_str(b)));
        }
    }

    TEST_ASSERT_EQUAL_STRING("disabled", vpn_manager_state_str(99));
}

static void test_ts_hold_reason_names_the_levers(void)
{
    char out[176];

    /* nothing to change but the type */
    TEST_ASSERT_EQUAL_STRING(
        "not started: 24 KB of internal RAM free, Tailscale needs 30 KB: use WireGuard",
        vpn_ts_hold_reason(24, 30, false, false, out, sizeof(out)));

    /* Bluetooth on: named first, WireGuard as the alternative (the number
       is the measured cost on the flash-only controller build, 2026-10-31:
       31.5 KB on Lean, 34.6 on Full) */
    TEST_ASSERT_EQUAL_STRING(
        "not started: 24 KB of internal RAM free, Tailscale needs 30 KB: "
        "turn Bluetooth off (about 32 KB), or use WireGuard",
        vpn_ts_hold_reason(24, 30, true, false, out, sizeof(out)));

    /* both levers */
    TEST_ASSERT_EQUAL_STRING(
        "not started: 9 KB of internal RAM free, Tailscale needs 30 KB: "
        "turn Bluetooth off (about 32 KB), set the WiFi memory profile to Lean "
        "(about 14 KB), or use WireGuard",
        vpn_ts_hold_reason(9, 30, true, true, out, sizeof(out)));

    /* a short buffer truncates cleanly */
    char tiny[40];

    TEST_ASSERT_EQUAL_STRING("not started: 9 KB of internal RAM free,",
                             vpn_ts_hold_reason(9, 30, true, true, tiny, sizeof(tiny)));
    TEST_ASSERT_EQUAL_STRING("", vpn_ts_hold_reason(9, 30, true, true, NULL, 0));
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_key_shape);
    RUN_TEST(test_endpoint_shape);
    RUN_TEST(test_disabled_config_always_ok);
    RUN_TEST(test_enabled_config_valid);
    RUN_TEST(test_enabled_config_requires_keys);
    RUN_TEST(test_enabled_config_requires_endpoint_and_address);
    RUN_TEST(test_tailscale_valid_configs);
    RUN_TEST(test_tailscale_rejects);
    RUN_TEST(test_routes_list_rules);
    RUN_TEST(test_legacy_allowed_ip_converts);
    RUN_TEST(test_state_words_cover_every_state);
    RUN_TEST(test_ts_hold_reason_names_the_levers);
    UNITY_END();
}
