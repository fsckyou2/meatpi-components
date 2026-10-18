/**
 * @file test_session.c
 * @brief Host tests for the pure OTA session state machine: every
 *        transition rule, error latching, and backend-call accounting,
 *        against a recorder backend (no esp_ota).
 */
#include <string.h>

#include "unity.h"

#include "ota_manager_private.h"

/* ---- recorder backend ----------------------------------------------------------- */

typedef struct
{
    int begins;
    int writes;
    int ends;
    int aborts;
    size_t bytes;
    esp_err_t begin_rc;
    esp_err_t write_rc;
    esp_err_t end_rc;
} rec_t;

static rec_t s_rec;

static esp_err_t rec_begin(void *io, size_t total)
{
    (void)io;
    (void)total;
    s_rec.begins++;
    return s_rec.begin_rc;
}

static esp_err_t rec_write(void *io, const void *d, size_t l)
{
    (void)io;
    (void)d;
    s_rec.writes++;
    s_rec.bytes += l;
    return s_rec.write_rc;
}

static esp_err_t rec_end(void *io)
{
    (void)io;
    s_rec.ends++;
    return s_rec.end_rc;
}

static void rec_abort(void *io)
{
    (void)io;
    s_rec.aborts++;
}

static const ota_session_ops_t REC_OPS =
{
    .begin = rec_begin,
    .write = rec_write,
    .end = rec_end,
    .abort = rec_abort,
};

static ota_session_t s_s;

static void fresh(void)
{
    memset(&s_rec, 0, sizeof(s_rec));
    ota_session_init(&s_s, &REC_OPS, NULL);
}

/* ---- tests ------------------------------------------------------------------------ */

static void test_happy_path_known_size(void)
{
    fresh();
    TEST_ASSERT_EQUAL(ESP_OK, ota_session_begin(&s_s, 8));
    TEST_ASSERT_EQUAL(OTA_MANAGER_RECEIVING, s_s.state);
    TEST_ASSERT_EQUAL(ESP_OK, ota_session_write(&s_s, "abcd", 4));
    TEST_ASSERT_EQUAL(ESP_OK, ota_session_write(&s_s, "efgh", 4));
    TEST_ASSERT_EQUAL(ESP_OK, ota_session_end(&s_s));
    TEST_ASSERT_EQUAL(OTA_MANAGER_READY, s_s.state);
    TEST_ASSERT_EQUAL(8, s_s.received);
    TEST_ASSERT_EQUAL(1, s_rec.ends);
    TEST_ASSERT_EQUAL(0, s_rec.aborts);
}

static void test_write_before_begin_rejected(void)
{
    fresh();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ota_session_write(&s_s, "x", 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ota_session_end(&s_s));
    TEST_ASSERT_EQUAL(0, s_rec.writes);
}

static void test_double_begin_rejected_while_receiving(void)
{
    fresh();
    ota_session_begin(&s_s, 0);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ota_session_begin(&s_s, 0));
    TEST_ASSERT_EQUAL(1, s_rec.begins); /* second never hit the backend */
}

static void test_overrun_of_announced_size_fails(void)
{
    fresh();
    ota_session_begin(&s_s, 4);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      ota_session_write(&s_s, "toolong", 7));
    TEST_ASSERT_EQUAL(OTA_MANAGER_FAILED, s_s.state);
    TEST_ASSERT_EQUAL(1, s_rec.aborts); /* flash session released */
    TEST_ASSERT_NOT_NULL(strstr(s_s.error, "larger"));
}

static void test_short_image_fails_at_end(void)
{
    fresh();
    ota_session_begin(&s_s, 100);
    ota_session_write(&s_s, "half", 4);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, ota_session_end(&s_s));
    TEST_ASSERT_EQUAL(OTA_MANAGER_FAILED, s_s.state);
    TEST_ASSERT_NOT_NULL(strstr(s_s.error, "short"));
    TEST_ASSERT_EQUAL(0, s_rec.ends); /* backend end never attempted */
}

static void test_empty_image_fails_at_end(void)
{
    fresh();
    ota_session_begin(&s_s, 0);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, ota_session_end(&s_s));
    TEST_ASSERT_NOT_NULL(strstr(s_s.error, "empty"));
}

static void test_backend_write_failure_latches_and_aborts(void)
{
    fresh();
    ota_session_begin(&s_s, 0);
    s_rec.write_rc = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, ota_session_write(&s_s, "x", 1));
    TEST_ASSERT_EQUAL(OTA_MANAGER_FAILED, s_s.state);
    TEST_ASSERT_EQUAL(1, s_rec.aborts);
}

static void test_validation_failure_no_double_abort(void)
{
    fresh();
    ota_session_begin(&s_s, 0);
    ota_session_write(&s_s, "data", 4);
    s_rec.end_rc = ESP_FAIL; /* esp_ota_end releases the handle itself */
    TEST_ASSERT_EQUAL(ESP_FAIL, ota_session_end(&s_s));
    TEST_ASSERT_EQUAL(OTA_MANAGER_FAILED, s_s.state);
    TEST_ASSERT_EQUAL(0, s_rec.aborts);
    TEST_ASSERT_NOT_NULL(strstr(s_s.error, "validation"));
}

static void test_retry_after_failure_and_abort_resets(void)
{
    fresh();
    ota_session_begin(&s_s, 0);
    s_rec.write_rc = ESP_FAIL;
    ota_session_write(&s_s, "x", 1); /* -> FAILED */
    s_rec.write_rc = ESP_OK;

    /* begin from FAILED = retry */
    TEST_ASSERT_EQUAL(ESP_OK, ota_session_begin(&s_s, 0));
    TEST_ASSERT_EQUAL(ESP_OK, ota_session_write(&s_s, "ok", 2));

    /* abort from RECEIVING -> IDLE + backend abort */
    TEST_ASSERT_EQUAL(ESP_OK, ota_session_abort(&s_s));
    TEST_ASSERT_EQUAL(OTA_MANAGER_IDLE, s_s.state);
    TEST_ASSERT_EQUAL(0, s_s.received);
    TEST_ASSERT_EQUAL(2, s_rec.aborts);
}

/* ---- the image head and the digest (2026-10-18) ------------------------------------ */

/* a fake digest: the byte sum in out[0..3], the count in out[4..7] */
static uint32_t s_hash_sum;
static uint32_t s_hash_n;

static void rec_hash_reset(void *io)
{
    (void)io;
    s_hash_sum = 0;
    s_hash_n = 0;
}

static void rec_hash_update(void *io, const void *d, size_t l)
{
    const uint8_t *p = d;

    (void)io;

    for (size_t i = 0; i < l; i++)
    {
        s_hash_sum += p[i];
    }

    s_hash_n += (uint32_t)l;
}

static void rec_hash_final(void *io, uint8_t out[32])
{
    (void)io;
    memset(out, 0, 32);
    memcpy(out, &s_hash_sum, 4);
    memcpy(out + 4, &s_hash_n, 4);
}

static const ota_session_ops_t HASH_OPS =
{
    .begin = rec_begin,
    .write = rec_write,
    .end = rec_end,
    .abort = rec_abort,
    .hash_reset = rec_hash_reset,
    .hash_update = rec_hash_update,
    .hash_final = rec_hash_final,
};

static const char *const WICAN_PREFIXES[] = { "wican-fw_obd_pro", "wican-pro" };

/** An image of @p total bytes: the header, the descriptor, then filler. */
static void make_image(uint8_t *img, size_t total, uint16_t chip,
                       uint32_t desc_magic, const char *name, const char *ver)
{
    memset(img, 0xA5, total);
    img[0] = 0xE9;
    img[12] = (uint8_t)chip;
    img[13] = (uint8_t)(chip >> 8);
    img[32] = (uint8_t)desc_magic;
    img[33] = (uint8_t)(desc_magic >> 8);
    img[34] = (uint8_t)(desc_magic >> 16);
    img[35] = (uint8_t)(desc_magic >> 24);
    memset(img + 48, 0, 64);
    strncpy((char *)img + 48, ver, 31);
    strncpy((char *)img + 80, name, 31);
}

static void fresh_checked(void)
{
    memset(&s_rec, 0, sizeof(s_rec));
    ota_session_init(&s_s, &HASH_OPS, NULL);
    ota_session_set_product(&s_s, "WiCAN PRO", WICAN_PREFIXES, 2,
                            OTA_SESSION_CHIP_ESP32S3);
}

/** The digest the fake would compute over @p img. */
static void expected_of(const uint8_t *img, size_t n, uint8_t out[32])
{
    rec_hash_reset(NULL);
    rec_hash_update(NULL, img, n);
    rec_hash_final(NULL, out);
}

static void test_head_accepts_our_names(void)
{
    uint8_t img[400];

    fresh_checked();
    make_image(img, sizeof(img), 0x0009, 0xABCD5432u, "wican-fw_obd_pro_v452p",
               "v4.52p");
    TEST_ASSERT_EQUAL(ESP_OK, ota_session_begin(&s_s, sizeof(img)));
    TEST_ASSERT_EQUAL(ESP_OK, ota_session_write(&s_s, img, sizeof(img)));
    TEST_ASSERT_EQUAL(ESP_OK, ota_session_end(&s_s));
    TEST_ASSERT_EQUAL(OTA_MANAGER_READY, s_s.state);
    TEST_ASSERT_EQUAL(sizeof(img), s_rec.bytes); /* the head reached the flash */
    TEST_ASSERT_EQUAL_STRING("wican-fw_obd_pro_v452p", s_s.project_name);
    TEST_ASSERT_EQUAL_STRING("v4.52p", s_s.version);

    fresh_checked();
    make_image(img, sizeof(img), 0x0009, 0xABCD5432u, "wican-pro_7", "7.0.0");
    ota_session_begin(&s_s, 0);
    TEST_ASSERT_EQUAL(ESP_OK, ota_session_write(&s_s, img, sizeof(img)));
    TEST_ASSERT_EQUAL(ESP_OK, ota_session_end(&s_s));

    /* the constant name of the new builds */
    fresh_checked();
    make_image(img, sizeof(img), 0x0009, 0xABCD5432u, "wican-fw_obd_pro", "6.1.0");
    ota_session_begin(&s_s, 0);
    TEST_ASSERT_EQUAL(ESP_OK, ota_session_write(&s_s, img, sizeof(img)));
    TEST_ASSERT_EQUAL(ESP_OK, ota_session_end(&s_s));
}

static void test_head_refuses_foreign_images(void)
{
    uint8_t img[400];

    /* the ESPNetLink's image */
    fresh_checked();
    make_image(img, sizeof(img), 0x0009, 0xABCD5432u, "espnetlink-fw_v122-61", "x");
    ota_session_begin(&s_s, sizeof(img));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE,
                      ota_session_write(&s_s, img, sizeof(img)));
    TEST_ASSERT_EQUAL(OTA_MANAGER_FAILED, s_s.state);
    TEST_ASSERT_EQUAL_STRING("not a WiCAN PRO image: espnetlink-fw_v122-61",
                             s_s.error);
    TEST_ASSERT_EQUAL(0, s_rec.writes); /* nothing reached the flash */
    TEST_ASSERT_EQUAL(1, s_rec.aborts);

    /* not an image at all */
    fresh_checked();
    make_image(img, sizeof(img), 0x0009, 0xABCD5432u, "wican-fw_obd_pro", "x");
    img[0] = 0x7F;
    ota_session_begin(&s_s, 0);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE,
                      ota_session_write(&s_s, img, sizeof(img)));
    TEST_ASSERT_EQUAL_STRING("not an application image", s_s.error);

    /* no descriptor */
    fresh_checked();
    make_image(img, sizeof(img), 0x0009, 0x11223344u, "wican-fw_obd_pro", "x");
    ota_session_begin(&s_s, 0);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE,
                      ota_session_write(&s_s, img, sizeof(img)));
    TEST_ASSERT_EQUAL_STRING("no application descriptor", s_s.error);

    /* another chip */
    fresh_checked();
    make_image(img, sizeof(img), 0x0000, 0xABCD5432u, "wican-fw_obd_pro", "x");
    ota_session_begin(&s_s, 0);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE,
                      ota_session_write(&s_s, img, sizeof(img)));
    TEST_ASSERT_EQUAL_STRING("an image for another chip (id 0x0000)", s_s.error);

    /* a retry after the refusal is a fresh session */
    TEST_ASSERT_EQUAL(ESP_OK, ota_session_begin(&s_s, 0));
    TEST_ASSERT_EQUAL(0, s_s.head_len);
    TEST_ASSERT_FALSE(s_s.head_checked);
}

static void test_head_with_one_byte_chunks(void)
{
    uint8_t img[300];

    fresh_checked();
    make_image(img, sizeof(img), 0x0009, 0xABCD5432u, "wican-fw_obd_pro", "6.1.0");
    ota_session_begin(&s_s, sizeof(img));

    for (size_t i = 0; i < sizeof(img); i++)
    {
        TEST_ASSERT_EQUAL(ESP_OK, ota_session_write(&s_s, img + i, 1));

        if (i < OTA_SESSION_HEAD_LEN - 1)
        {
            TEST_ASSERT_EQUAL(0, s_rec.writes);    /* held back */
        }
    }

    TEST_ASSERT_EQUAL(ESP_OK, ota_session_end(&s_s));
    TEST_ASSERT_EQUAL(sizeof(img), s_rec.bytes);
    TEST_ASSERT_EQUAL(sizeof(img), s_s.received);
}

static void test_image_shorter_than_the_head(void)
{
    uint8_t img[150]; /* the descriptor's fields fit, the head does not */

    fresh_checked();
    make_image(img, sizeof(img), 0x0009, 0xABCD5432u, "wican-fw_obd_pro", "x");
    ota_session_begin(&s_s, 0);
    TEST_ASSERT_EQUAL(ESP_OK, ota_session_write(&s_s, img, sizeof(img)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, ota_session_end(&s_s));
    TEST_ASSERT_EQUAL_STRING("short image (no application descriptor)", s_s.error);
    TEST_ASSERT_EQUAL(0, s_rec.ends);
    TEST_ASSERT_EQUAL(1, s_rec.aborts);
}

static void test_digest_match_mismatch_and_none(void)
{
    uint8_t img[400];
    uint8_t want[32];

    make_image(img, sizeof(img), 0x0009, 0xABCD5432u, "wican-fw_obd_pro", "6.1.0");
    expected_of(img, sizeof(img), want);

    /* a match: the image is activated */
    fresh_checked();
    TEST_ASSERT_EQUAL(ESP_OK, ota_session_begin_ex(&s_s, sizeof(img), want));
    TEST_ASSERT_EQUAL(ESP_OK, ota_session_write(&s_s, img, 100));
    TEST_ASSERT_EQUAL(ESP_OK, ota_session_write(&s_s, img + 100, 300));
    TEST_ASSERT_EQUAL(ESP_OK, ota_session_end(&s_s));
    TEST_ASSERT_EQUAL(OTA_MANAGER_READY, s_s.state);
    TEST_ASSERT_EQUAL(1, s_rec.ends);

    /* a mismatch: nothing is activated, the slot is scratch again */
    fresh_checked();
    want[0] ^= 0x01;
    TEST_ASSERT_EQUAL(ESP_OK, ota_session_begin_ex(&s_s, sizeof(img), want));
    TEST_ASSERT_EQUAL(ESP_OK, ota_session_write(&s_s, img, sizeof(img)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_CRC, ota_session_end(&s_s));
    TEST_ASSERT_EQUAL(OTA_MANAGER_FAILED, s_s.state);
    TEST_ASSERT_EQUAL_STRING("sha256 mismatch", s_s.error);
    TEST_ASSERT_EQUAL(0, s_rec.ends);
    TEST_ASSERT_EQUAL(1, s_rec.aborts);

    /* no expectation: no comparison */
    fresh_checked();
    TEST_ASSERT_EQUAL(ESP_OK, ota_session_begin(&s_s, 0));
    TEST_ASSERT_EQUAL(ESP_OK, ota_session_write(&s_s, img, sizeof(img)));
    TEST_ASSERT_EQUAL(ESP_OK, ota_session_end(&s_s));

    /* the expectation does not outlive its session */
    fresh_checked();
    TEST_ASSERT_EQUAL(ESP_OK, ota_session_begin_ex(&s_s, 0, want));
    ota_session_abort(&s_s);
    TEST_ASSERT_FALSE(s_s.expect_digest);
    TEST_ASSERT_EQUAL(ESP_OK, ota_session_begin(&s_s, 0));
    TEST_ASSERT_EQUAL(ESP_OK, ota_session_write(&s_s, img, sizeof(img)));
    TEST_ASSERT_EQUAL(ESP_OK, ota_session_end(&s_s));
}

static void test_without_a_product_everything_passes(void)
{
    /* a session that never named a product (the old contract) */
    fresh();
    TEST_ASSERT_EQUAL(ESP_OK, ota_session_begin(&s_s, 4));
    TEST_ASSERT_EQUAL(ESP_OK, ota_session_write(&s_s, "junk", 4));
    TEST_ASSERT_EQUAL(ESP_OK, ota_session_end(&s_s));
    TEST_ASSERT_EQUAL(OTA_MANAGER_READY, s_s.state);
}

void run_session_tests(void)
{
    RUN_TEST(test_happy_path_known_size);
    RUN_TEST(test_write_before_begin_rejected);
    RUN_TEST(test_double_begin_rejected_while_receiving);
    RUN_TEST(test_overrun_of_announced_size_fails);
    RUN_TEST(test_short_image_fails_at_end);
    RUN_TEST(test_empty_image_fails_at_end);
    RUN_TEST(test_backend_write_failure_latches_and_aborts);
    RUN_TEST(test_validation_failure_no_double_abort);
    RUN_TEST(test_retry_after_failure_and_abort_resets);
    RUN_TEST(test_head_accepts_our_names);
    RUN_TEST(test_head_refuses_foreign_images);
    RUN_TEST(test_head_with_one_byte_chunks);
    RUN_TEST(test_image_shorter_than_the_head);
    RUN_TEST(test_digest_match_mismatch_and_none);
    RUN_TEST(test_without_a_product_everything_passes);
}
