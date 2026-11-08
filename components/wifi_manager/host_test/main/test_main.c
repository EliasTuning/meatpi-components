/**
 * @file test_main.c
 * @brief Host test entry point for the wifi_manager selection module.
 */
#include "unity.h"

void test_scan_prefers_primary(void);
void test_scan_falls_back_in_priority_order(void);
void test_scan_skips_failed_candidate(void);
void test_scan_all_failed_keeps_trying_round_robin(void);
void test_scan_clean_alternative_always_first(void);
void test_scan_nothing_present_returns_none(void);
void test_deprioritised_after_threshold_and_fade(void);
void test_faded_streak_starts_over(void);
void test_success_clears_failures(void);
void test_deprioritise_at_once(void);
void test_sequential_rotates_and_wraps(void);
void test_sequential_skips_failed(void);
void test_sequential_all_failed_rotates_anyway(void);
void test_sequential_first_pick_is_primary(void);
void test_single_network_keeps_trying(void);
void test_roam_better_available(void);
void test_duplicate_ssid_entries_are_independent(void);
void test_roam_never_to_same_ssid(void);
void test_parse_ap_ipv4(void);
void test_parse_ipv4_plain(void);
void test_default_hostname(void);
void test_netmask_valid(void);
void test_backoff_curve(void);
void test_ap_client_pause_policy(void);
void test_trial_connects_and_reports_address(void);
void test_trial_wrong_password_by_reason(void);
void test_trial_not_found_and_refused(void);
void test_trial_no_address_budget(void);
void test_trial_total_budget(void);
void test_trial_busy_station_lets_go_first(void);
void test_trial_leave_that_never_comes(void);
void test_trial_refuses_bad_credentials_and_a_second_run(void);
void test_trial_events_when_idle_or_done_are_not_ours(void);
void test_trial_clock_wrap(void);
void test_trial_channel_hint_kept_and_bounded(void);
void test_vpn_pause_applies(void);
void test_ssid_list_has(void);

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_scan_prefers_primary);
    RUN_TEST(test_scan_falls_back_in_priority_order);
    RUN_TEST(test_scan_skips_failed_candidate);
    RUN_TEST(test_scan_all_failed_keeps_trying_round_robin);
    RUN_TEST(test_scan_clean_alternative_always_first);
    RUN_TEST(test_scan_nothing_present_returns_none);
    RUN_TEST(test_deprioritised_after_threshold_and_fade);
    RUN_TEST(test_faded_streak_starts_over);
    RUN_TEST(test_success_clears_failures);
    RUN_TEST(test_deprioritise_at_once);
    RUN_TEST(test_sequential_rotates_and_wraps);
    RUN_TEST(test_sequential_skips_failed);
    RUN_TEST(test_sequential_all_failed_rotates_anyway);
    RUN_TEST(test_sequential_first_pick_is_primary);
    RUN_TEST(test_single_network_keeps_trying);
    RUN_TEST(test_roam_better_available);
    RUN_TEST(test_duplicate_ssid_entries_are_independent);
    RUN_TEST(test_roam_never_to_same_ssid);
    RUN_TEST(test_parse_ap_ipv4);
    RUN_TEST(test_parse_ipv4_plain);
    RUN_TEST(test_default_hostname);
    RUN_TEST(test_netmask_valid);
    RUN_TEST(test_backoff_curve);
    RUN_TEST(test_ap_client_pause_policy);
    RUN_TEST(test_trial_connects_and_reports_address);
    RUN_TEST(test_trial_wrong_password_by_reason);
    RUN_TEST(test_trial_not_found_and_refused);
    RUN_TEST(test_trial_no_address_budget);
    RUN_TEST(test_trial_total_budget);
    RUN_TEST(test_trial_busy_station_lets_go_first);
    RUN_TEST(test_trial_leave_that_never_comes);
    RUN_TEST(test_trial_refuses_bad_credentials_and_a_second_run);
    RUN_TEST(test_trial_events_when_idle_or_done_are_not_ours);
    RUN_TEST(test_trial_clock_wrap);
    RUN_TEST(test_trial_channel_hint_kept_and_bounded);
    RUN_TEST(test_vpn_pause_applies);
    RUN_TEST(test_ssid_list_has);
    UNITY_END();
}
