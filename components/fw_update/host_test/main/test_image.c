/* The image head parser (2026-10-19): the dongle relay reads the project
   name and the version of a staged image before it pushes it. */
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "fw_update_core.h"

static void make_head(uint8_t *h, const char *project, const char *version)
{
    memset(h, 0, FWU_IMAGE_HEAD_LEN);
    h[0] = 0xE9;            /* the image magic */
    h[12] = 0x09;           /* chip id 9: ESP32-S3 (LE) */
    h[13] = 0x00;
    h[32] = 0x32;           /* the descriptor magic 0xABCD5432 (LE) */
    h[33] = 0x54;
    h[34] = 0xCD;
    h[35] = 0xAB;
    strncpy((char *)h + 32 + 16, version, 32);
    strncpy((char *)h + 32 + 48, project, 32);
}

void test_image_head_parse(void)
{
    uint8_t h[FWU_IMAGE_HEAD_LEN + 16];
    char project[33];
    char version[33];

    make_head(h, "espnetlink-fw", "1.23.0-alfa.1");
    TEST_ASSERT_TRUE(fwu_image_head_parse(h, sizeof(h), project, sizeof(project),
                                          version, sizeof(version)));
    TEST_ASSERT_EQUAL_STRING("espnetlink-fw", project);
    TEST_ASSERT_EQUAL_STRING("1.23.0-alfa.1", version);

    /* a 32-character field has no terminator inside the image */
    make_head(h, "abcdefghijklmnopqrstuvwxyz012345", "v1.22-61-gcc81591");
    TEST_ASSERT_TRUE(fwu_image_head_parse(h, FWU_IMAGE_HEAD_LEN, project,
                                          sizeof(project), version, sizeof(version)));
    TEST_ASSERT_EQUAL_STRING("abcdefghijklmnopqrstuvwxyz012345", project);
    TEST_ASSERT_EQUAL_STRING("v1.22-61-gcc81591", version);

    /* a short caller buffer is cut, never overrun */
    char small[8];
    TEST_ASSERT_TRUE(fwu_image_head_parse(h, FWU_IMAGE_HEAD_LEN, small, sizeof(small),
                                          version, sizeof(version)));
    TEST_ASSERT_EQUAL_STRING("abcdefg", small);

    /* not an image: the magic, the chip, the descriptor, the length */
    make_head(h, "espnetlink-fw", "1.23.0");
    h[0] = 0xE8;
    TEST_ASSERT_FALSE(fwu_image_head_parse(h, sizeof(h), project, sizeof(project),
                                           version, sizeof(version)));
    make_head(h, "espnetlink-fw", "1.23.0");
    h[12] = 0x00;           /* chip id 0: ESP32 */
    TEST_ASSERT_FALSE(fwu_image_head_parse(h, sizeof(h), project, sizeof(project),
                                           version, sizeof(version)));
    make_head(h, "espnetlink-fw", "1.23.0");
    h[35] = 0x00;
    TEST_ASSERT_FALSE(fwu_image_head_parse(h, sizeof(h), project, sizeof(project),
                                           version, sizeof(version)));
    make_head(h, "espnetlink-fw", "1.23.0");
    TEST_ASSERT_FALSE(fwu_image_head_parse(h, FWU_IMAGE_HEAD_LEN - 1, project,
                                           sizeof(project), version, sizeof(version)));
    TEST_ASSERT_FALSE(fwu_image_head_parse(NULL, sizeof(h), project, sizeof(project),
                                           version, sizeof(version)));
}
