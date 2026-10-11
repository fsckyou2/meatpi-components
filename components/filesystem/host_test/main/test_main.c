/**
 * @file test_main.c
 * @brief Host test entry point for the filesystem path module.
 */
#include "unity.h"

void test_resolve_accepts_valid_paths(void);
void test_resolve_maps_backends(void);
void test_resolve_rejects_unknown_prefix(void);
void test_resolve_rejects_traversal_and_dot(void);
void test_resolve_rejects_empty_segment_and_trailing_slash(void);
void test_resolve_rejects_bad_chars_and_overlong(void);
void test_temp_name_appends_suffix(void);
void test_temp_name_rejects_overflow(void);
void test_parent_derivation(void);
void test_parent_of_root_rejected(void);
void test_region_blank_detection(void);
void test_lfs_probe_blank_ours_and_factory(void);
void test_lfs_probe_other_and_bad_input(void);
void test_api_hidden_folders(void);

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_resolve_accepts_valid_paths);
    RUN_TEST(test_resolve_maps_backends);
    RUN_TEST(test_resolve_rejects_unknown_prefix);
    RUN_TEST(test_resolve_rejects_traversal_and_dot);
    RUN_TEST(test_resolve_rejects_empty_segment_and_trailing_slash);
    RUN_TEST(test_resolve_rejects_bad_chars_and_overlong);
    RUN_TEST(test_temp_name_appends_suffix);
    RUN_TEST(test_temp_name_rejects_overflow);
    RUN_TEST(test_parent_derivation);
    RUN_TEST(test_parent_of_root_rejected);
    RUN_TEST(test_region_blank_detection);
    RUN_TEST(test_lfs_probe_blank_ours_and_factory);
    RUN_TEST(test_lfs_probe_other_and_bad_input);
    RUN_TEST(test_api_hidden_folders);
    UNITY_END();
}
