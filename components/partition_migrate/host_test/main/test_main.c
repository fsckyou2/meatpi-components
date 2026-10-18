/**
 * @file test_main.c
 * @brief Host test entry point for the partition_migrate core.
 */
#include "unity.h"

void test_parse_the_real_tables(void);
void test_same_table_is_ours(void);
void test_legacy_pro_table_migrates(void);
void test_moved_app_slot_is_foreign(void);
void test_changed_nvs_or_otadata_is_foreign(void);
void test_missing_phy_init_is_foreign(void);
void test_blank_flash_is_foreign(void);
void test_garbage_is_foreign(void);
void test_extra_data_partition_migrates(void);
void test_md5_entry_ends_the_table(void);
void test_too_many_entries_is_foreign(void);
void test_unparsable_own_table_is_foreign(void);
void test_md5_vectors(void);
void test_table_md5_is_the_stored_digest(void);

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_the_real_tables);
    RUN_TEST(test_same_table_is_ours);
    RUN_TEST(test_legacy_pro_table_migrates);
    RUN_TEST(test_moved_app_slot_is_foreign);
    RUN_TEST(test_changed_nvs_or_otadata_is_foreign);
    RUN_TEST(test_missing_phy_init_is_foreign);
    RUN_TEST(test_blank_flash_is_foreign);
    RUN_TEST(test_garbage_is_foreign);
    RUN_TEST(test_extra_data_partition_migrates);
    RUN_TEST(test_md5_entry_ends_the_table);
    RUN_TEST(test_too_many_entries_is_foreign);
    RUN_TEST(test_unparsable_own_table_is_foreign);
    RUN_TEST(test_md5_vectors);
    RUN_TEST(test_table_md5_is_the_stored_digest);
    UNITY_END();
}
