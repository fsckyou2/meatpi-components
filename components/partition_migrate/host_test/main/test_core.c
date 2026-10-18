/**
 * @file test_core.c
 * @brief The pure core against the two real WiCAN Pro tables: the v4.51p
 *        release's partition-table.bin (6 entries, one 6 MB `storage`)
 *        and the v6 build's (7 entries, `settings` + `storage`), byte for
 *        byte as esptool writes them, MD5 entry included.
 */
#include <string.h>

#include "unity.h"

#include "partition_migrate_core.h"

/* LEGACY_PRO_TABLE: 224 bytes, 6 entries + the MD5 entry */
static const uint8_t LEGACY_PRO_TABLE[224] =
{
    0xaa, 0x50, 0x01, 0x02, 0x00, 0x90, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x6e, 0x76, 0x73, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xaa, 0x50, 0x01, 0x00, 0x00, 0xd0, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x6f, 0x74, 0x61, 0x64,
    0x61, 0x74, 0x61, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xaa, 0x50, 0x01, 0x01, 0x00, 0xf0, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x70, 0x68, 0x79, 0x5f,
    0x69, 0x6e, 0x69, 0x74, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xaa, 0x50, 0x00, 0x10, 0x00, 0x00, 0x01, 0x00, 0x00, 0xe0, 0x4e, 0x00, 0x6f, 0x74, 0x61, 0x5f,
    0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xaa, 0x50, 0x00, 0x11, 0x00, 0x00, 0x50, 0x00, 0x00, 0xe0, 0x4e, 0x00, 0x6f, 0x74, 0x61, 0x5f,
    0x31, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xaa, 0x50, 0x01, 0x81, 0x00, 0xe0, 0x9e, 0x00, 0x00, 0x00, 0x60, 0x00, 0x73, 0x74, 0x6f, 0x72,
    0x61, 0x67, 0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xeb, 0xeb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x22, 0x52, 0x42, 0xe6, 0x82, 0x2a, 0x60, 0xaa, 0x93, 0xa9, 0xa6, 0x7a, 0xbd, 0x94, 0x42, 0xed,
};

/* V6_PRO_TABLE: 256 bytes, 7 entries + the MD5 entry */
static const uint8_t V6_PRO_TABLE[256] =
{
    0xaa, 0x50, 0x01, 0x02, 0x00, 0x90, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x6e, 0x76, 0x73, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xaa, 0x50, 0x01, 0x00, 0x00, 0xd0, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x6f, 0x74, 0x61, 0x64,
    0x61, 0x74, 0x61, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xaa, 0x50, 0x01, 0x01, 0x00, 0xf0, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x70, 0x68, 0x79, 0x5f,
    0x69, 0x6e, 0x69, 0x74, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xaa, 0x50, 0x00, 0x10, 0x00, 0x00, 0x01, 0x00, 0x00, 0xe0, 0x4e, 0x00, 0x6f, 0x74, 0x61, 0x5f,
    0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xaa, 0x50, 0x00, 0x11, 0x00, 0x00, 0x50, 0x00, 0x00, 0xe0, 0x4e, 0x00, 0x6f, 0x74, 0x61, 0x5f,
    0x31, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xaa, 0x50, 0x01, 0x83, 0x00, 0xe0, 0x9e, 0x00, 0x00, 0x00, 0x04, 0x00, 0x73, 0x65, 0x74, 0x74,
    0x69, 0x6e, 0x67, 0x73, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xaa, 0x50, 0x01, 0x83, 0x00, 0xe0, 0xa2, 0x00, 0x00, 0x00, 0x5c, 0x00, 0x73, 0x74, 0x6f, 0x72,
    0x61, 0x67, 0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xeb, 0xeb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x35, 0xc1, 0x6c, 0xb5, 0x67, 0x74, 0x77, 0xa2, 0x7c, 0x66, 0x2b, 0xd9, 0x12, 0x58, 0x23, 0x29,
};

/* the table as the flash region holds it: the bin, then 0xFF to 0xC00 */
static void as_flash(uint8_t *region, const uint8_t *bin, size_t bin_len)
{
    memset(region, 0xFF, PM_TABLE_MAX_LEN);
    memcpy(region, bin, bin_len);
}

/* entry i of a table: its 32 bytes start at i * 32 */
static uint8_t *entry(uint8_t *table, size_t i)
{
    return table + i * PM_ENTRY_LEN;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

void test_parse_the_real_tables(void)
{
    pm_entry_t e[PM_MAX_ENTRIES];
    size_t n;

    n = pm_table_parse(LEGACY_PRO_TABLE, sizeof(LEGACY_PRO_TABLE), e,
                       PM_MAX_ENTRIES);
    TEST_ASSERT_EQUAL_UINT(6, n);
    TEST_ASSERT_EQUAL_STRING("nvs", e[0].label);
    TEST_ASSERT_EQUAL_STRING("ota_1", e[4].label);
    TEST_ASSERT_EQUAL_HEX32(0x500000, e[4].offset);
    TEST_ASSERT_EQUAL_STRING("storage", e[5].label);
    TEST_ASSERT_EQUAL_HEX8(0x81, e[5].subtype);
    TEST_ASSERT_EQUAL_HEX32(0x9EE000, e[5].offset);
    TEST_ASSERT_EQUAL_HEX32(0x600000, e[5].size);

    n = pm_table_parse(V6_PRO_TABLE, sizeof(V6_PRO_TABLE), e,
                       PM_MAX_ENTRIES);
    TEST_ASSERT_EQUAL_UINT(7, n);
    TEST_ASSERT_EQUAL_STRING("settings", e[5].label);
    TEST_ASSERT_EQUAL_HEX32(0x9EE000, e[5].offset);
    TEST_ASSERT_EQUAL_HEX32(0x40000, e[5].size);
    TEST_ASSERT_EQUAL_STRING("storage", e[6].label);
    TEST_ASSERT_EQUAL_HEX32(0xA2E000, e[6].offset);
    TEST_ASSERT_EQUAL_HEX32(0x5C0000, e[6].size);

    TEST_ASSERT_TRUE(pm_entry_is_anchored(&e[0]));   /* nvs      */
    TEST_ASSERT_TRUE(pm_entry_is_anchored(&e[1]));   /* otadata  */
    TEST_ASSERT_TRUE(pm_entry_is_anchored(&e[2]));   /* phy_init */
    TEST_ASSERT_TRUE(pm_entry_is_anchored(&e[3]));   /* ota_0    */
    TEST_ASSERT_FALSE(pm_entry_is_anchored(&e[5]));  /* settings */
    TEST_ASSERT_FALSE(pm_entry_is_anchored(&e[6]));  /* storage  */
}

void test_same_table_is_ours(void)
{
    uint8_t flash[PM_TABLE_MAX_LEN];
    char why[160] = "";

    as_flash(flash, V6_PRO_TABLE, sizeof(V6_PRO_TABLE));
    TEST_ASSERT_EQUAL(PM_TABLE_SAME,
                      pm_table_compare(flash, sizeof(flash), V6_PRO_TABLE,
                                       sizeof(V6_PRO_TABLE), why,
                                       sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("this build's table", why);
}

void test_legacy_pro_table_migrates(void)
{
    uint8_t flash[PM_TABLE_MAX_LEN];
    char why[160] = "";

    as_flash(flash, LEGACY_PRO_TABLE, sizeof(LEGACY_PRO_TABLE));
    TEST_ASSERT_EQUAL(PM_TABLE_MIGRATE,
                      pm_table_compare(flash, sizeof(flash), V6_PRO_TABLE,
                                       sizeof(V6_PRO_TABLE), why,
                                       sizeof(why)));
    TEST_ASSERT_EQUAL_STRING(
        "storage 6144 KB at 0x9EE000 -> settings 256 KB at 0x9EE000, "
        "storage 5888 KB at 0xA2E000", why);
}

void test_moved_app_slot_is_foreign(void)
{
    uint8_t flash[PM_TABLE_MAX_LEN];
    char why[160] = "";

    as_flash(flash, LEGACY_PRO_TABLE, sizeof(LEGACY_PRO_TABLE));
    put32(entry(flash, 4) + 4, 0x510000);      /* ota_1 moved by 64 KB */
    TEST_ASSERT_EQUAL(PM_TABLE_FOREIGN,
                      pm_table_compare(flash, sizeof(flash), V6_PRO_TABLE,
                                       sizeof(V6_PRO_TABLE), why,
                                       sizeof(why)));
    TEST_ASSERT_NOT_NULL(strstr(why, "ota_1"));

    as_flash(flash, LEGACY_PRO_TABLE, sizeof(LEGACY_PRO_TABLE));
    put32(entry(flash, 3) + 8, 0x400000);      /* ota_0 smaller */
    TEST_ASSERT_EQUAL(PM_TABLE_FOREIGN,
                      pm_table_compare(flash, sizeof(flash), V6_PRO_TABLE,
                                       sizeof(V6_PRO_TABLE), why,
                                       sizeof(why)));
    TEST_ASSERT_NOT_NULL(strstr(why, "ota_0"));
}

void test_changed_nvs_or_otadata_is_foreign(void)
{
    uint8_t flash[PM_TABLE_MAX_LEN];
    char why[160] = "";

    as_flash(flash, LEGACY_PRO_TABLE, sizeof(LEGACY_PRO_TABLE));
    put32(entry(flash, 0) + 8, 0x6000);        /* nvs 24 KB */
    TEST_ASSERT_EQUAL(PM_TABLE_FOREIGN,
                      pm_table_compare(flash, sizeof(flash), V6_PRO_TABLE,
                                       sizeof(V6_PRO_TABLE), why,
                                       sizeof(why)));
    TEST_ASSERT_NOT_NULL(strstr(why, "nvs"));

    as_flash(flash, LEGACY_PRO_TABLE, sizeof(LEGACY_PRO_TABLE));
    put32(entry(flash, 1) + 4, 0xE000);        /* otadata elsewhere */
    TEST_ASSERT_EQUAL(PM_TABLE_FOREIGN,
                      pm_table_compare(flash, sizeof(flash), V6_PRO_TABLE,
                                       sizeof(V6_PRO_TABLE), why,
                                       sizeof(why)));
    TEST_ASSERT_NOT_NULL(strstr(why, "otadata"));
}

void test_missing_phy_init_is_foreign(void)
{
    uint8_t flash[PM_TABLE_MAX_LEN];
    char why[160] = "";

    /* the legacy table without its phy_init entry: ours has one the flash
       lacks, so the rewrite would create an anchored partition */
    memset(flash, 0xFF, sizeof(flash));
    memcpy(entry(flash, 0), entry((uint8_t *)LEGACY_PRO_TABLE, 0), 2 * PM_ENTRY_LEN);
    memcpy(entry(flash, 2), entry((uint8_t *)LEGACY_PRO_TABLE, 3), 3 * PM_ENTRY_LEN);
    TEST_ASSERT_EQUAL(PM_TABLE_FOREIGN,
                      pm_table_compare(flash, sizeof(flash), V6_PRO_TABLE,
                                       sizeof(V6_PRO_TABLE), why,
                                       sizeof(why)));
    TEST_ASSERT_NOT_NULL(strstr(why, "phy_init"));
}

void test_blank_flash_is_foreign(void)
{
    uint8_t flash[PM_TABLE_MAX_LEN];
    char why[160] = "";

    memset(flash, 0xFF, sizeof(flash));
    TEST_ASSERT_EQUAL(PM_TABLE_FOREIGN,
                      pm_table_compare(flash, sizeof(flash), V6_PRO_TABLE,
                                       sizeof(V6_PRO_TABLE), why,
                                       sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("no partition table in flash", why);
}

void test_garbage_is_foreign(void)
{
    uint8_t flash[PM_TABLE_MAX_LEN];
    pm_entry_t e[PM_MAX_ENTRIES];

    as_flash(flash, LEGACY_PRO_TABLE, sizeof(LEGACY_PRO_TABLE));
    entry(flash, 2)[0] = 0x12;                 /* a bad magic mid-table */
    TEST_ASSERT_EQUAL_UINT(0, pm_table_parse(flash, sizeof(flash), e,
                                             PM_MAX_ENTRIES));
    TEST_ASSERT_EQUAL(PM_TABLE_FOREIGN,
                      pm_table_compare(flash, sizeof(flash), V6_PRO_TABLE,
                                       sizeof(V6_PRO_TABLE), NULL, 0));

    as_flash(flash, LEGACY_PRO_TABLE, sizeof(LEGACY_PRO_TABLE));
    put32(entry(flash, 5) + 8, 0);             /* a zero-size partition */
    TEST_ASSERT_EQUAL_UINT(0, pm_table_parse(flash, sizeof(flash), e,
                                             PM_MAX_ENTRIES));

    for (size_t i = 0; i < sizeof(flash); i++)
    {
        flash[i] = (uint8_t)(i * 7U + 3U);     /* noise */
    }

    TEST_ASSERT_EQUAL_UINT(0, pm_table_parse(flash, sizeof(flash), e,
                                             PM_MAX_ENTRIES));
}

void test_extra_data_partition_migrates(void)
{
    uint8_t flash[PM_TABLE_MAX_LEN];
    char why[160] = "";
    static const uint8_t COREDUMP[PM_ENTRY_LEN] =
    {
        0xaa, 0x50, 0x01, 0x03, 0x00, 0x00, 0xfc, 0x00, 0x00, 0x00, 0x04, 0x00,
        'c', 'o', 'r', 'e', 'd', 'u', 'm', 'p', 0, 0, 0, 0, 0, 0, 0, 0,
        0x00, 0x00, 0x00, 0x00,
    };

    /* v6's table plus a coredump partition after storage: a data partition
       this build drops */
    memset(flash, 0xFF, sizeof(flash));
    memcpy(flash, V6_PRO_TABLE, 7 * PM_ENTRY_LEN);
    memcpy(entry(flash, 7), COREDUMP, PM_ENTRY_LEN);
    TEST_ASSERT_EQUAL(PM_TABLE_MIGRATE,
                      pm_table_compare(flash, sizeof(flash), V6_PRO_TABLE,
                                       sizeof(V6_PRO_TABLE), why,
                                       sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("coredump 256 KB at 0xFC0000 -> ", why);
}

void test_md5_entry_ends_the_table(void)
{
    uint8_t flash[PM_TABLE_MAX_LEN];
    pm_entry_t e[PM_MAX_ENTRIES];

    as_flash(flash, V6_PRO_TABLE, sizeof(V6_PRO_TABLE));

    for (size_t i = sizeof(V6_PRO_TABLE); i < sizeof(flash); i++)
    {
        flash[i] = (uint8_t)(i * 13U + 1U);    /* noise after the MD5 */
    }

    TEST_ASSERT_EQUAL_UINT(7, pm_table_parse(flash, sizeof(flash), e,
                                             PM_MAX_ENTRIES));
    TEST_ASSERT_EQUAL(PM_TABLE_SAME,
                      pm_table_compare(flash, sizeof(flash), V6_PRO_TABLE,
                                       sizeof(V6_PRO_TABLE), NULL, 0));
}

void test_too_many_entries_is_foreign(void)
{
    uint8_t flash[PM_TABLE_MAX_LEN];
    pm_entry_t e[PM_MAX_ENTRIES];

    memset(flash, 0xFF, sizeof(flash));

    for (size_t i = 0; i < PM_MAX_ENTRIES + 1; i++)
    {
        memcpy(entry(flash, i), entry((uint8_t *)V6_PRO_TABLE, 6),
               PM_ENTRY_LEN);
        put32(entry(flash, i) + 4, 0x100000U * (uint32_t)(i + 1));
    }

    TEST_ASSERT_EQUAL_UINT(0, pm_table_parse(flash, sizeof(flash), e,
                                             PM_MAX_ENTRIES));
    TEST_ASSERT_EQUAL(PM_TABLE_FOREIGN,
                      pm_table_compare(flash, sizeof(flash), V6_PRO_TABLE,
                                       sizeof(V6_PRO_TABLE), NULL, 0));
}

void test_unparsable_own_table_is_foreign(void)
{
    uint8_t flash[PM_TABLE_MAX_LEN];
    uint8_t ours[64];
    char why[160] = "";

    as_flash(flash, LEGACY_PRO_TABLE, sizeof(LEGACY_PRO_TABLE));
    memset(ours, 0, sizeof(ours));
    TEST_ASSERT_EQUAL(PM_TABLE_FOREIGN,
                      pm_table_compare(flash, sizeof(flash), ours,
                                       sizeof(ours), why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("this build's own table does not parse", why);
}

/* ---- the layout id (2026-10-18) ------------------------------------------------- */

static void hex16(const uint8_t *digest, char out[33])
{
    for (int i = 0; i < 16; i++)
    {
        snprintf(out + 2 * i, 3, "%02x", digest[i]);
    }
}

void test_md5_vectors(void)
{
    uint8_t d[16];
    char hex[33];
    static const char FOX[] = "The quick brown fox jumps over the lazy dog";
    uint8_t long_in[200];

    pm_md5((const uint8_t *)"", 0, d);
    hex16(d, hex);
    TEST_ASSERT_EQUAL_STRING("d41d8cd98f00b204e9800998ecf8427e", hex);
    pm_md5((const uint8_t *)"abc", 3, d);
    hex16(d, hex);
    TEST_ASSERT_EQUAL_STRING("900150983cd24fb0d6963f7d28e17f72", hex);
    pm_md5((const uint8_t *)FOX, strlen(FOX), d);
    hex16(d, hex);
    TEST_ASSERT_EQUAL_STRING("9e107d9d372bb6826bd81d3542a419d6", hex);
    /* 56 bytes: the padding needs a second block */
    pm_md5((const uint8_t *)"12345678901234567890123456789012345678901234567890123456",
           56, d);
    hex16(d, hex);
    TEST_ASSERT_EQUAL_STRING("49f193adce178490e34d1b3a4ec0064c", hex);
    /* 200 'a': several blocks */
    memset(long_in, 'a', sizeof(long_in));
    pm_md5(long_in, sizeof(long_in), d);
    hex16(d, hex);
    TEST_ASSERT_EQUAL_STRING("887f30b43b2867f4a9accceee7d16e6c", hex);
}

void test_table_md5_is_the_stored_digest(void)
{
    char hex[33];
    char stored[33];
    uint8_t flash[PM_TABLE_MAX_LEN];

    /* the digest IDF appended: the last 16 bytes of each table */
    TEST_ASSERT_TRUE(pm_table_md5(V6_PRO_TABLE, sizeof(V6_PRO_TABLE), hex));
    hex16(V6_PRO_TABLE + sizeof(V6_PRO_TABLE) - 16, stored);
    TEST_ASSERT_EQUAL_STRING(stored, hex);
    TEST_ASSERT_EQUAL_STRING("35c16cb5677477a27c662bd912582329", hex);

    TEST_ASSERT_TRUE(pm_table_md5(LEGACY_PRO_TABLE, sizeof(LEGACY_PRO_TABLE), hex));
    hex16(LEGACY_PRO_TABLE + sizeof(LEGACY_PRO_TABLE) - 16, stored);
    TEST_ASSERT_EQUAL_STRING(stored, hex);
    TEST_ASSERT_EQUAL_STRING("225242e6822a60aa93a9a67abd9442ed", hex);

    /* the same table as the flash region holds it (0xFF padding) */
    as_flash(flash, V6_PRO_TABLE, sizeof(V6_PRO_TABLE));
    TEST_ASSERT_TRUE(pm_table_md5(flash, sizeof(flash), hex));
    TEST_ASSERT_EQUAL_STRING("35c16cb5677477a27c662bd912582329", hex);

    /* a table without the MD5 entry still has an id: the entries' digest */
    as_flash(flash, V6_PRO_TABLE, sizeof(V6_PRO_TABLE) - 32);
    TEST_ASSERT_TRUE(pm_table_md5(flash, sizeof(flash), hex));
    TEST_ASSERT_EQUAL_STRING("35c16cb5677477a27c662bd912582329", hex);

    /* blank flash and garbage have none */
    memset(flash, 0xFF, sizeof(flash));
    TEST_ASSERT_FALSE(pm_table_md5(flash, sizeof(flash), hex));
    TEST_ASSERT_EQUAL_STRING("", hex);
    memset(flash, 0x5A, sizeof(flash));
    TEST_ASSERT_FALSE(pm_table_md5(flash, sizeof(flash), hex));
}
