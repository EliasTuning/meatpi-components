/**
 * @file test_select.c
 * @brief Unit tests for the pure STA candidate selection + failure memory.
 */
#include <string.h>

#include "unity.h"

#include "wifi_manager_private.h"

static wm_select_state_t s_st;
static wm_network_t      s_cand[3];

static void reset(void)
{
    wm_select_init(&s_st);
    memset(s_cand, 0, sizeof(s_cand));
    strcpy(s_cand[0].ssid, "primary");
    strcpy(s_cand[1].ssid, "fb1");
    strcpy(s_cand[2].ssid, "fb2");
}

static void strike(int idx, uint32_t now)
{
    for (int i = 0; i < WM_AUTH_FAIL_THRESHOLD; i++)
    {
        wm_select_on_attempt_fail(&s_st, idx, now);
    }
}

void test_scan_prefers_primary(void)
{
    char present[2][WM_SSID_LEN] = { "fb1", "primary" };

    reset();
    TEST_ASSERT_EQUAL_INT(0, wm_select_from_scan(&s_st, s_cand, 3,
                                                 present, 2, 0));
}

void test_scan_falls_back_in_priority_order(void)
{
    char present[2][WM_SSID_LEN] = { "fb2", "fb1" };

    reset();
    /* primary absent -> fb1 (index 1) wins over fb2 despite scan order */
    TEST_ASSERT_EQUAL_INT(1, wm_select_from_scan(&s_st, s_cand, 3,
                                                 present, 2, 0));
}

void test_scan_skips_failed_candidate(void)
{
    char present[2][WM_SSID_LEN] = { "primary", "fb1" };

    reset();
    strike(0, 1000);
    /* three rejections on the primary: the visible fallback goes first */
    TEST_ASSERT_EQUAL_INT(1, wm_select_from_scan(&s_st, s_cand, 3,
                                                 present, 2, 1000));
}

void test_scan_all_failed_keeps_trying_round_robin(void)
{
    char present[2][WM_SSID_LEN] = { "primary", "fb1" };

    reset();
    strike(0, 1000);
    strike(1, 1000);

    /* everything visible has failed: never defer, alternate between
     * them every cycle (no throttle, no ban window) */
    int a = wm_select_from_scan(&s_st, s_cand, 3, present, 2, 2000);
    int b = wm_select_from_scan(&s_st, s_cand, 3, present, 2, 7000);
    int c = wm_select_from_scan(&s_st, s_cand, 3, present, 2, 12000);

    TEST_ASSERT_TRUE(a == 0 || a == 1);
    TEST_ASSERT_TRUE(b == 0 || b == 1);
    TEST_ASSERT_NOT_EQUAL(a, b);
    TEST_ASSERT_EQUAL_INT(a, c);
}

void test_scan_clean_alternative_always_first(void)
{
    char both[2][WM_SSID_LEN] = { "primary", "fb1" };

    reset();
    strike(0, 1000);

    /* a clean alternative is preferred over a failed one, every cycle,
     * for as long as the failure memory lasts... */
    for (uint32_t t = 1000; t < 1000 + WM_FAIL_MEMORY_MS - 5000; t += 5000)
    {
        TEST_ASSERT_EQUAL_INT(1, wm_select_from_scan(&s_st, s_cand, 3,
                                                     both, 2, t));
    }

    /* ...and once it faded the primary is back in front */
    TEST_ASSERT_EQUAL_INT(0, wm_select_from_scan(&s_st, s_cand, 3, both, 2,
                                                 1000 + WM_FAIL_MEMORY_MS + 1));
}

void test_scan_nothing_present_returns_none(void)
{
    char present[1][WM_SSID_LEN] = { "someone-elses-ap" };

    reset();
    TEST_ASSERT_EQUAL_INT(-1, wm_select_from_scan(&s_st, s_cand, 3,
                                                  present, 1, 0));
}

void test_deprioritised_after_threshold_and_fade(void)
{
    reset();

    wm_select_on_attempt_fail(&s_st, 0, 0);
    wm_select_on_attempt_fail(&s_st, 0, 0);
    TEST_ASSERT_FALSE(wm_select_is_deprioritised(&s_st, 0, 0));
    TEST_ASSERT_EQUAL_UINT8(2, wm_select_fail_count(&s_st, 0, 0));

    wm_select_on_attempt_fail(&s_st, 0, 0); /* third strike */
    TEST_ASSERT_TRUE(wm_select_is_deprioritised(&s_st, 0, 0));
    TEST_ASSERT_TRUE(wm_select_is_deprioritised(&s_st, 0,
                                                WM_FAIL_MEMORY_MS - 1));
    /* the memory fades: the entry regains its place in the order */
    TEST_ASSERT_FALSE(wm_select_is_deprioritised(&s_st, 0,
                                                 WM_FAIL_MEMORY_MS + 1));
    TEST_ASSERT_EQUAL_UINT8(0, wm_select_fail_count(&s_st, 0,
                                                    WM_FAIL_MEMORY_MS + 1));
}

void test_faded_streak_starts_over(void)
{
    reset();
    wm_select_on_attempt_fail(&s_st, 0, 0);
    wm_select_on_attempt_fail(&s_st, 0, 0);
    /* two old strikes, then one after the memory faded: count is 1 */
    wm_select_on_attempt_fail(&s_st, 0, WM_FAIL_MEMORY_MS + 5);
    TEST_ASSERT_EQUAL_UINT8(1, wm_select_fail_count(&s_st, 0,
                                                    WM_FAIL_MEMORY_MS + 5));
    TEST_ASSERT_FALSE(wm_select_is_deprioritised(&s_st, 0,
                                                 WM_FAIL_MEMORY_MS + 5));
}

void test_success_clears_failures(void)
{
    reset();
    strike(0, 0);
    TEST_ASSERT_TRUE(wm_select_is_deprioritised(&s_st, 0, 0));

    wm_select_on_success(&s_st, 0);
    TEST_ASSERT_FALSE(wm_select_is_deprioritised(&s_st, 0, 0));
    TEST_ASSERT_EQUAL_UINT8(0, wm_select_fail_count(&s_st, 0, 0));
}

void test_deprioritise_at_once(void)
{
    reset();
    /* a failed roam trial: one strike is proof enough */
    wm_select_deprioritise(&s_st, 0, 500);
    TEST_ASSERT_TRUE(wm_select_is_deprioritised(&s_st, 0, 500));
    /* ...and later strikes keep counting on top */
    wm_select_on_attempt_fail(&s_st, 0, 600);
    TEST_ASSERT_EQUAL_UINT8(WM_AUTH_FAIL_THRESHOLD + 1,
                            wm_select_fail_count(&s_st, 0, 600));
}

void test_sequential_rotates_and_wraps(void)
{
    reset();
    TEST_ASSERT_EQUAL_INT(0, wm_select_sequential(&s_st, s_cand, 3, 0));
    TEST_ASSERT_EQUAL_INT(1, wm_select_sequential(&s_st, s_cand, 3, 0));
    TEST_ASSERT_EQUAL_INT(2, wm_select_sequential(&s_st, s_cand, 3, 0));
    TEST_ASSERT_EQUAL_INT(0, wm_select_sequential(&s_st, s_cand, 3, 0));
}

void test_sequential_skips_failed(void)
{
    reset();
    strike(1, 0); /* index 1 */
    TEST_ASSERT_EQUAL_INT(0, wm_select_sequential(&s_st, s_cand, 3, 0));
    TEST_ASSERT_EQUAL_INT(2, wm_select_sequential(&s_st, s_cand, 3, 0));
}

void test_sequential_all_failed_rotates_anyway(void)
{
    reset();
    strike(0, 1000);
    strike(1, 1000);
    strike(2, 1000);

    /* nothing is ever deferred: 0, 1, 2, 0 ... at the normal cadence */
    int a = wm_select_sequential(&s_st, s_cand, 3, 1000);
    int b = wm_select_sequential(&s_st, s_cand, 3, 6000);
    int c = wm_select_sequential(&s_st, s_cand, 3, 11000);
    int d = wm_select_sequential(&s_st, s_cand, 3, 16000);

    TEST_ASSERT_EQUAL_INT(0, a);
    TEST_ASSERT_EQUAL_INT(1, b);
    TEST_ASSERT_EQUAL_INT(2, c);
    TEST_ASSERT_EQUAL_INT(0, d);
}

void test_sequential_first_pick_is_primary(void)
{
    reset();
    /* the pre-fix cursor made the first blind pick candidate 1 */
    TEST_ASSERT_EQUAL_INT(0, wm_select_sequential(&s_st, s_cand, 3, 0));
}

void test_single_network_keeps_trying(void)
{
    /* THE drive-home case: one configured SSID, wrong password at the
     * current location -> it is still our best chance: every cycle
     * returns it, no trickle, no ban window (meatpi 2026-09-06) */
    reset();
    strike(0, 1000);

    for (uint32_t t = 1000; t < 1000 + 15u * 60u * 1000u; t += 5000)
    {
        TEST_ASSERT_EQUAL_INT(0, wm_select_sequential(&s_st, s_cand, 1,
                                                      t));
    }
}

void test_roam_better_available(void)
{
    char home_visible[2][WM_SSID_LEN] = { "primary", "fb2" };
    char only_current[1][WM_SSID_LEN] = { "fb2" };

    reset();

    /* connected to fb2 (index 2); home appears -> migrate to index 0 */
    TEST_ASSERT_EQUAL_INT(0, wm_select_better(&s_st, s_cand, 3, 2,
                                              home_visible, 2, 0));
    /* only the current network visible -> stay */
    TEST_ASSERT_EQUAL_INT(-1, wm_select_better(&s_st, s_cand, 3, 2,
                                               only_current, 1, 0));
    /* already on the primary -> never roam */
    TEST_ASSERT_EQUAL_INT(-1, wm_select_better(&s_st, s_cand, 3, 0,
                                               home_visible, 2, 0));
    /* the better network rejected us lately -> stay on the working one */
    strike(0, 0);
    TEST_ASSERT_EQUAL_INT(-1, wm_select_better(&s_st, s_cand, 3, 2,
                                               home_visible, 2, 0));
    /* memory faded -> migrate again */
    TEST_ASSERT_EQUAL_INT(0, wm_select_better(&s_st, s_cand, 3, 2,
                                              home_visible, 2,
                                              WM_FAIL_MEMORY_MS + 1));
}

void test_duplicate_ssid_entries_are_independent(void)
{
    /* the reason the memory is per ENTRY: two entries share a name with
     * different passwords (home vs the office); rejections with one
     * password must not stop the other from being tried */
    char present[1][WM_SSID_LEN] = { "twin" };
    wm_network_t twins[2];

    memset(twins, 0, sizeof(twins));
    strcpy(twins[0].ssid, "twin");
    strcpy(twins[0].password, "pw-home");
    strcpy(twins[1].ssid, "twin");
    strcpy(twins[1].password, "pw-office");

    reset();
    strike(0, 1000);
    /* entry 0 rejected three times -> entry 1 (other password) next */
    TEST_ASSERT_EQUAL_INT(1, wm_select_from_scan(&s_st, twins, 2,
                                                 present, 1, 1000));
    /* both rejected -> alternate, never defer */
    strike(1, 1000);
    int a = wm_select_from_scan(&s_st, twins, 2, present, 1, 2000);
    int b = wm_select_from_scan(&s_st, twins, 2, present, 1, 7000);

    TEST_ASSERT_NOT_EQUAL(a, b);
    /* success on entry 1 clears only entry 1 */
    wm_select_on_success(&s_st, 1);
    TEST_ASSERT_FALSE(wm_select_is_deprioritised(&s_st, 1, 8000));
    TEST_ASSERT_TRUE(wm_select_is_deprioritised(&s_st, 0, 8000));
}

void test_roam_never_to_same_ssid(void)
{
    /* connected on the office entry of a shared name: the home entry is
     * the same AP with another password, no reason to leave */
    char present[1][WM_SSID_LEN] = { "twin" };
    wm_network_t twins[2];

    memset(twins, 0, sizeof(twins));
    strcpy(twins[0].ssid, "twin");
    strcpy(twins[1].ssid, "twin");

    reset();
    TEST_ASSERT_EQUAL_INT(-1, wm_select_better(&s_st, twins, 2, 1,
                                               present, 1, 0));
}

void test_parse_ap_ipv4(void)
{
    uint32_t ip = 0;

    /* the legacy default parses to host-order 0xC0A85001 */
    TEST_ASSERT_TRUE(wm_parse_ap_ipv4("192.168.80.1", &ip));
    TEST_ASSERT_EQUAL_HEX32(0xC0A85001u, ip);

    TEST_ASSERT_TRUE(wm_parse_ap_ipv4("10.0.0.1", &ip));
    TEST_ASSERT_EQUAL_HEX32(0x0A000001u, ip);
    TEST_ASSERT_TRUE(wm_parse_ap_ipv4("172.16.5.254", &ip));

    /* rejects: host octet 0/255, out-of-range octet, short/long, junk,
       empty, NULL, leading/trailing garbage, negative */
    TEST_ASSERT_FALSE(wm_parse_ap_ipv4("192.168.80.0", &ip));
    TEST_ASSERT_FALSE(wm_parse_ap_ipv4("192.168.80.255", &ip));
    TEST_ASSERT_FALSE(wm_parse_ap_ipv4("192.168.80.256", &ip));
    TEST_ASSERT_FALSE(wm_parse_ap_ipv4("192.168.80", &ip));
    TEST_ASSERT_FALSE(wm_parse_ap_ipv4("192.168.80.1.5", &ip));
    TEST_ASSERT_FALSE(wm_parse_ap_ipv4("192.168.80.1 ", &ip));
    TEST_ASSERT_FALSE(wm_parse_ap_ipv4("192.168..1", &ip));
    TEST_ASSERT_FALSE(wm_parse_ap_ipv4("", &ip));
    TEST_ASSERT_FALSE(wm_parse_ap_ipv4("garbage", &ip));
    TEST_ASSERT_FALSE(wm_parse_ap_ipv4("-1.0.0.1", &ip));
    TEST_ASSERT_FALSE(wm_parse_ap_ipv4(NULL, &ip));
}

void test_default_hostname(void)
{
    char out[WM_HOSTNAME_LEN];

    /* empty setting -> wican_<device id> (the mDNS host name) */
    TEST_ASSERT_TRUE(wm_default_hostname("", "68ee8f5a653d", out,
                                         sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("wican_68ee8f5a653d", out);

    /* a configured name wins verbatim */
    TEST_ASSERT_TRUE(wm_default_hostname("garage-car", "68ee8f5a653d",
                                         out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("garage-car", out);

    /* nothing to build from: false + empty output */
    TEST_ASSERT_FALSE(wm_default_hostname("", "", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_FALSE(wm_default_hostname(NULL, NULL, out, sizeof(out)));

    /* the 32-char setting cap fits WM_HOSTNAME_LEN exactly */
    TEST_ASSERT_TRUE(wm_default_hostname("abcdefghijklmnopqrstuvwxyz012345",
                                         "x", out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(32, (int)strlen(out));

    /* a buffer too small for the result is refused, never truncated */
    char tiny[8];

    TEST_ASSERT_FALSE(wm_default_hostname("", "68ee8f5a653d", tiny,
                                          sizeof(tiny)));
}

void test_parse_ipv4_plain(void)
{
    uint32_t ip = 0;

    /* the plain parser accepts network/broadcast host octets (it feeds
       netmask + DNS fields, not host addresses) */
    TEST_ASSERT_TRUE(wm_parse_ipv4("255.255.255.0", &ip));
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFF00u, ip);
    TEST_ASSERT_TRUE(wm_parse_ipv4("10.0.0.0", &ip));
    TEST_ASSERT_TRUE(wm_parse_ipv4("1.1.1.1", &ip));
    TEST_ASSERT_EQUAL_HEX32(0x01010101u, ip);

    /* same lexical rules as the host-address parser */
    TEST_ASSERT_FALSE(wm_parse_ipv4("256.0.0.1", &ip));
    TEST_ASSERT_FALSE(wm_parse_ipv4("10.0.0", &ip));
    TEST_ASSERT_FALSE(wm_parse_ipv4("10.0.0.1.2", &ip));
    TEST_ASSERT_FALSE(wm_parse_ipv4("", &ip));
    TEST_ASSERT_FALSE(wm_parse_ipv4(NULL, &ip));
}

void test_netmask_valid(void)
{
    /* contiguous masks /8../31 */
    TEST_ASSERT_TRUE(wm_netmask_valid(0xFF000000u));  /* /8  */
    TEST_ASSERT_TRUE(wm_netmask_valid(0xFFFF0000u));  /* /16 */
    TEST_ASSERT_TRUE(wm_netmask_valid(0xFFFFFF00u));  /* /24 */
    TEST_ASSERT_TRUE(wm_netmask_valid(0xFFFFFF80u));  /* /25 */
    TEST_ASSERT_TRUE(wm_netmask_valid(0xFFFFFFFEu));  /* /31 */

    /* degenerate or holed masks */
    TEST_ASSERT_FALSE(wm_netmask_valid(0));            /* /0        */
    TEST_ASSERT_FALSE(wm_netmask_valid(0xFFFFFFFFu));  /* /32       */
    TEST_ASSERT_FALSE(wm_netmask_valid(0xFF00FF00u));  /* holed     */
    TEST_ASSERT_FALSE(wm_netmask_valid(0x80000001u));  /* holed     */
    TEST_ASSERT_FALSE(wm_netmask_valid(0x00FFFF00u));  /* not MSB   */
}

void test_ap_client_pause_policy(void)
{
    /* no client on the AP: never pause */
    TEST_ASSERT_FALSE(wm_sta_pause_for_ap_clients(0, true, 0, 6));
    TEST_ASSERT_FALSE(wm_sta_pause_for_ap_clients(0, false, 99, 6));
    /* a client but no association yet this boot (fresh device being
     * configured over its own AP): connect right away */
    TEST_ASSERT_FALSE(wm_sta_pause_for_ap_clients(1, false, 0, 6));
    TEST_ASSERT_FALSE(wm_sta_pause_for_ap_clients(3, false, 0, 6));
    /* reconnect with a client parked on the AP: defer, but bounded */
    TEST_ASSERT_TRUE(wm_sta_pause_for_ap_clients(1, true, 0, 6));
    TEST_ASSERT_TRUE(wm_sta_pause_for_ap_clients(1, true, 5, 6));
    TEST_ASSERT_FALSE(wm_sta_pause_for_ap_clients(1, true, 6, 6));
    TEST_ASSERT_FALSE(wm_sta_pause_for_ap_clients(2, true, 7, 6));
}

void test_backoff_curve(void)
{
    /* first two attempts stay on the 5 s cadence (candidate walk) */
    TEST_ASSERT_EQUAL_INT(0, wm_backoff_skip_loops(1));
    TEST_ASSERT_EQUAL_INT(0, wm_backoff_skip_loops(2));

    /* then double: 10 / 20 s ... */
    TEST_ASSERT_EQUAL_INT(1, wm_backoff_skip_loops(3));
    TEST_ASSERT_EQUAL_INT(3, wm_backoff_skip_loops(4));

    /* ... capped at the 30 s cadence forever after */
    TEST_ASSERT_EQUAL_INT(WM_BACKOFF_MAX_SKIP_LOOPS,
                          wm_backoff_skip_loops(5));
    TEST_ASSERT_EQUAL_INT(WM_BACKOFF_MAX_SKIP_LOOPS,
                          wm_backoff_skip_loops(6));
    TEST_ASSERT_EQUAL_INT(WM_BACKOFF_MAX_SKIP_LOOPS,
                          wm_backoff_skip_loops(7));
    TEST_ASSERT_EQUAL_INT(WM_BACKOFF_MAX_SKIP_LOOPS,
                          wm_backoff_skip_loops(1000));

    /* degenerate inputs never go negative */
    TEST_ASSERT_EQUAL_INT(0, wm_backoff_skip_loops(0));
    TEST_ASSERT_EQUAL_INT(0, wm_backoff_skip_loops(-5));
}

void test_vpn_pause_applies(void)
{
    bool flags[WM_MAX_CANDIDATES] = { true, false, true, false, false, false };

    /* no station session, an index past the configured list, an empty
       list or no flags at all: never */
    TEST_ASSERT_FALSE(wm_vpn_pause_applies(flags, 3, -1));
    TEST_ASSERT_FALSE(wm_vpn_pause_applies(flags, 3, 3));
    TEST_ASSERT_FALSE(wm_vpn_pause_applies(flags, 0, 0));
    TEST_ASSERT_FALSE(wm_vpn_pause_applies(NULL, 3, 0));
    TEST_ASSERT_FALSE(wm_vpn_pause_applies(flags, WM_MAX_CANDIDATES + 1, 0));

    /* the connected candidate's own flag, primary or backup */
    TEST_ASSERT_TRUE(wm_vpn_pause_applies(flags, 3, 0));
    TEST_ASSERT_FALSE(wm_vpn_pause_applies(flags, 3, 1));
    TEST_ASSERT_TRUE(wm_vpn_pause_applies(flags, 3, 2));

    /* a backup that moved up a slot keeps its flag: the collapse is the
       settings parser's job, the lookup only sees the collapsed list */
    bool collapsed[WM_MAX_CANDIDATES] = { false, true };

    TEST_ASSERT_TRUE(wm_vpn_pause_applies(collapsed, 2, 1));
    TEST_ASSERT_FALSE(wm_vpn_pause_applies(collapsed, 2, 0));
}

/* the sender's network list (2026-11-08): whole names, commas, blanks */
void test_ssid_list_has(void)
{
    /* nothing listed, nothing asked: never a match */
    TEST_ASSERT_FALSE(wm_ssid_list_has("", "HomeWiFi"));
    TEST_ASSERT_FALSE(wm_ssid_list_has(NULL, "HomeWiFi"));
    TEST_ASSERT_FALSE(wm_ssid_list_has("HomeWiFi", ""));
    TEST_ASSERT_FALSE(wm_ssid_list_has("HomeWiFi", NULL));

    /* one name, several names, blanks around the commas */
    TEST_ASSERT_TRUE(wm_ssid_list_has("HomeWiFi", "HomeWiFi"));
    TEST_ASSERT_TRUE(wm_ssid_list_has("HomeWiFi,Garage", "Garage"));
    TEST_ASSERT_TRUE(wm_ssid_list_has("HomeWiFi, Garage ,Office", "Garage"));
    TEST_ASSERT_TRUE(wm_ssid_list_has(" HomeWiFi ", "HomeWiFi"));
    TEST_ASSERT_FALSE(wm_ssid_list_has("HomeWiFi,Garage", "Office"));

    /* whole names only: no prefix, no suffix, no case folding */
    TEST_ASSERT_FALSE(wm_ssid_list_has("HomeWiFi", "Home"));
    TEST_ASSERT_FALSE(wm_ssid_list_has("HomeWiFi", "HomeWiFi2"));
    TEST_ASSERT_FALSE(wm_ssid_list_has("HomeWiFi,Garage", "homewifi"));

    /* a name with a space inside keeps it; empty entries are skipped */
    TEST_ASSERT_TRUE(wm_ssid_list_has("My Home, Garage", "My Home"));
    TEST_ASSERT_TRUE(wm_ssid_list_has(",,Garage,", "Garage"));
    TEST_ASSERT_FALSE(wm_ssid_list_has(",,", "Garage"));
}
