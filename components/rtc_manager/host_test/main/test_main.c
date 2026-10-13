/**
 * @file test_main.c
 * @brief Host tests for rtc_manager's pure parts: the RX8130 time codec
 *        (BCD round-trip, plausibility gating: a fresh board / drained
 *        caps must NOT restore the clock), the civil-to-epoch conversion
 *        the boot restore uses instead of mktime, and the clock-step
 *        bookkeeping (the measured step of every set, the kept table, the
 *        catch-up by generation). Expected: 12 Tests 0 Failures 0 Ignored.
 */
#include <string.h>

#include "unity.h"

#include "rtc_manager_private.h"

void setUp(void)
{
}

void tearDown(void)
{
}

static void test_decode_known_time(void)
{
    /* 2026-07-04 15:42:37 UTC, weekday one-hot (Saturday) */
    const uint8_t regs[RTC_REGS_LEN] =
        { 0x37, 0x42, 0x15, 0x40, 0x04, 0x07, 0x26 };
    struct tm t;

    TEST_ASSERT_TRUE(rtc_time_decode(regs, &t));
    TEST_ASSERT_EQUAL_INT(37, t.tm_sec);
    TEST_ASSERT_EQUAL_INT(42, t.tm_min);
    TEST_ASSERT_EQUAL_INT(15, t.tm_hour);
    TEST_ASSERT_EQUAL_INT(4, t.tm_mday);
    TEST_ASSERT_EQUAL_INT(6, t.tm_mon);       /* July, 0-based */
    TEST_ASSERT_EQUAL_INT(126, t.tm_year);    /* 2026          */
}

static void test_encode_round_trip(void)
{
    struct tm in =
    {
        .tm_sec = 59, .tm_min = 8, .tm_hour = 23, .tm_mday = 31,
        .tm_mon = 11, .tm_year = 199, .tm_wday = 3, /* 2099-12-31 */
    };
    uint8_t regs[RTC_REGS_LEN];
    struct tm out;

    rtc_time_encode(&in, regs);
    TEST_ASSERT_EQUAL_HEX8(0x59, regs[0]);
    TEST_ASSERT_EQUAL_HEX8(0x23, regs[2]);
    TEST_ASSERT_EQUAL_HEX8(1 << 3, regs[3]); /* one-hot weekday */
    TEST_ASSERT_EQUAL_HEX8(0x31, regs[4]);
    TEST_ASSERT_EQUAL_HEX8(0x12, regs[5]);
    TEST_ASSERT_EQUAL_HEX8(0x99, regs[6]);
    TEST_ASSERT_TRUE(rtc_time_decode(regs, &out));
    TEST_ASSERT_EQUAL_INT(in.tm_sec, out.tm_sec);
    TEST_ASSERT_EQUAL_INT(in.tm_mday, out.tm_mday);
    TEST_ASSERT_EQUAL_INT(in.tm_year, out.tm_year);
}

static void test_fresh_board_rejected(void)
{
    /* power-on reset value: 2000-01-01 00:00:00, year 00 < 20 */
    const uint8_t regs[RTC_REGS_LEN] =
        { 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x00 };
    struct tm t;

    TEST_ASSERT_FALSE(rtc_time_decode(regs, &t));
}

static void test_garbage_rejected(void)
{
    const uint8_t all_ff[RTC_REGS_LEN] =
        { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    const uint8_t bad_month[RTC_REGS_LEN] =
        { 0x00, 0x00, 0x12, 0x01, 0x15, 0x13, 0x26 };
    const uint8_t bad_day[RTC_REGS_LEN] =
        { 0x00, 0x00, 0x12, 0x01, 0x00, 0x07, 0x26 };
    struct tm t;

    TEST_ASSERT_FALSE(rtc_time_decode(all_ff, &t));
    TEST_ASSERT_FALSE(rtc_time_decode(bad_month, &t));
    TEST_ASSERT_FALSE(rtc_time_decode(bad_day, &t));
}

static void test_status_bits_masked(void)
{
    /* RX8130 seconds/minutes carry a status bit in bit7: must be masked */
    const uint8_t regs[RTC_REGS_LEN] =
        { 0x80 | 0x30, 0x80 | 0x15, 0x12, 0x01, 0x04, 0x07, 0x26 };
    struct tm t;

    TEST_ASSERT_TRUE(rtc_time_decode(regs, &t));
    TEST_ASSERT_EQUAL_INT(30, t.tm_sec);
    TEST_ASSERT_EQUAL_INT(15, t.tm_min);
}

/* ---- the civil-to-epoch conversion (replaces mktime at the restore) ------ */

static int64_t epoch_of(int y, int mo, int d, int h, int mi, int s)
{
    struct tm t =
    {
        .tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d,
        .tm_hour = h, .tm_min = mi, .tm_sec = s,
    };

    return rtc_time_to_epoch(&t);
}

static void test_epoch_vectors(void)
{
    /* python: calendar.timegm(...) */
    TEST_ASSERT_EQUAL_INT64(0, epoch_of(1970, 1, 1, 0, 0, 0));
    TEST_ASSERT_EQUAL_INT64(1577836800LL, epoch_of(2020, 1, 1, 0, 0, 0));
    TEST_ASSERT_EQUAL_INT64(1709208000LL, epoch_of(2024, 2, 29, 12, 0, 0));
    TEST_ASSERT_EQUAL_INT64(1783179757LL, epoch_of(2026, 7, 4, 15, 42, 37));
    TEST_ASSERT_EQUAL_INT64(1791880680LL, epoch_of(2026, 10, 13, 8, 38, 0));
    TEST_ASSERT_EQUAL_INT64(2147483648LL, epoch_of(2038, 1, 19, 3, 14, 8));
    TEST_ASSERT_EQUAL_INT64(4102444799LL, epoch_of(2099, 12, 31, 23, 59, 59));
    /* the day before and after a leap day are 86400 s apart */
    TEST_ASSERT_EQUAL_INT64(86400, epoch_of(2024, 3, 1, 0, 0, 0)
                                   - epoch_of(2024, 2, 29, 0, 0, 0));
}

static void test_epoch_from_the_chip_codec(void)
{
    /* the register image of 2026-07-04 15:42:37 goes through the decoder
     * and lands on the same epoch, weekday register ignored */
    const uint8_t regs[RTC_REGS_LEN] =
        { 0x37, 0x42, 0x15, 0x40, 0x04, 0x07, 0x26 };
    struct tm t;

    TEST_ASSERT_TRUE(rtc_time_decode(regs, &t));
    TEST_ASSERT_EQUAL_INT64(1783179757LL, rtc_time_to_epoch(&t));
}

/* ---- the clock-step bookkeeping --------------------------------------------- */

#define WALL_2026 1791880680000LL   /* 2026-10-13T08:38:00Z in ms */

static void test_clock_first_set_is_measured_from_the_baseline(void)
{
    rtcm_clock_t c;
    rtc_clock_state_t st;
    rtc_clock_step_t step;

    /* boot: the clock reads 1970 + uptime, so wall == mono */
    rtcm_clock_init(&c, 5000, 5000);
    rtcm_clock_state(&c, &st);
    TEST_ASSERT_FALSE(st.valid);
    TEST_ASSERT_EQUAL_UINT8(RTC_CLOCK_SRC_NONE, st.source);
    TEST_ASSERT_EQUAL_UINT32(0, st.generation);
    TEST_ASSERT_EQUAL_UINT32(0, st.steps_this_boot);

    /* 1 s later SNTP lands: the wall jumps to 2026 */
    rtcm_clock_note(&c, WALL_2026 + 6000, 6000, RTC_CLOCK_SRC_SNTP, &step);
    TEST_ASSERT_EQUAL_UINT32(1, step.generation);
    TEST_ASSERT_EQUAL_UINT32(6000, step.mono_ms);
    TEST_ASSERT_EQUAL_INT64(WALL_2026, step.delta_ms);
    TEST_ASSERT_EQUAL_INT64(WALL_2026 + 6000, step.wall_ms);
    TEST_ASSERT_EQUAL_UINT8(RTC_CLOCK_SRC_SNTP, step.source);
    rtcm_clock_state(&c, &st);
    TEST_ASSERT_TRUE(st.valid);
    TEST_ASSERT_EQUAL_UINT8(RTC_CLOCK_SRC_SNTP, st.source);
    TEST_ASSERT_EQUAL_UINT32(1, st.generation);
    TEST_ASSERT_EQUAL_UINT32(1, st.steps_this_boot);
    TEST_ASSERT_EQUAL_INT64(WALL_2026, st.last.delta_ms);
}

static void test_clock_later_steps_measure_only_the_change(void)
{
    rtcm_clock_t c;
    rtc_clock_step_t step;

    /* a warm reset carried the clock: wall is already 2026 at init, so
     * the carried time is the baseline and the restore is a small step */
    rtcm_clock_init(&c, WALL_2026 + 100, 100);
    rtcm_clock_note(&c, WALL_2026 + 900 - 700, 900, RTC_CLOCK_SRC_RTC, &step);
    TEST_ASSERT_EQUAL_INT64(-700, step.delta_ms);  /* the chip's second lag */
    /* SNTP corrects 2.5 s backwards */
    rtcm_clock_note(&c, WALL_2026 + 20000 - 700 - 2500, 20000,
                    RTC_CLOCK_SRC_SNTP, &step);
    TEST_ASSERT_EQUAL_INT64(-2500, step.delta_ms);
    TEST_ASSERT_EQUAL_UINT32(2, step.generation);
    /* a set that moves nothing is still a step (generation bumps) */
    rtcm_clock_note(&c, WALL_2026 + 30000 - 700 - 2500, 30000,
                    RTC_CLOCK_SRC_MANUAL, &step);
    TEST_ASSERT_EQUAL_INT64(0, step.delta_ms);
    TEST_ASSERT_EQUAL_UINT32(3, step.generation);
    TEST_ASSERT_EQUAL_UINT8(RTC_CLOCK_SRC_MANUAL, step.source);
}

static void test_clock_mono_past_32_bits_is_no_step(void)
{
    rtcm_clock_t c;
    rtc_clock_step_t step;
    int64_t mono = 5000000000LL;   /* 57 days up: past the uint32 wrap */

    rtcm_clock_init(&c, WALL_2026, 0);
    rtcm_clock_note(&c, WALL_2026 + mono, mono, RTC_CLOCK_SRC_SNTP, &step);
    TEST_ASSERT_EQUAL_INT64(0, step.delta_ms);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)mono, step.mono_ms); /* truncated */
}

static void test_clock_table_keeps_the_last_eight(void)
{
    rtcm_clock_t c;
    rtc_clock_step_t out[RTCM_STEPS_KEEP + 2];
    rtc_clock_state_t st;

    rtcm_clock_init(&c, 0, 0);

    for (int i = 1; i <= 9; i++)
    {
        /* every step moves the wall by i seconds */
        rtcm_clock_note(&c, (int64_t)i * 1000 * (i + 1) / 2 + i * 10,
                        i * 10, RTC_CLOCK_SRC_SNTP, NULL);
    }

    rtcm_clock_state(&c, &st);
    TEST_ASSERT_EQUAL_UINT32(9, st.steps_this_boot);
    TEST_ASSERT_EQUAL_UINT32(9, st.generation);
    TEST_ASSERT_EQUAL_INT64(9000, st.last.delta_ms);
    /* since 0: the oldest (generation 1) was evicted, 2..9 remain in order */
    int n = rtcm_clock_steps_since(&c, 0, out, RTCM_STEPS_KEEP + 2);

    TEST_ASSERT_EQUAL_INT(8, n);
    TEST_ASSERT_EQUAL_UINT32(2, out[0].generation);
    TEST_ASSERT_EQUAL_UINT32(9, out[7].generation);
    TEST_ASSERT_EQUAL_INT64(2000, out[0].delta_ms);
    /* a consumer that saw generation 7 gets 8 and 9 */
    n = rtcm_clock_steps_since(&c, 7, out, RTCM_STEPS_KEEP);
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_UINT32(8, out[0].generation);
    TEST_ASSERT_EQUAL_UINT32(9, out[1].generation);
    /* caught up: nothing */
    TEST_ASSERT_EQUAL_INT(0, rtcm_clock_steps_since(&c, 9, out, 8));
    /* a cap smaller than the backlog returns the oldest ones first */
    n = rtcm_clock_steps_since(&c, 0, out, 3);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_UINT32(2, out[0].generation);
    TEST_ASSERT_EQUAL_UINT32(4, out[2].generation);
}

static void test_clock_steps_since_on_a_fresh_table(void)
{
    rtcm_clock_t c;
    rtc_clock_step_t out[2];

    rtcm_clock_init(&c, 0, 0);
    TEST_ASSERT_EQUAL_INT(0, rtcm_clock_steps_since(&c, 0, out, 2));
    rtcm_clock_note(&c, 1000, 10, RTC_CLOCK_SRC_RTC, NULL);
    TEST_ASSERT_EQUAL_INT(1, rtcm_clock_steps_since(&c, 0, out, 2));
    TEST_ASSERT_EQUAL_UINT8(RTC_CLOCK_SRC_RTC, out[0].source);
    TEST_ASSERT_EQUAL_INT64(990, out[0].delta_ms);
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_decode_known_time);
    RUN_TEST(test_encode_round_trip);
    RUN_TEST(test_fresh_board_rejected);
    RUN_TEST(test_garbage_rejected);
    RUN_TEST(test_status_bits_masked);
    RUN_TEST(test_epoch_vectors);
    RUN_TEST(test_epoch_from_the_chip_codec);
    RUN_TEST(test_clock_first_set_is_measured_from_the_baseline);
    RUN_TEST(test_clock_later_steps_measure_only_the_change);
    RUN_TEST(test_clock_mono_past_32_bits_is_no_step);
    RUN_TEST(test_clock_table_keeps_the_last_eight);
    RUN_TEST(test_clock_steps_since_on_a_fresh_table);
    UNITY_END();
}
