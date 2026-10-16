/**
 * @file test_autobaud.c
 * @brief Host suite for can_autobaud_core.c: the listen-before-talk policy
 *        of the native CAN node, replayed with scripted time and counters
 *        (frames = frames received, bad = receive errors). The numbers used
 *        for a wrong bitrate are the bench's (2026-10-02): thousands of
 *        receive errors per second and not one frame. Run from
 *        test_main.c's app_main.
 */
#include <string.h>

#include "unity.h"

#include "can_autobaud_core.h"

#define TICK_MS 25u /* how often the RX task steps the policy while listening */

static can_ab_t s_ab;
static uint32_t s_now, s_frames, s_bad;
static can_ab_action_t s_last;   /* the last action that asked for a change */
static int s_applies;            /* how many did                             */

static void start(const uint16_t *cands, uint8_t n, bool want_normal)
{
    can_ab_cfg_t cfg = { .n_candidates = n, .want_normal = want_normal };

    memcpy(cfg.candidates, cands, n * sizeof(uint16_t));
    s_applies = 0;
    s_last = can_ab_init(&s_ab, &cfg, s_now, s_frames, s_bad);
}

/** Advance @p ms with @p frames_per_tick / @p bad_per_tick arriving, the
 *  way the glue does: step, apply, mark. */
static void run(uint32_t ms, uint32_t frames_per_tick, uint32_t bad_per_tick)
{
    for (uint32_t t = 0; t < ms; t += TICK_MS)
    {
        s_now += TICK_MS;
        s_frames += frames_per_tick;
        s_bad += bad_per_tick;

        can_ab_action_t a = can_ab_step(&s_ab, s_now, s_frames, s_bad);

        if (a.apply)
        {
            s_last = a;
            s_applies++;
            can_ab_mark(&s_ab, s_now, s_frames, s_bad);
        }
    }
}

static const uint16_t FIXED_500[] = { 500 };
static const uint16_t AUTO_500_250[] = { 500, 250 };

static void reset_clock(void)
{
    s_now = 1000;
    s_frames = 0;
    s_bad = 0;
}

void test_ab_always_starts_listen_only(void)
{
    reset_clock();
    start(FIXED_500, 1, true);
    TEST_ASSERT_TRUE(s_last.apply);
    TEST_ASSERT_EQUAL_UINT16(500, s_last.baud_kbps);
    TEST_ASSERT_TRUE(s_last.listen_only); /* even when asked to talk */
    TEST_ASSERT_EQUAL(CAN_AB_LISTENING, s_ab.state);
    TEST_ASSERT_FALSE(s_ab.normal);
    TEST_ASSERT_EQUAL_STRING("listening", can_ab_state_name(&s_ab));

    start(AUTO_500_250, 2, true);
    TEST_ASSERT_TRUE(s_last.listen_only);
    TEST_ASSERT_EQUAL_STRING("detecting", can_ab_state_name(&s_ab));
}

void test_ab_frames_prove_the_bitrate_and_promote(void)
{
    reset_clock();
    start(FIXED_500, 1, true);
    run(25, 1, 0);                       /* one frame: not yet            */
    TEST_ASSERT_EQUAL(CAN_AB_LISTENING, s_ab.state);
    TEST_ASSERT_EQUAL(0, s_applies);
    run(25, 1, 0);                       /* the second one proves it      */
    TEST_ASSERT_EQUAL(CAN_AB_RUNNING, s_ab.state);
    TEST_ASSERT_EQUAL(1, s_applies);
    TEST_ASSERT_EQUAL_UINT16(500, s_last.baud_kbps);
    TEST_ASSERT_FALSE(s_last.listen_only);
    TEST_ASSERT_TRUE(s_ab.normal);
    TEST_ASSERT_TRUE(s_ab.verified);
    TEST_ASSERT_EQUAL_UINT16(500, s_ab.detected_kbps);
    TEST_ASSERT_EQUAL_STRING("running", can_ab_state_name(&s_ab));

    /* a slow bus proves it too: frames never expire */
    reset_clock();
    start(FIXED_500, 1, true);
    run(1500, 0, 0 /* nothing */);       /* (silent rule fires first)     */
    reset_clock();
    start(FIXED_500, 1, false);
    run(25, 1, 0);
    run(2000, 0, 0);
    TEST_ASSERT_EQUAL(CAN_AB_LISTENING, s_ab.state); /* one frame only    */
    run(25, 1, 0);
    TEST_ASSERT_EQUAL(CAN_AB_RUNNING, s_ab.state);
    TEST_ASSERT_TRUE(s_ab.verified);
}

void test_ab_listen_setting_never_gets_a_tx_pin(void)
{
    reset_clock();
    start(FIXED_500, 1, false);          /* settings: silent              */
    run(1000, 4, 0);
    TEST_ASSERT_EQUAL(CAN_AB_RUNNING, s_ab.state);
    TEST_ASSERT_EQUAL(0, s_applies);     /* stays as it started           */
    TEST_ASSERT_FALSE(s_ab.normal);
}

void test_ab_silent_bus_fixed_bitrate_goes_to_its_mode(void)
{
    reset_clock();
    start(FIXED_500, 1, true);
    run(275, 0, 0);
    TEST_ASSERT_EQUAL(CAN_AB_LISTENING, s_ab.state);
    TEST_ASSERT_EQUAL(0, s_applies);
    run(50, 0, 0);                       /* 300 ms of nothing             */
    TEST_ASSERT_EQUAL(CAN_AB_RUNNING, s_ab.state);
    TEST_ASSERT_EQUAL(1, s_applies);
    TEST_ASSERT_FALSE(s_last.listen_only);
    TEST_ASSERT_TRUE(s_ab.normal);
    TEST_ASSERT_FALSE(s_ab.verified);    /* silence proves nothing        */
    TEST_ASSERT_EQUAL_UINT16(0, s_ab.detected_kbps);
}

void test_ab_wrong_bitrate_never_talks(void)
{
    /* 250k bus, node at 500k: ~4000 receive errors/s, no frame */
    reset_clock();
    start(FIXED_500, 1, true);
    run(5000, 0, 100);
    TEST_ASSERT_EQUAL(CAN_AB_MISMATCH, s_ab.state);
    TEST_ASSERT_EQUAL(0, s_applies);     /* listen-only from start to end */
    TEST_ASSERT_FALSE(s_ab.normal);
    TEST_ASSERT_TRUE(s_ab.bitten);
    TEST_ASSERT_EQUAL_STRING("mismatch", can_ab_state_name(&s_ab));
}

void test_ab_silence_after_a_mismatch_does_not_promote(void)
{
    reset_clock();
    start(FIXED_500, 1, true);
    run(500, 0, 100);
    TEST_ASSERT_EQUAL(CAN_AB_MISMATCH, s_ab.state);

    /* the other-bitrate traffic stops (ignition off): listening again,
       but silence no longer makes this node talk */
    run(1100, 0, 0);
    TEST_ASSERT_EQUAL(CAN_AB_LISTENING, s_ab.state);
    run(10000, 0, 0);
    TEST_ASSERT_EQUAL(CAN_AB_LISTENING, s_ab.state);
    TEST_ASSERT_EQUAL(0, s_applies);
    TEST_ASSERT_FALSE(s_ab.normal);

    /* and it comes back at the wrong bitrate: mismatch again, no burst */
    run(500, 0, 100);
    TEST_ASSERT_EQUAL(CAN_AB_MISMATCH, s_ab.state);
    TEST_ASSERT_EQUAL(0, s_applies);
}

void test_ab_frames_after_a_mismatch_promote(void)
{
    reset_clock();
    start(FIXED_500, 1, true);
    run(500, 0, 100);
    TEST_ASSERT_EQUAL(CAN_AB_MISMATCH, s_ab.state);
    run(100, 2, 0);                      /* the bus is at our bitrate now */
    TEST_ASSERT_EQUAL(CAN_AB_RUNNING, s_ab.state);
    TEST_ASSERT_EQUAL(1, s_applies);
    TEST_ASSERT_FALSE(s_last.listen_only);
    TEST_ASSERT_TRUE(s_ab.verified);
}

void test_ab_watchdog_demotes_a_talking_node(void)
{
    /* silent at start, promoted; then the bus wakes up at another bitrate */
    reset_clock();
    start(FIXED_500, 1, true);
    run(400, 0, 0);
    TEST_ASSERT_TRUE(s_ab.normal);
    TEST_ASSERT_EQUAL(1, s_applies);

    run(25, 0, 100);                     /* at the FIRST look, not at the
                                            end of a watchdog window      */
    TEST_ASSERT_EQUAL(CAN_AB_MISMATCH, s_ab.state);
    TEST_ASSERT_EQUAL(2, s_applies);
    TEST_ASSERT_TRUE(s_last.listen_only);
    TEST_ASSERT_EQUAL_UINT16(500, s_last.baud_kbps);
    TEST_ASSERT_FALSE(s_ab.normal);
    TEST_ASSERT_EQUAL_UINT32(1, s_ab.demotions);

    /* a listen-only node that was running: state only, nothing to apply */
    reset_clock();
    start(FIXED_500, 1, false);
    run(100, 2, 0);
    TEST_ASSERT_EQUAL(CAN_AB_RUNNING, s_ab.state);
    run(300, 0, 100);
    TEST_ASSERT_EQUAL(CAN_AB_MISMATCH, s_ab.state);
    TEST_ASSERT_EQUAL(0, s_applies);
    TEST_ASSERT_EQUAL_UINT32(0, s_ab.demotions);
}

void test_ab_errors_beside_frames_are_not_a_mismatch(void)
{
    /* a noisy bus at the right bitrate: frames keep coming */
    reset_clock();
    start(FIXED_500, 1, true);
    run(100, 2, 0);
    TEST_ASSERT_TRUE(s_ab.normal);
    run(5000, 1, 40);
    TEST_ASSERT_EQUAL(CAN_AB_RUNNING, s_ab.state);
    TEST_ASSERT_TRUE(s_ab.normal);
    TEST_ASSERT_EQUAL(1, s_applies);
    TEST_ASSERT_EQUAL_UINT32(0, s_ab.demotions);
}

void test_ab_rare_glitches_never_add_up(void)
{
    /* a quiet bus with one stray error every 200 ms: 5 per span, never 16 */
    reset_clock();
    start(FIXED_500, 1, true);

    for (int i = 0; i < 100; i++)
    {
        run(175, 0, 0);
        run(25, 0, 1);
    }

    TEST_ASSERT_NOT_EQUAL(CAN_AB_MISMATCH, s_ab.state);
    TEST_ASSERT_FALSE(s_ab.bitten);
}

void test_ab_auto_walks_to_the_candidate_that_reads(void)
{
    reset_clock();
    start(AUTO_500_250, 2, true);
    run(25, 0, 100);                     /* 500 does not read this bus:   */
    TEST_ASSERT_EQUAL(0, s_applies);     /* said once is not enough       */
    run(25, 0, 100);                     /* twice is                      */
    TEST_ASSERT_EQUAL(1, s_applies);
    TEST_ASSERT_EQUAL_UINT16(250, s_last.baud_kbps);
    TEST_ASSERT_TRUE(s_last.listen_only);
    TEST_ASSERT_EQUAL_UINT16(250, can_ab_baud(&s_ab));
    TEST_ASSERT_EQUAL(CAN_AB_LISTENING, s_ab.state);

    run(100, 2, 0);                      /* 250 does                      */
    TEST_ASSERT_EQUAL(CAN_AB_RUNNING, s_ab.state);
    TEST_ASSERT_EQUAL(2, s_applies);
    TEST_ASSERT_EQUAL_UINT16(250, s_last.baud_kbps);
    TEST_ASSERT_FALSE(s_last.listen_only);
    TEST_ASSERT_EQUAL_UINT16(250, s_ab.detected_kbps);
    TEST_ASSERT_EQUAL_UINT32(1, s_ab.switches);
}

void test_ab_one_corrupted_frame_is_not_a_mismatch(void)
{
    /* bench 2026-10-03: a listening controller is error-passive, and one
       frame it mis-reads comes back as a cascade of 41 to 48 errors */

    /* a listen-only node that was running */
    reset_clock();
    start(FIXED_500, 1, false);
    run(100, 2, 0);
    TEST_ASSERT_EQUAL(CAN_AB_RUNNING, s_ab.state);
    run(300, 0, 0);                      /* a sparse bus: nothing for a
                                            while, so no frame stands
                                            beside what comes next        */
    run(25, 0, 45);                      /* the cascade                   */
    TEST_ASSERT_EQUAL(CAN_AB_RUNNING, s_ab.state);
    TEST_ASSERT_TRUE(s_ab.suspect);
    run(1000, 2, 0);                     /* the bus goes on               */
    TEST_ASSERT_EQUAL(CAN_AB_RUNNING, s_ab.state);
    TEST_ASSERT_TRUE(s_ab.verified);
    TEST_ASSERT_FALSE(s_ab.suspect);
    TEST_ASSERT_FALSE(s_ab.bitten);
    TEST_ASSERT_EQUAL(0, s_applies);

    /* the same one much later is the first helping again, not the second */
    run(300, 0, 0);
    run(25, 0, 45);
    TEST_ASSERT_EQUAL(CAN_AB_RUNNING, s_ab.state);
    run(100, 2, 0);
    TEST_ASSERT_FALSE(s_ab.bitten);

    /* during detection, at the right candidate, before any frame */
    reset_clock();
    start(AUTO_500_250, 2, true);
    run(25, 0, 45);
    run(50, 2, 0);
    TEST_ASSERT_EQUAL(CAN_AB_RUNNING, s_ab.state);
    TEST_ASSERT_EQUAL_UINT16(500, s_ab.detected_kbps);
    TEST_ASSERT_EQUAL_UINT32(0, s_ab.switches);

    /* two helpings and no frame between them: that is a mismatch */
    reset_clock();
    start(FIXED_500, 1, false);
    run(100, 2, 0);
    run(300, 0, 0);
    run(25, 0, 45);
    run(25, 0, 45);
    TEST_ASSERT_EQUAL(CAN_AB_MISMATCH, s_ab.state);
    TEST_ASSERT_FALSE(s_ab.verified);

    /* ... but not when they are one cascade seen in two looks: the error
       interrupt wakes the stepping task every CAN_AB_BAD_MIN errors */
    reset_clock();
    start(FIXED_500, 1, false);
    run(100, 2, 0);
    run(300, 0, 0);

    for (int i = 0; i < 5; i++)          /* 40 errors within 4 ms         */
    {
        s_now += 1;
        s_bad += CAN_AB_BAD_MIN;
        (void)can_ab_step(&s_ab, s_now, s_frames, s_bad);
        TEST_ASSERT_EQUAL(CAN_AB_RUNNING, s_ab.state);
    }

    run(100, 2, 0);
    TEST_ASSERT_EQUAL(CAN_AB_RUNNING, s_ab.state);
    TEST_ASSERT_FALSE(s_ab.suspect);

    /* errors that go on past CAN_AB_CONFIRM_MS are the wrong bitrate */
    run(300, 0, 0);

    for (int i = 0; i < 12; i++)
    {
        s_now += 1;
        s_bad += CAN_AB_BAD_MIN;
        (void)can_ab_step(&s_ab, s_now, s_frames, s_bad);
    }

    TEST_ASSERT_EQUAL(CAN_AB_MISMATCH, s_ab.state);
}

void test_ab_frames_verify_a_node_running_on_silence(void)
{
    reset_clock();
    start(FIXED_500, 1, true);
    run(400, 0, 0);                      /* silent bus: as configured     */
    TEST_ASSERT_EQUAL(CAN_AB_RUNNING, s_ab.state);
    TEST_ASSERT_TRUE(s_ab.normal);
    TEST_ASSERT_FALSE(s_ab.verified);
    TEST_ASSERT_EQUAL(1, s_applies);

    run(25, 1, 0);                       /* one frame: not yet            */
    TEST_ASSERT_FALSE(s_ab.verified);
    run(25, 1, 0);
    TEST_ASSERT_TRUE(s_ab.verified);     /* the proof that was missing    */
    TEST_ASSERT_EQUAL_UINT16(500, s_ab.detected_kbps);
    TEST_ASSERT_EQUAL(1, s_applies);     /* nothing to change on the node */
    TEST_ASSERT_TRUE(s_ab.normal);
}

void test_ab_auto_on_a_silent_bus_keeps_listening(void)
{
    reset_clock();
    start(AUTO_500_250, 2, true);
    run(10000, 0, 0);
    TEST_ASSERT_EQUAL(CAN_AB_LISTENING, s_ab.state);
    TEST_ASSERT_EQUAL(0, s_applies);     /* no bitrate, no bounce, no TX  */
    TEST_ASSERT_FALSE(s_ab.normal);
    TEST_ASSERT_EQUAL_STRING("detecting", can_ab_state_name(&s_ab));
}

void test_ab_auto_unreadable_bus_rests_between_rounds(void)
{
    /* traffic at a bitrate no candidate reads: one round, a rest, a round */
    reset_clock();
    start(AUTO_500_250, 2, false);
    run(1000, 0, 100);
    TEST_ASSERT_EQUAL(CAN_AB_MISMATCH, s_ab.state);
    TEST_ASSERT_EQUAL(1, s_applies);     /* 500 -> 250, then rest         */
    TEST_ASSERT_EQUAL_STRING("mismatch", can_ab_state_name(&s_ab));

    run(10000, 0, 100);
    TEST_ASSERT_LESS_OR_EQUAL(12, s_applies); /* not a bounce per tick    */
    TEST_ASSERT_GREATER_OR_EQUAL(4, s_applies);
    TEST_ASSERT_EQUAL_UINT16(0, s_ab.detected_kbps);

    /* a shorter rest (the bus guard's listener: it must find a bus that
       wakes up at the other bitrate within a fraction of a second) */
    can_ab_cfg_t quick = { .candidates = { 500, 250 }, .n_candidates = 2,
                           .retry_ms = 200 };

    reset_clock();
    s_applies = 0;
    s_last = can_ab_init(&s_ab, &quick, s_now, s_frames, s_bad);
    run(2000, 0, 100);
    TEST_ASSERT_GREATER_OR_EQUAL(10, s_applies);
    TEST_ASSERT_LESS_OR_EQUAL(20, s_applies);
}

void test_ab_auto_follows_a_bus_that_changes_bitrate(void)
{
    reset_clock();
    start(AUTO_500_250, 2, true);
    run(100, 2, 0);
    TEST_ASSERT_TRUE(s_ab.normal);
    TEST_ASSERT_EQUAL_UINT16(500, s_ab.detected_kbps);

    run(200, 0, 100);                    /* the bench moved the bus       */
    TEST_ASSERT_FALSE(s_ab.normal);
    TEST_ASSERT_TRUE(s_last.listen_only);
    TEST_ASSERT_EQUAL_UINT16(250, s_last.baud_kbps);
    TEST_ASSERT_EQUAL_UINT32(1, s_ab.demotions);

    run(100, 2, 0);
    TEST_ASSERT_TRUE(s_ab.normal);
    TEST_ASSERT_EQUAL_UINT16(250, s_ab.detected_kbps);
    TEST_ASSERT_FALSE(s_last.listen_only);
}

void test_ab_runtime_mode_requests(void)
{
    /* a tool asks a talking node for listen-only: at once */
    reset_clock();
    start(FIXED_500, 1, true);
    run(100, 2, 0);
    TEST_ASSERT_TRUE(s_ab.normal);

    can_ab_action_t a = can_ab_want_normal(&s_ab, false, s_now, s_frames,
                                           s_bad);

    TEST_ASSERT_TRUE(a.apply);
    TEST_ASSERT_TRUE(a.listen_only);
    TEST_ASSERT_EQUAL_UINT16(500, a.baud_kbps);
    TEST_ASSERT_FALSE(s_ab.normal);
    TEST_ASSERT_EQUAL(CAN_AB_RUNNING, s_ab.state);

    /* asked again: nothing to do */
    a = can_ab_want_normal(&s_ab, false, s_now, s_frames, s_bad);
    TEST_ASSERT_FALSE(a.apply);

    /* and back to talking: the verdict is in, at once */
    a = can_ab_want_normal(&s_ab, true, s_now, s_frames, s_bad);
    TEST_ASSERT_TRUE(a.apply);
    TEST_ASSERT_FALSE(a.listen_only);
    TEST_ASSERT_TRUE(s_ab.normal);

    /* asked to talk before any verdict: not now, the policy promotes */
    reset_clock();
    start(FIXED_500, 1, false);
    a = can_ab_want_normal(&s_ab, true, s_now, s_frames, s_bad);
    TEST_ASSERT_FALSE(a.apply);
    TEST_ASSERT_FALSE(s_ab.normal);
    run(400, 0, 0);                      /* the silent rule               */
    TEST_ASSERT_EQUAL(1, s_applies);
    TEST_ASSERT_FALSE(s_last.listen_only);
    TEST_ASSERT_TRUE(s_ab.normal);

    /* asked to talk on a bus this bitrate cannot read: never */
    reset_clock();
    start(FIXED_500, 1, false);
    run(500, 0, 100);
    TEST_ASSERT_EQUAL(CAN_AB_MISMATCH, s_ab.state);
    a = can_ab_want_normal(&s_ab, true, s_now, s_frames, s_bad);
    TEST_ASSERT_FALSE(a.apply);
    run(3000, 0, 100);
    TEST_ASSERT_EQUAL(0, s_applies);
    TEST_ASSERT_FALSE(s_ab.normal);

    a = can_ab_want_normal(NULL, true, 0, 0, 0);
    TEST_ASSERT_FALSE(a.apply);
}

void test_ab_wraparound_time_and_counters(void)
{
    s_now = 0xFFFFFF00u;
    s_frames = 0xFFFFFFFFu;
    s_bad = 0xFFFFFFF0u;
    start(FIXED_500, 1, true);
    run(400, 0, 0);                      /* 300 ms across the time wrap   */
    TEST_ASSERT_EQUAL(CAN_AB_RUNNING, s_ab.state);
    TEST_ASSERT_TRUE(s_ab.normal);

    s_now = 0xFFFFFF00u;
    s_frames = 0xFFFFFFFFu;
    s_bad = 0xFFFFFFF0u;
    start(FIXED_500, 1, true);
    run(50, 1, 0);                       /* frame counter wraps to 1      */
    TEST_ASSERT_EQUAL(CAN_AB_RUNNING, s_ab.state);
    TEST_ASSERT_TRUE(s_ab.verified);

    s_now = 0xFFFFFF00u;
    s_frames = 5;
    s_bad = 0xFFFFFFF0u;
    start(FIXED_500, 1, true);
    run(100, 0, 100);                    /* error counter wraps           */
    TEST_ASSERT_EQUAL(CAN_AB_MISMATCH, s_ab.state);
}

void test_ab_null_and_empty_config(void)
{
    can_ab_t ab;
    can_ab_action_t a = can_ab_init(&ab, NULL, 0, 0, 0);

    TEST_ASSERT_TRUE(a.apply);
    TEST_ASSERT_EQUAL_UINT16(500, a.baud_kbps);
    TEST_ASSERT_TRUE(a.listen_only);

    a = can_ab_step(&ab, 1000, 0, 0);    /* silent: running, but never TX */
    TEST_ASSERT_FALSE(a.apply);
    TEST_ASSERT_FALSE(ab.normal);

    a = can_ab_init(NULL, NULL, 0, 0, 0);
    TEST_ASSERT_FALSE(a.apply);
    a = can_ab_step(NULL, 0, 0, 0);
    TEST_ASSERT_FALSE(a.apply);
    can_ab_mark(NULL, 0, 0, 0);
    TEST_ASSERT_EQUAL_UINT16(500, can_ab_baud(NULL));
    TEST_ASSERT_NOT_NULL(can_ab_state_name(NULL));

    /* more candidates than slots: clamped, not overrun */
    can_ab_cfg_t big = { .n_candidates = 200, .want_normal = true };

    for (int i = 0; i < CAN_AB_MAX_CANDIDATES; i++)
    {
        big.candidates[i] = (uint16_t)(100 + i);
    }

    (void)can_ab_init(&ab, &big, 0, 0, 0);
    TEST_ASSERT_EQUAL_UINT8(CAN_AB_MAX_CANDIDATES, ab.cfg.n_candidates);
}

void test_ab_adopts_a_stated_bitrate_on_a_silent_auto_bus(void)
{
    bool taken = false;
    can_ab_action_t a;

    /* auto, silent: no verdict of its own */
    reset_clock();
    start(AUTO_500_250, 2, true);
    run(400, 0, 0);
    TEST_ASSERT_EQUAL(CAN_AB_LISTENING, s_ab.state);

    /* a tester states 250: the node runs there, normal, unproven */
    a = can_ab_adopt(&s_ab, 250, s_now, s_frames, s_bad, &taken);
    TEST_ASSERT_TRUE(taken);
    TEST_ASSERT_TRUE(a.apply);
    TEST_ASSERT_EQUAL_UINT16(250, a.baud_kbps);
    TEST_ASSERT_FALSE(a.listen_only);
    can_ab_mark(&s_ab, s_now, s_frames, s_bad);
    TEST_ASSERT_EQUAL(CAN_AB_RUNNING, s_ab.state);
    TEST_ASSERT_TRUE(s_ab.normal);
    TEST_ASSERT_TRUE(s_ab.adopted);
    TEST_ASSERT_FALSE(s_ab.verified);
    TEST_ASSERT_EQUAL_UINT16(250, can_ab_baud(&s_ab));
    TEST_ASSERT_EQUAL_STRING("running", can_ab_state_name(&s_ab));

    /* frames at the stated bitrate prove it */
    run(50, 1, 0);
    TEST_ASSERT_TRUE(s_ab.verified);
    TEST_ASSERT_EQUAL_UINT16(250, s_ab.detected_kbps);

    /* a second statement changes nothing once a verdict exists */
    a = can_ab_adopt(&s_ab, 500, s_now, s_frames, s_bad, &taken);
    TEST_ASSERT_FALSE(taken);
    TEST_ASSERT_FALSE(a.apply);
    TEST_ASSERT_EQUAL_UINT16(250, can_ab_baud(&s_ab));

    /* a wrong statement is demoted by the watchdog like any talking node,
       and the candidate walk goes on from there */
    reset_clock();
    start(AUTO_500_250, 2, true);
    run(400, 0, 0);
    a = can_ab_adopt(&s_ab, 500, s_now, s_frames, s_bad, &taken);
    TEST_ASSERT_TRUE(taken);
    can_ab_mark(&s_ab, s_now, s_frames, s_bad);
    s_applies = 0;
    run(50, 0, 10);                      /* garbage, no frame             */
    TEST_ASSERT_FALSE(s_ab.normal);
    TEST_ASSERT_FALSE(s_ab.adopted);
    TEST_ASSERT_TRUE(s_ab.bitten);
    TEST_ASSERT_TRUE(s_last.listen_only);
    TEST_ASSERT_EQUAL_UINT16(250, s_last.baud_kbps);

    /* the settings' listen-only wish is kept at the stated bitrate */
    reset_clock();
    start(AUTO_500_250, 2, false);
    run(400, 0, 0);
    a = can_ab_adopt(&s_ab, 250, s_now, s_frames, s_bad, &taken);
    TEST_ASSERT_TRUE(taken);
    TEST_ASSERT_TRUE(a.apply);
    TEST_ASSERT_TRUE(a.listen_only);
    TEST_ASSERT_FALSE(s_ab.normal);
}

void test_ab_adopt_is_refused_without_a_silent_auto_bus(void)
{
    bool taken = true;
    can_ab_action_t a;

    /* a fixed bitrate is the settings' word */
    reset_clock();
    start(FIXED_500, 1, true);
    run(400, 0, 0);
    a = can_ab_adopt(&s_ab, 250, s_now, s_frames, s_bad, &taken);
    TEST_ASSERT_FALSE(taken);
    TEST_ASSERT_FALSE(a.apply);
    TEST_ASSERT_EQUAL_UINT16(500, can_ab_baud(&s_ab));

    /* auto, but the node is too young to call the bus silent */
    reset_clock();
    start(AUTO_500_250, 2, true);
    run(100, 0, 0);
    a = can_ab_adopt(&s_ab, 500, s_now, s_frames, s_bad, &taken);
    TEST_ASSERT_FALSE(taken);
    run(300, 0, 0);
    a = can_ab_adopt(&s_ab, 500, s_now, s_frames, s_bad, &taken);
    TEST_ASSERT_TRUE(taken);             /* ... and old enough now        */

    /* an error in the span is not silence; a span later it is */
    reset_clock();
    start(AUTO_500_250, 2, true);
    run(400, 0, 0);
    s_bad += 1;
    a = can_ab_adopt(&s_ab, 500, s_now, s_frames, s_bad, &taken);
    TEST_ASSERT_FALSE(taken);
    run(1100, 0, 0);
    a = can_ab_adopt(&s_ab, 500, s_now, s_frames, s_bad, &taken);
    TEST_ASSERT_TRUE(taken);

    /* a bitrate this node does not know */
    reset_clock();
    start(AUTO_500_250, 2, true);
    run(400, 0, 0);
    a = can_ab_adopt(&s_ab, 125, s_now, s_frames, s_bad, &taken);
    TEST_ASSERT_FALSE(taken);
    TEST_ASSERT_EQUAL(CAN_AB_LISTENING, s_ab.state);

    /* a bus that once carried unreadable traffic: only frames promote */
    reset_clock();
    start(AUTO_500_250, 2, true);
    run(1000, 0, 100);                   /* every candidate fails         */
    TEST_ASSERT_TRUE(s_ab.bitten);
    run(2000, 0, 0);                     /* then silence                  */
    a = can_ab_adopt(&s_ab, 500, s_now, s_frames, s_bad, &taken);
    TEST_ASSERT_FALSE(taken);
    TEST_ASSERT_FALSE(s_ab.normal);

    /* NULL is harmless */
    a = can_ab_adopt(NULL, 500, 0, 0, 0, &taken);
    TEST_ASSERT_FALSE(taken);
    TEST_ASSERT_FALSE(a.apply);
    a = can_ab_adopt(&s_ab, 500, s_now, s_frames, s_bad, NULL);
    TEST_ASSERT_FALSE(a.apply);
}

void run_autobaud_tests(void)
{
    RUN_TEST(test_ab_always_starts_listen_only);
    RUN_TEST(test_ab_frames_prove_the_bitrate_and_promote);
    RUN_TEST(test_ab_listen_setting_never_gets_a_tx_pin);
    RUN_TEST(test_ab_silent_bus_fixed_bitrate_goes_to_its_mode);
    RUN_TEST(test_ab_wrong_bitrate_never_talks);
    RUN_TEST(test_ab_silence_after_a_mismatch_does_not_promote);
    RUN_TEST(test_ab_frames_after_a_mismatch_promote);
    RUN_TEST(test_ab_watchdog_demotes_a_talking_node);
    RUN_TEST(test_ab_errors_beside_frames_are_not_a_mismatch);
    RUN_TEST(test_ab_rare_glitches_never_add_up);
    RUN_TEST(test_ab_auto_walks_to_the_candidate_that_reads);
    RUN_TEST(test_ab_one_corrupted_frame_is_not_a_mismatch);
    RUN_TEST(test_ab_frames_verify_a_node_running_on_silence);
    RUN_TEST(test_ab_auto_on_a_silent_bus_keeps_listening);
    RUN_TEST(test_ab_auto_unreadable_bus_rests_between_rounds);
    RUN_TEST(test_ab_auto_follows_a_bus_that_changes_bitrate);
    RUN_TEST(test_ab_runtime_mode_requests);
    RUN_TEST(test_ab_wraparound_time_and_counters);
    RUN_TEST(test_ab_null_and_empty_config);
    RUN_TEST(test_ab_adopts_a_stated_bitrate_on_a_silent_auto_bus);
    RUN_TEST(test_ab_adopt_is_refused_without_a_silent_auto_bus);
}
