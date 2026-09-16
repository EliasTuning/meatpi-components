/**
 * @file test_main.c
 * @brief Host test entry point for the obd_chip pure parsing/framing logic.
 */
#include "unity.h"

void test_simple_response(void);
void test_echo_off_response(void);
void test_prompt_split_across_chunks(void);
void test_bytes_after_prompt_not_consumed(void);
void test_unsolicited_noise_interleaves_into_window(void);
void test_long_real_car_response_all_splits(void);
void test_overflow_is_flagged_not_fatal(void);
void test_chip_error_classification(void);
void test_monitor_command_classification(void);
void test_fw_iterator_and_end_marker(void);
void test_eeprom_guard(void);
void test_stslcs_parse_full_native(void);
void test_stslcs_parse_elm327_and_armed_wake(void);
void test_stslcs_provision_policy(void);
void test_stslcs_ignores_garbage(void);
void test_stslcs_midline_gt_is_not_the_prompt(void);

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_simple_response);
    RUN_TEST(test_echo_off_response);
    RUN_TEST(test_prompt_split_across_chunks);
    RUN_TEST(test_bytes_after_prompt_not_consumed);
    RUN_TEST(test_unsolicited_noise_interleaves_into_window);
    RUN_TEST(test_long_real_car_response_all_splits);
    RUN_TEST(test_overflow_is_flagged_not_fatal);
    RUN_TEST(test_chip_error_classification);
    RUN_TEST(test_monitor_command_classification);
    RUN_TEST(test_fw_iterator_and_end_marker);
    RUN_TEST(test_eeprom_guard);
    RUN_TEST(test_stslcs_parse_full_native);
    RUN_TEST(test_stslcs_parse_elm327_and_armed_wake);
    RUN_TEST(test_stslcs_provision_policy);
    RUN_TEST(test_stslcs_ignores_garbage);
    RUN_TEST(test_stslcs_midline_gt_is_not_the_prompt);
    UNITY_END();
}
