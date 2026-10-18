/*
 * This file is part of the MeatPi components project.
 *
 * Copyright (C) 2022-2026 MeatPi Electronics.
 * Written by Ali Slim <ali@meatpi.com>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file test_brake_core.c
 * @brief Host tests of the crash-loop brake's pure half: which resets count,
 *        when the streak grows and when it ends, what a park that ends to
 *        try again leaves behind, and that the count outlives the tracker's
 *        PSRAM history.
 */
#include <stddef.h>
#include <string.h>

#include "unity.h"

#include "restart_tracker_private.h"

#define RST_SW    3U
#define RST_PANIC 4U

static rt_brake_state_t s_brake;
static restart_tracker_brake_t s_out;

/** One boot whose run before ended in a crash (`true`) or any other way. */
static uint8_t boot(bool crash)
{
    rt_brake_boot(&s_brake, crash, 0, &s_out);
    TEST_ASSERT_TRUE(rt_brake_valid(&s_brake));
    return s_out.verdict;
}

/** Three quick crashes: the device is parked and says so, as main does. */
static void park(void)
{
    rt_brake_reset(&s_brake);
    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BOOT_NORMAL, boot(true));
    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BOOT_NORMAL, boot(true));
    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BOOT_PARK, boot(true));
    rt_brake_set_mode(&s_brake, RESTART_TRACKER_BOOT_PARK);
}

void test_brake_layout_and_validity(void)
{
    TEST_ASSERT_EQUAL_UINT32(12, sizeof(rt_brake_state_t));
    TEST_ASSERT_EQUAL_UINT32(8, offsetof(rt_brake_state_t, crc));
    TEST_ASSERT_EQUAL_UINT32(8, offsetof(rt_crash_store_t, brake));
    TEST_ASSERT_EQUAL_UINT32(20, offsetof(rt_crash_store_t, pending));

    memset(&s_brake, 0xA5, sizeof(s_brake)); /* power-on garbage */
    TEST_ASSERT_FALSE(rt_brake_valid(&s_brake));
    memset(&s_brake, 0, sizeof(s_brake)); /* all zero is no state either */
    TEST_ASSERT_FALSE(rt_brake_valid(&s_brake));

    rt_brake_reset(&s_brake);
    TEST_ASSERT_TRUE(rt_brake_valid(&s_brake));
    TEST_ASSERT_EQUAL_UINT8(0, s_brake.streak);
    TEST_ASSERT_EQUAL_UINT8(RT_REPORT_BUDGET, s_brake.budget);

    s_brake.streak ^= 0x02U; /* scribbled on */
    TEST_ASSERT_FALSE(rt_brake_valid(&s_brake));
}

void test_brake_which_resets_are_crashes(void)
{
    static const uint32_t CRASH[] = { 4, 5, 6, 7, 15 };
    static const uint32_t OTHER[] = { 0, 1, 2, 3, 8, 9, 10, 11, 12, 13, 14, 16 };

    for (size_t i = 0; i < sizeof(CRASH) / sizeof(CRASH[0]); i++)
    {
        TEST_ASSERT_TRUE(rt_reset_reason_is_crash(CRASH[i]));
    }

    for (size_t i = 0; i < sizeof(OTHER) / sizeof(OTHER[0]); i++)
    {
        TEST_ASSERT_FALSE(rt_reset_reason_is_crash(OTHER[i]));
    }
}

void test_brake_three_quick_crashes_park(void)
{
    rt_brake_reset(&s_brake);
    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BOOT_NORMAL, boot(false));
    TEST_ASSERT_EQUAL_UINT8(0, s_out.streak);

    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BOOT_NORMAL, boot(true));
    TEST_ASSERT_EQUAL_UINT8(1, s_out.streak);
    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BOOT_NORMAL, boot(true));
    TEST_ASSERT_EQUAL_UINT8(2, s_out.streak);
    TEST_ASSERT_EQUAL_UINT8(0, s_out.parks);

    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BOOT_PARK, boot(true));
    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BRAKE_STREAK, s_out.streak);
    TEST_ASSERT_EQUAL_UINT8(1, s_out.parks);
    TEST_ASSERT_EQUAL_UINT32(0, s_out.retry_after_s); /* parked for good */
}

void test_brake_a_settled_run_ends_the_streak(void)
{
    rt_brake_reset(&s_brake);
    boot(true);
    boot(true);
    TEST_ASSERT_EQUAL_UINT8(2, s_out.streak);

    rt_brake_settle(&s_brake); /* this run stayed up */
    TEST_ASSERT_TRUE(rt_brake_valid(&s_brake));
    TEST_ASSERT_EQUAL_UINT8(0, s_brake.streak);

    /* the settled run crashes after all: one crash of a healthy run */
    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BOOT_NORMAL, boot(true));
    TEST_ASSERT_EQUAL_UINT8(0, s_out.streak);

    /* from here it takes three quick ones again */
    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BOOT_NORMAL, boot(true));
    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BOOT_NORMAL, boot(true));
    TEST_ASSERT_EQUAL_UINT8(2, s_out.streak);
    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BOOT_PARK, boot(true));
}

void test_brake_any_other_reset_ends_the_streak(void)
{
    rt_brake_reset(&s_brake);
    boot(true);
    boot(true);

    /* a planned restart, a wake, the EN pin, a brown-out */
    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BOOT_NORMAL, boot(false));
    TEST_ASSERT_EQUAL_UINT8(0, s_out.streak);

    boot(true);
    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BOOT_NORMAL, boot(true));
    TEST_ASSERT_EQUAL_UINT8(2, s_out.streak);
}

void test_brake_a_park_that_retries_keeps_the_streak(void)
{
    park();
    rt_brake_mark_retry(&s_brake); /* the park's timer, or the button */
    TEST_ASSERT_TRUE(rt_brake_valid(&s_brake));

    /* the try: a normal boot, the streak still there */
    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BOOT_NORMAL, boot(false));
    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BRAKE_STREAK, s_out.streak);
    TEST_ASSERT_EQUAL_UINT8(1, s_out.parks);

    /* it crashes again before settling: parked at once, not after three */
    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BOOT_PARK, boot(true));
    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BRAKE_STREAK + 1U, s_out.streak);
    TEST_ASSERT_EQUAL_UINT8(2, s_out.parks);
}

void test_brake_a_retry_that_settles_is_healthy_again(void)
{
    park();
    rt_brake_mark_retry(&s_brake);
    boot(false);
    rt_brake_settle(&s_brake);
    TEST_ASSERT_EQUAL_UINT8(0, s_brake.streak);
    TEST_ASSERT_EQUAL_UINT8(0, s_brake.parks);

    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BOOT_NORMAL, boot(true));
    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BOOT_NORMAL, boot(true));
    TEST_ASSERT_EQUAL_UINT8(1, s_out.streak); /* the first was the settled run's */
}

void test_brake_a_park_ended_from_outside_starts_over(void)
{
    /* no retry mark: someone reset a parked device (a flasher, the EN pin) */
    park();
    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BOOT_NORMAL, boot(false));
    TEST_ASSERT_EQUAL_UINT8(0, s_out.streak);
    TEST_ASSERT_EQUAL_UINT8(0, s_out.parks);
}

void test_brake_a_crash_inside_the_park_parks_bare(void)
{
    park();
    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BOOT_PARK_BARE, boot(true));
    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BRAKE_STREAK, s_out.streak);
    TEST_ASSERT_EQUAL_UINT8(2, s_out.parks);

    /* and a crash of the bare park stays bare: there is no less to do */
    rt_brake_set_mode(&s_brake, RESTART_TRACKER_BOOT_PARK_BARE);
    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BOOT_PARK_BARE, boot(true));

    /* a bare park that retries is a try like any other */
    rt_brake_set_mode(&s_brake, RESTART_TRACKER_BOOT_PARK_BARE);
    rt_brake_mark_retry(&s_brake);
    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BOOT_NORMAL, boot(false));
    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BOOT_PARK, boot(true));
}

void test_brake_safe_mode_is_not_braked(void)
{
    rt_brake_reset(&s_brake);
    boot(true);
    boot(true);
    rt_brake_set_mode(&s_brake, RESTART_TRACKER_BOOT_SAFE);

    /* safe mode crashing is not the firmware's loop */
    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BOOT_NORMAL, boot(true));
    TEST_ASSERT_EQUAL_UINT8(0, s_out.streak);

    /* and leaving safe mode by its own restart starts over too */
    boot(true);
    boot(true);
    rt_brake_set_mode(&s_brake, RESTART_TRACKER_BOOT_SAFE);
    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BOOT_NORMAL, boot(false));
    TEST_ASSERT_EQUAL_UINT8(0, s_out.streak);

    /* a mode that does not exist is recorded as a normal run */
    rt_brake_set_mode(&s_brake, 99);
    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BOOT_NORMAL, s_brake.run_mode);
    TEST_ASSERT_TRUE(rt_brake_valid(&s_brake));
}

void test_brake_retry_time(void)
{
    /* the product's own retry, when it has one */
    rt_brake_reset(&s_brake);
    rt_brake_boot(&s_brake, true, 21600, &s_out);
    TEST_ASSERT_EQUAL_UINT32(0, s_out.retry_after_s); /* not parked: none */
    rt_brake_boot(&s_brake, true, 21600, &s_out);
    rt_brake_boot(&s_brake, true, 21600, &s_out);
    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BOOT_PARK, s_out.verdict);
    TEST_ASSERT_EQUAL_UINT32(21600, s_out.retry_after_s);

    /* the bench's knob: kept over the boots that do not park, used by the
       first park, gone for the second */
    rt_brake_reset(&s_brake);
    rt_brake_set_test_retry(&s_brake, 30);
    TEST_ASSERT_TRUE(rt_brake_valid(&s_brake));
    boot(true);
    boot(true);
    TEST_ASSERT_EQUAL_UINT16(30, s_brake.test_retry_s);
    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BOOT_PARK, boot(true));
    TEST_ASSERT_EQUAL_UINT32(30, s_out.retry_after_s);
    TEST_ASSERT_EQUAL_UINT16(0, s_brake.test_retry_s);

    rt_brake_set_mode(&s_brake, RESTART_TRACKER_BOOT_PARK);
    rt_brake_mark_retry(&s_brake);
    boot(false);
    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BOOT_PARK, boot(true));
    TEST_ASSERT_EQUAL_UINT32(0, s_out.retry_after_s);
}

void test_brake_report_budget(void)
{
    rt_brake_reset(&s_brake);

    for (uint8_t i = 0; i < RT_REPORT_BUDGET; i++)
    {
        TEST_ASSERT_TRUE(rt_brake_spend(&s_brake));
        TEST_ASSERT_TRUE(rt_brake_valid(&s_brake));
    }

    TEST_ASSERT_FALSE(rt_brake_spend(&s_brake));
    TEST_ASSERT_EQUAL_UINT8(0, s_brake.budget);

    /* boots do not give it back, crashes or not; a settled run does */
    boot(true);
    boot(false);
    TEST_ASSERT_FALSE(rt_brake_spend(&s_brake));
    rt_brake_settle(&s_brake);
    TEST_ASSERT_EQUAL_UINT8(RT_REPORT_BUDGET, s_brake.budget);
}

void test_brake_a_long_loop_stays_parked(void)
{
    /* 300 failed tries: the counters stop at their ceiling, the verdict
       never falls back to a normal boot */
    park();

    for (int i = 0; i < 300; i++)
    {
        rt_brake_set_mode(&s_brake, RESTART_TRACKER_BOOT_PARK);
        rt_brake_mark_retry(&s_brake);
        TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BOOT_NORMAL, boot(false));
        TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BOOT_PARK, boot(true));
    }

    TEST_ASSERT_EQUAL_UINT8(UINT8_MAX, s_out.streak);
    TEST_ASSERT_EQUAL_UINT8(UINT8_MAX, s_out.parks);
}

void test_brake_count_outlives_the_psram_history(void)
{
    static rt_crash_store_t store;

    rt_crash_store_reset(&store);
    TEST_ASSERT_TRUE(rt_brake_valid(&store.brake));
    rt_brake_boot(&store.brake, true, 0, &s_out);
    rt_brake_boot(&store.brake, true, 0, &s_out);
    TEST_ASSERT_EQUAL_UINT8(2, store.brake.streak);

    /* the tracker's PSRAM state came back invalid: its history starts
       again, the kept notes go, the count stays */
    store.kept[3].magic = RT_CRASH_NOTE_MAGIC;
    TEST_ASSERT_NULL(rt_crash_collect(&store, 1, 0, true));
    TEST_ASSERT_EQUAL_UINT32(0, store.kept[3].magic);
    TEST_ASSERT_EQUAL_UINT8(2, store.brake.streak);
    rt_brake_boot(&store.brake, true, 0, &s_out);
    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BOOT_PARK, s_out.verdict);

    /* a count that was scribbled on starts from zero, the notes stay */
    store.kept[2].magic = RT_CRASH_NOTE_MAGIC;
    store.brake.budget = 200;
    TEST_ASSERT_NULL(rt_crash_collect(&store, 2, 1, false));
    TEST_ASSERT_TRUE(rt_brake_valid(&store.brake));
    TEST_ASSERT_EQUAL_UINT8(0, store.brake.streak);
    TEST_ASSERT_EQUAL_UINT8(RT_REPORT_BUDGET, store.brake.budget);
    TEST_ASSERT_EQUAL_UINT32(RT_CRASH_NOTE_MAGIC, store.kept[2].magic);

    /* a store that is not ours (power-on, another layout) starts whole */
    rt_brake_boot(&store.brake, true, 0, &s_out);
    store.version = (uint16_t)(RT_CRASH_VERSION - 1U);
    TEST_ASSERT_NULL(rt_crash_collect(&store, 3, 2, false));
    TEST_ASSERT_TRUE(rt_crash_store_valid(&store));
    TEST_ASSERT_EQUAL_UINT8(0, store.brake.streak);
    TEST_ASSERT_EQUAL_UINT32(0, store.kept[2].magic);
}

void test_brake_record_fields_and_names(void)
{
    static restart_tracker_state_t state;
    rt_inputs_t in = { .now_unix = 1767225600LL, .reset_reason = RST_PANIC };

    /* no record yet: nothing to write, nothing invented */
    memset(&state, 0xA5, sizeof(state));
    rt_record_set_mode(&state, RESTART_TRACKER_BOOT_PARK);
    rt_record_set_settled(&state);
    TEST_ASSERT_FALSE(rt_state_is_valid(&state));

    rt_record_boot(&state, &in);
    in.reset_reason = RST_SW;
    rt_record_boot(&state, &in);

    const restart_tracker_record_t *now =
        &state.history[state.latest_history_index];
    const restart_tracker_record_t *before = &state.history[0];

    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BOOT_NORMAL, now->boot_mode);
    TEST_ASSERT_EQUAL_UINT8(0, now->settled);

    rt_record_set_mode(&state, RESTART_TRACKER_BOOT_PARK);
    rt_record_set_settled(&state);
    TEST_ASSERT_TRUE(rt_state_is_valid(&state)); /* the CRC went along */
    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BOOT_PARK, now->boot_mode);
    TEST_ASSERT_EQUAL_UINT8(1, now->settled);
    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_BOOT_NORMAL, before->boot_mode);
    TEST_ASSERT_EQUAL_UINT8(0, before->settled);

    /* a new boot's record starts clean */
    rt_record_boot(&state, &in);
    now = &state.history[state.latest_history_index];
    TEST_ASSERT_EQUAL_UINT8(0, now->boot_mode);
    TEST_ASSERT_EQUAL_UINT8(0, now->settled);

    TEST_ASSERT_EQUAL_STRING("normal", restart_tracker_boot_mode_to_str(0));
    TEST_ASSERT_EQUAL_STRING("park", restart_tracker_boot_mode_to_str(1));
    TEST_ASSERT_EQUAL_STRING("park_bare", restart_tracker_boot_mode_to_str(2));
    TEST_ASSERT_EQUAL_STRING("safe", restart_tracker_boot_mode_to_str(3));
    TEST_ASSERT_EQUAL_STRING("invalid", restart_tracker_boot_mode_to_str(4));
    TEST_ASSERT_EQUAL_STRING(
        "park_retry", restart_tracker_planned_reason_to_str(
                          RESTART_TRACKER_PLANNED_REASON_PARK_RETRY));
    TEST_ASSERT_EQUAL_STRING(
        "partition_migrate", restart_tracker_planned_reason_to_str(
                                 RESTART_TRACKER_PLANNED_REASON_PARTITION_MIGRATE));
    /* fw_update_record.h keys the boot decision on the numeric values */
    TEST_ASSERT_EQUAL(11, RESTART_TRACKER_PLANNED_REASON_PARTITION_MIGRATE);
    TEST_ASSERT_EQUAL(12, RESTART_TRACKER_PLANNED_REASON_OTA_ROLLBACK);
    TEST_ASSERT_EQUAL(4, RESTART_TRACKER_PLANNED_REASON_OTA_APPLY);
    TEST_ASSERT_EQUAL(6, RESTART_TRACKER_PLANNED_REASON_SAFE_MODE);
    TEST_ASSERT_EQUAL(10, RESTART_TRACKER_PLANNED_REASON_PARK_RETRY);
    TEST_ASSERT_EQUAL_STRING(
        "ota_rollback", restart_tracker_planned_reason_to_str(
                            RESTART_TRACKER_PLANNED_REASON_OTA_ROLLBACK));
    TEST_ASSERT_EQUAL_STRING(
        "boot", restart_tracker_source_to_str(RESTART_TRACKER_SOURCE_BOOT));
    TEST_ASSERT_EQUAL_STRING(
        "park", restart_tracker_source_to_str(RESTART_TRACKER_SOURCE_PARK));
}
