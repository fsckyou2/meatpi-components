/**
 * @file test_path.c
 * @brief Unit tests for fs_path_resolve / fs_path_temp_name / fs_path_parent.
 */
#include <string.h>

#include "unity.h"

#include "filesystem_private.h"
#include "filesystem.h"

void test_resolve_accepts_valid_paths(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, fs_path_resolve("/data", NULL));
    TEST_ASSERT_EQUAL(ESP_OK, fs_path_resolve("/sd", NULL));
    TEST_ASSERT_EQUAL(ESP_OK, fs_path_resolve("/data/a.txt", NULL));
    TEST_ASSERT_EQUAL(ESP_OK, fs_path_resolve("/data/web/icons/x.svg", NULL));
    TEST_ASSERT_EQUAL(ESP_OK, fs_path_resolve("/sd/logs/2026-07-02.log", NULL));
    TEST_ASSERT_EQUAL(ESP_OK, fs_path_resolve("/data/with space.txt", NULL));
}

void test_resolve_maps_backends(void)
{
    fs_backend_t b;

    TEST_ASSERT_EQUAL(ESP_OK, fs_path_resolve("/data/x", &b));
    TEST_ASSERT_EQUAL(FS_BACKEND_INTERNAL, b);

    TEST_ASSERT_EQUAL(ESP_OK, fs_path_resolve("/sd/x", &b));
    TEST_ASSERT_EQUAL(FS_BACKEND_SD, b);
}

void test_resolve_rejects_unknown_prefix(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, fs_path_resolve(NULL, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, fs_path_resolve("", NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, fs_path_resolve("data/x", NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, fs_path_resolve("/nope/x", NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, fs_path_resolve("/datax/y", NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, fs_path_resolve("/settings/x", NULL));
}

void test_resolve_rejects_traversal_and_dot(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, fs_path_resolve("/data/../etc", NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, fs_path_resolve("/data/a/../b", NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, fs_path_resolve("/data/./a", NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, fs_path_resolve("/data/..", NULL));
}

void test_resolve_rejects_empty_segment_and_trailing_slash(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, fs_path_resolve("/data//x", NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, fs_path_resolve("/data/", NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, fs_path_resolve("/data/a/", NULL));
}

void test_resolve_rejects_bad_chars_and_overlong(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, fs_path_resolve("/data/a\\b", NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, fs_path_resolve("/data/a\tb", NULL));

    char long_path[FS_PATH_MAX + 8];

    memset(long_path, 'a', sizeof(long_path));
    memcpy(long_path, "/data/", 6);
    long_path[sizeof(long_path) - 1] = '\0';
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, fs_path_resolve(long_path, NULL));
}

void test_temp_name_appends_suffix(void)
{
    char out[FS_PATH_MAX];

    TEST_ASSERT_EQUAL(ESP_OK,
                      fs_path_temp_name("/data/a.txt", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("/data/a.txt.tmp", out);
}

void test_temp_name_rejects_overflow(void)
{
    char small[8];

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      fs_path_temp_name("/data/a.txt", small, sizeof(small)));

    /* path that fits FS_PATH_MAX but whose temp sibling would not */
    char long_path[FS_PATH_MAX];
    char out[FS_PATH_MAX + 16];

    memset(long_path, 'a', sizeof(long_path));
    memcpy(long_path, "/data/", 6);
    long_path[FS_PATH_MAX - 2] = '\0'; /* len = FS_PATH_MAX - 2 */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      fs_path_temp_name(long_path, out, sizeof(out)));
}

void test_parent_derivation(void)
{
    char out[FS_PATH_MAX];

    TEST_ASSERT_EQUAL(ESP_OK,
                      fs_path_parent("/data/a/b.txt", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("/data/a", out);

    TEST_ASSERT_EQUAL(ESP_OK, fs_path_parent("/data/b.txt", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("/data", out);
}

void test_parent_of_root_rejected(void)
{
    char out[FS_PATH_MAX];

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      fs_path_parent("/data", out, sizeof(out)));
}

/* first-boot detection: erased flash is all 0xFF; anything else (a
 * formatted superblock, a single stray byte, nothing at all) is not blank */
void test_region_blank_detection(void)
{
    uint8_t erased[64];
    memset(erased, 0xFF, sizeof(erased));
    TEST_ASSERT_TRUE(fs_region_is_blank(erased, sizeof(erased)));

    uint8_t formatted[64];
    memset(formatted, 0xFF, sizeof(formatted));
    memcpy(formatted + 8, "littlefs", 8); /* lfs superblock magic */
    TEST_ASSERT_FALSE(fs_region_is_blank(formatted, sizeof(formatted)));

    uint8_t stray[64];
    memset(stray, 0xFF, sizeof(stray));
    stray[63] = 0x00;
    TEST_ASSERT_FALSE(fs_region_is_blank(stray, sizeof(stray)));

    TEST_ASSERT_FALSE(fs_region_is_blank(erased, 0));
    TEST_ASSERT_FALSE(fs_region_is_blank(NULL, 16));
}

/* ---- the superblock probe (2026-10-06) ---------------------------------- */

/* a block as LittleFS writes it: revision, a tag, "littlefs", a tag, the
 * inline struct {version, block_size, block_count, name_max, file_max,
 * attr_max} little-endian; the rest erased */
static void lfs_superblock(uint8_t *blk, size_t len, size_t at,
                                uint32_t version, uint32_t block_size,
                                uint32_t block_count)
{
    memset(blk, 0xFF, len);
    memset(blk, 0x00, at); /* revision + the tag before the name */
    memcpy(blk + at, "littlefs", 8);
    memset(blk + at + 8, 0x5A, 4); /* the struct's tag (XOR-chained) */

    uint32_t v[6] = { version, block_size, block_count, 64, 0x7FFFFFFF, 1022 };

    for (int i = 0; i < 6; i++)
    {
        blk[at + 12 + i * 4 + 0] = (uint8_t)(v[i]);
        blk[at + 12 + i * 4 + 1] = (uint8_t)(v[i] >> 8);
        blk[at + 12 + i * 4 + 2] = (uint8_t)(v[i] >> 16);
        blk[at + 12 + i * 4 + 3] = (uint8_t)(v[i] >> 24);
    }
}

/* a blank pair is a first boot; our own superblock reads our block count;
   the factory firmware's 6 MB filesystem reads 1536 (the report of
   2026-10-06); one erased block of the pair is still a filesystem */
void test_lfs_probe_blank_ours_and_factory(void)
{
    uint8_t b0[256];
    uint8_t b1[256];
    uint32_t n = 0;

    memset(b0, 0xFF, sizeof(b0));
    memset(b1, 0xFF, sizeof(b1));
    TEST_ASSERT_EQUAL_INT(FS_LFS_BLANK, fs_lfs_probe(b0, b1, 256, 4096, &n));

    lfs_superblock(b0, 256, 8, 0x00020001, 4096, 64);
    lfs_superblock(b1, 256, 8, 0x00020001, 4096, 64);
    TEST_ASSERT_EQUAL_INT(FS_LFS_LITTLEFS, fs_lfs_probe(b0, b1, 256, 4096, &n));
    TEST_ASSERT_EQUAL_UINT32(64, n);

    lfs_superblock(b0, 256, 8, 0x00020001, 4096, 1536);
    lfs_superblock(b1, 256, 8, 0x00020001, 4096, 1536);
    TEST_ASSERT_EQUAL_INT(FS_LFS_LITTLEFS, fs_lfs_probe(b0, b1, 256, 4096, &n));
    TEST_ASSERT_EQUAL_UINT32(1536, n);

    memset(b0, 0xFF, sizeof(b0));                  /* block 0 erased */
    TEST_ASSERT_EQUAL_INT(FS_LFS_LITTLEFS, fs_lfs_probe(b0, b1, 256, 4096, &n));
    TEST_ASSERT_EQUAL_UINT32(1536, n);

    lfs_superblock(b0, 256, 40, 0x00020001, 4096, 1472); /* deeper in */
    memset(b1, 0xFF, sizeof(b1));
    TEST_ASSERT_EQUAL_INT(FS_LFS_LITTLEFS, fs_lfs_probe(b0, b1, 256, 4096, &n));
    TEST_ASSERT_EQUAL_UINT32(1472, n);
}

/* what is not a LittleFS of ours: a FAT boot sector, data blocks, a name
   with no sane struct behind it, another block size, a version-1
   filesystem, a struct cut off by the probe's length, bad arguments */
void test_lfs_probe_other_and_bad_input(void)
{
    uint8_t b0[256];
    uint8_t b1[256];
    uint32_t n = 99;

    memset(b0, 0xFF, sizeof(b0));
    memset(b1, 0xFF, sizeof(b1));
    memcpy(b0, "ë<MSDOS5.0", 11);     /* FAT */
    TEST_ASSERT_EQUAL_INT(FS_LFS_OTHER, fs_lfs_probe(b0, b1, 256, 4096, &n));

    for (size_t i = 0; i < sizeof(b0); i++)       /* data blocks */
    {
        b0[i] = (uint8_t)(i * 7 + 3);
        b1[i] = (uint8_t)(i * 13 + 1);
    }
    TEST_ASSERT_EQUAL_INT(FS_LFS_OTHER, fs_lfs_probe(b0, b1, 256, 4096, &n));

    memset(b0, 0xFF, sizeof(b0));
    memcpy(b0 + 8, "littlefs", 8);                /* the name alone */
    memset(b1, 0xFF, sizeof(b1));
    TEST_ASSERT_EQUAL_INT(FS_LFS_OTHER, fs_lfs_probe(b0, b1, 256, 4096, &n));

    lfs_superblock(b0, 256, 8, 0x00020001, 256, 1536); /* block size */
    TEST_ASSERT_EQUAL_INT(FS_LFS_OTHER, fs_lfs_probe(b0, b1, 256, 4096, &n));
    lfs_superblock(b0, 256, 8, 0x00010007, 4096, 1536); /* version 1 */
    TEST_ASSERT_EQUAL_INT(FS_LFS_OTHER, fs_lfs_probe(b0, b1, 256, 4096, &n));
    lfs_superblock(b0, 256, 8, 0x00020001, 4096, 0);    /* no blocks */
    TEST_ASSERT_EQUAL_INT(FS_LFS_OTHER, fs_lfs_probe(b0, b1, 256, 4096, &n));

    lfs_superblock(b0, 256, 8, 0x00020001, 4096, 1536);
    TEST_ASSERT_EQUAL_INT(FS_LFS_OTHER, fs_lfs_probe(b0, b1, 30, 4096, &n)); /* cut */
    TEST_ASSERT_EQUAL_INT(FS_LFS_OTHER, fs_lfs_probe(NULL, b1, 256, 4096, &n));
    TEST_ASSERT_EQUAL_INT(FS_LFS_OTHER, fs_lfs_probe(b0, b1, 256, 4096, NULL));
    TEST_ASSERT_EQUAL_UINT32(99, n);
}

/* the folders the HTTP file surface never reaches (config_backup, 2026-10-11) */
void test_api_hidden_folders(void)
{
    TEST_ASSERT_TRUE(filesystem_api_hidden("/data/certs"));
    TEST_ASSERT_TRUE(filesystem_api_hidden("/data/certs/homeca/client.key"));
    TEST_ASSERT_TRUE(filesystem_api_hidden("/data/backups"));
    TEST_ASSERT_TRUE(filesystem_api_hidden("/data/backups/latest/settings.enc"));
    TEST_ASSERT_TRUE(filesystem_api_hidden("/sd/backups/wican-68ee8f5a653d/latest"));
    TEST_ASSERT_TRUE(filesystem_api_hidden("/SD/Backups/x"));   /* FAT folds case */
    TEST_ASSERT_FALSE(filesystem_api_hidden("/data/certsx"));
    TEST_ASSERT_FALSE(filesystem_api_hidden("/data/Certs"));     /* LittleFS does not */
    TEST_ASSERT_FALSE(filesystem_api_hidden("/data"));
    TEST_ASSERT_FALSE(filesystem_api_hidden("/data/scripts/hello.be"));
    TEST_ASSERT_FALSE(filesystem_api_hidden("/sd/logs"));
    TEST_ASSERT_FALSE(filesystem_api_hidden(NULL));
}
