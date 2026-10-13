/* Host tests for data_logger_clock.c (issue #984, 2026-10-13): the record
   layout the rings depend on, how a queued record is re-based after a
   clock step, where the relative formats take their time from, when a
   step rotates the files, and the byte-exact journal lines and in-band
   markers. */
#include <stddef.h>
#include <string.h>

#include "unity.h"

#include "data_logger_private.h"

#define WALL 1791880680000LL   /* 2026-10-13T08:38:00Z in ms */

static dl_record_t param_rec(int64_t ts_ms, uint32_t mono_ms, uint8_t flags)
{
    dl_record_t r = { .ts_ms = ts_ms, .kind = DL_REC_PARAM, .flags = flags,
                      .mono_ms = mono_ms, .u.p = { .value = 1.0, .param = 2 } };

    return r;
}

static dl_record_t frame_rec(int64_t ts_ms, uint32_t mono_ms, uint8_t flags)
{
    dl_record_t r = { .ts_ms = ts_ms, .kind = DL_REC_FRAME, .flags = flags,
                      .mono_ms = mono_ms,
                      .u.f = { .id = 0x7E8, .dlc = 2, .data = { 0xAA, 0xBB } } };

    return r;
}

void test_record_layout_is_32_bytes_with_the_stamps_in_the_padding(void)
{
    TEST_ASSERT_EQUAL_size_t(32, sizeof(dl_record_t));
    TEST_ASSERT_EQUAL_size_t(24, offsetof(dl_record_t, kind));
    TEST_ASSERT_EQUAL_size_t(25, offsetof(dl_record_t, flags));
    TEST_ASSERT_EQUAL_size_t(28, offsetof(dl_record_t, mono_ms));
    TEST_ASSERT_EQUAL_UINT32(2, DL_PS_VERSION); /* the envelope moved on */
}

void test_rebase_adds_the_steps_that_came_after_the_record(void)
{
    /* the record was taken at mono 5000 on an unset clock */
    dl_record_t r = param_rec(5000, 5000, 0);
    dl_clock_step_t steps[3] =
    {
        { .generation = 1, .mono_ms = 4000, .delta_ms = 777, .source = 1 },
        { .generation = 2, .mono_ms = 6000, .delta_ms = WALL, .source = 2 },
        { .generation = 3, .mono_ms = 9000, .delta_ms = -2500, .source = 2 },
    };

    TEST_ASSERT_EQUAL_INT(0, dl_clock_rebase(&r, steps, 0));
    TEST_ASSERT_EQUAL_INT64(5000, r.ts_ms);
    /* the step at 4000 predates the record: not applied; 6000 and 9000 are */
    TEST_ASSERT_EQUAL_INT(2, dl_clock_rebase(&r, steps, 3));
    TEST_ASSERT_EQUAL_INT64(5000 + WALL - 2500, r.ts_ms);
    /* a record taken at the step's own millisecond counts as after it */
    r = param_rec(6000, 6000, 0);
    TEST_ASSERT_EQUAL_INT(1, dl_clock_rebase(&r, steps, 3));
    TEST_ASSERT_EQUAL_INT64(6000 - 2500, r.ts_ms);
}

void test_rebase_leaves_absolute_records_alone(void)
{
    dl_record_t r = param_rec(WALL, 100, DL_REC_ABS);
    dl_clock_step_t step = { .mono_ms = 200, .delta_ms = 3600000, .source = 2 };

    TEST_ASSERT_EQUAL_INT(0, dl_clock_rebase(&r, &step, 1));
    TEST_ASSERT_EQUAL_INT64(WALL, r.ts_ms);
    TEST_ASSERT_EQUAL_INT(0, dl_clock_rebase(NULL, &step, 1));
}

void test_rebase_compares_the_stamps_modulo_the_wrap(void)
{
    /* the record was taken 256 ms before the 49.7-day wrap, the step 16 ms
       after it: the step is after the record */
    dl_record_t r = param_rec(1000, 0xFFFFFF00u, 0);
    dl_clock_step_t step = { .mono_ms = 0x00000010u, .delta_ms = 50 };

    TEST_ASSERT_EQUAL_INT(1, dl_clock_rebase(&r, &step, 1));
    TEST_ASSERT_EQUAL_INT64(1050, r.ts_ms);
    /* the other way round: a step long before the record is not applied */
    r = param_rec(1000, 0x00000010u, 0);
    step.mono_ms = 0xFFFFFF00u;
    TEST_ASSERT_EQUAL_INT(0, dl_clock_rebase(&r, &step, 1));
}

void test_relative_time_comes_from_the_monotonic_clock(void)
{
    /* a file opened at wall W, mono 10000; the wall then steps BACK by
       2 h: rows keep climbing */
    const uint32_t open = 10000;
    dl_record_t a = frame_rec(WALL + 1000, open + 1000, 0);
    dl_record_t b = frame_rec(WALL + 2000 - 7200000, open + 2000, 0);
    dl_record_t c = frame_rec(WALL + 3000 - 7200000, open + 3000, 0);

    TEST_ASSERT_EQUAL_INT64(1000, dl_clock_rel_ms(&a, WALL, open));
    TEST_ASSERT_EQUAL_INT64(2000, dl_clock_rel_ms(&b, WALL, open));
    TEST_ASSERT_EQUAL_INT64(3000, dl_clock_rel_ms(&c, WALL, open));
    /* an absolute (salvaged) record keeps its wall-based offset, even
       negative: it was taken before this file */
    dl_record_t s = frame_rec(WALL - 15000, 77, DL_REC_ABS);

    TEST_ASSERT_EQUAL_INT64(-15000, dl_clock_rel_ms(&s, WALL, open));
    /* the open time as the corrected wall clock says it now */
    TEST_ASSERT_EQUAL_INT64(WALL - 7200000,
                            dl_clock_start_ms(WALL + 5000 - 7200000,
                                              open + 5000, open));
    /* past the wrap too */
    TEST_ASSERT_EQUAL_INT64(WALL,
                            dl_clock_start_ms(WALL + 32, 0x00000010u,
                                              0xFFFFFFF0u));
}

void test_rotation_rule(void)
{
    dl_clock_step_t small = { .delta_ms = 59999 };
    dl_clock_step_t back = { .delta_ms = -60000 };
    dl_clock_step_t big = { .delta_ms = 3600000 };
    dl_clock_step_t none = { .delta_ms = 0 };

    TEST_ASSERT_FALSE(dl_clock_rotates(&small, false));
    TEST_ASSERT_FALSE(dl_clock_rotates(&none, false));
    TEST_ASSERT_TRUE(dl_clock_rotates(&back, false));
    TEST_ASSERT_TRUE(dl_clock_rotates(&big, false));
    /* the first valid clock of a boot rotates whatever the delta */
    TEST_ASSERT_TRUE(dl_clock_rotates(&none, true));
    TEST_ASSERT_EQUAL_STRING("sntp", dl_clock_source_name(2));
    TEST_ASSERT_EQUAL_STRING("none", dl_clock_source_name(0));
    TEST_ASSERT_EQUAL_STRING("?", dl_clock_source_name(9));
}

void test_journal_lines_byte_exact(void)
{
    char line[320];
    dl_clock_step_t step = { .generation = 2, .mono_ms = 12345,
                             .delta_ms = 3600000, .wall_ms = WALL + 3600000,
                             .source = 2 };
    const char *files[2] = { "dl_0000000042.jsonl", "can_0000000042.mf4" };
    int n = dl_journal_boot_line(line, sizeof(line), 5, WALL, 4321, false, 0);

    TEST_ASSERT_EQUAL_STRING(
        "{\"ev\":\"boot\",\"boot\":5,\"ts\":1791880680000,\"up_ms\":4321,"
        "\"valid\":false,\"source\":\"none\"}\n", line);
    TEST_ASSERT_EQUAL_INT((int)strlen(line), n);

    n = dl_journal_step_line(line, sizeof(line), "valid", 5, &step, files, 2);
    TEST_ASSERT_EQUAL_STRING(
        "{\"ev\":\"valid\",\"boot\":5,\"ts_before\":1791880680000,"
        "\"ts_after\":1791884280000,\"delta_ms\":3600000,\"source\":\"sntp\","
        "\"files\":[\"dl_0000000042.jsonl\",\"can_0000000042.mf4\"]}\n", line);
    TEST_ASSERT_EQUAL_INT((int)strlen(line), n);

    step.delta_ms = -2500;
    step.source = 3;
    n = dl_journal_step_line(line, sizeof(line), "step", 6, &step, NULL, 0);
    TEST_ASSERT_EQUAL_STRING(
        "{\"ev\":\"step\",\"boot\":6,\"ts_before\":1791884282500,"
        "\"ts_after\":1791884280000,\"delta_ms\":-2500,\"source\":\"manual\","
        "\"files\":[]}\n", line);

    n = dl_journal_rotate_line(line, sizeof(line), 5, WALL + 3600000, "params",
                               "dl_0000000042.jsonl", "dl_1791884280.jsonl");
    TEST_ASSERT_EQUAL_STRING(
        "{\"ev\":\"rotate\",\"boot\":5,\"ts\":1791884280000,"
        "\"stream\":\"params\",\"from\":\"dl_0000000042.jsonl\","
        "\"to\":\"dl_1791884280.jsonl\",\"why\":\"clock\"}\n", line);
    TEST_ASSERT_EQUAL_INT((int)strlen(line), n);
    /* a buffer too small refuses instead of truncating */
    TEST_ASSERT_EQUAL_INT(-1, dl_journal_step_line(line, 40, "step", 5, &step,
                                                   files, 2));
}

void test_markers_byte_exact(void)
{
    char row[128];
    uint8_t rec[DL_WDL_STEP_LEN];
    dl_clock_step_t step = { .delta_ms = 3600000, .wall_ms = WALL + 3600000,
                             .source = 2 };

    TEST_ASSERT_GREATER_THAN(0, dl_jsonl_step_row(row, sizeof(row), &step));
    TEST_ASSERT_EQUAL_STRING(
        "{\"ts\":1791884280000,\"event\":\"clock_step\",\"delta_ms\":3600000,"
        "\"source\":\"sntp\"}\n", row);
    TEST_ASSERT_GREATER_THAN(0, dl_csv_step_row(row, sizeof(row), &step));
    TEST_ASSERT_EQUAL_STRING("1791884280000,logger.clock_step,3600000\n", row);

    /* 0x04 | ts LE | delta LE | source */
    TEST_ASSERT_EQUAL_size_t(DL_WDL_STEP_LEN, dl_wdl_encode_step(rec, &step));
    TEST_ASSERT_EQUAL_HEX8(0x04, rec[0]);
    /* 1791884280000 = 0x1A134A040C0 (python: v.to_bytes(8, "little")) */
    TEST_ASSERT_EQUAL_HEX8(0xC0, rec[1]);
    TEST_ASSERT_EQUAL_HEX8(0x40, rec[2]);
    TEST_ASSERT_EQUAL_HEX8(0xA0, rec[3]);
    TEST_ASSERT_EQUAL_HEX8(0x34, rec[4]);
    TEST_ASSERT_EQUAL_HEX8(0xA1, rec[5]);
    TEST_ASSERT_EQUAL_HEX8(0x01, rec[6]);
    TEST_ASSERT_EQUAL_HEX8(0x00, rec[7]);
    TEST_ASSERT_EQUAL_HEX8(0x00, rec[8]);
    /* 3600000 = 0x36EE80 */
    TEST_ASSERT_EQUAL_HEX8(0x80, rec[9]);
    TEST_ASSERT_EQUAL_HEX8(0xEE, rec[10]);
    TEST_ASSERT_EQUAL_HEX8(0x36, rec[11]);
    TEST_ASSERT_EQUAL_HEX8(0x00, rec[16]);
    TEST_ASSERT_EQUAL_HEX8(2, rec[17]);
    /* a negative delta is two's complement */
    step.delta_ms = -1;
    dl_wdl_encode_step(rec, &step);
    TEST_ASSERT_EQUAL_HEX8(0xFF, rec[9]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, rec[16]);
}

void test_wdl_scan_accepts_the_step_record(void)
{
    uint8_t buf[64];
    size_t n = 0;
    bool bad = true;
    dl_clock_step_t step = { .delta_ms = 5, .wall_ms = 7, .source = 1 };

    /* a param record, then the step record as the last one */
    buf[n++] = 0x02;
    memset(&buf[n], 0, 18);
    n += 18;
    n += dl_wdl_encode_step(&buf[n], &step);
    TEST_ASSERT_EQUAL_UINT(n, dl_recover_wdl_scan(buf, n, &bad));
    TEST_ASSERT_FALSE(bad);
    /* cut inside the step record: partial, not garbage */
    TEST_ASSERT_EQUAL_UINT(19, dl_recover_wdl_scan(buf, n - 3, &bad));
    TEST_ASSERT_FALSE(bad);
    /* the salvage sanity knows the one flag */
    dl_record_t r = param_rec(1, 2, DL_REC_ABS);

    TEST_ASSERT_TRUE(dl_recover_record_sane(&r));
    r.flags = 0x02;
    TEST_ASSERT_FALSE(dl_recover_record_sane(&r));
}

void test_journal_trim_cuts_at_a_line_boundary(void)
{
    const char *tail = "xyz\n{\"ev\":\"step\"}\n{\"ev\":\"boot\"}\n";

    TEST_ASSERT_EQUAL_UINT(4, dl_journal_trim_offset(tail, strlen(tail)));
    TEST_ASSERT_EQUAL_UINT(5, dl_journal_trim_offset("abcde", 5));
    TEST_ASSERT_EQUAL_UINT(0, dl_journal_trim_offset("", 0));
}

void test_relative_rows_take_the_offset(void)
{
    static const uint8_t DATA[] = { 0x01, 0x02, 0xAB };
    char row[80];
    uint8_t buf[DL_MF4_REC_SIZE];
    uint8_t msg[DL_BLF_MSG_SIZE];
    dl_record_t rec = frame_rec(0, 0, 0);

    TEST_ASSERT_GREATER_THAN(0, dl_asc_row(row, sizeof(row), 1234, 0x7E8, 0,
                                           DATA, 3));
    TEST_ASSERT_EQUAL_STRING(
        "   1.234000 1  7E8             Rx   d 3 01 02 AB\n", row);
    /* mf4: t = 1.0 s as f64 LE for 1000 ms */
    static const uint8_t ONE[] = { 0, 0, 0, 0, 0, 0, 0xF0, 0x3F };

    TEST_ASSERT_EQUAL_size_t(DL_MF4_REC_SIZE, dl_mf4_record(buf, 1000, &rec));
    TEST_ASSERT_EQUAL_MEMORY(ONE, buf, 8);
    /* blf: 500 ms -> 500,000,000 ns = 0x1DCD6500 LE at +24; a negative
       offset (a salvaged record) clamps at 0 */
    TEST_ASSERT_EQUAL_size_t(DL_BLF_MSG_SIZE, dl_blf_message(msg, 500, &rec));
    TEST_ASSERT_EQUAL_HEX8(0x00, msg[24]);
    TEST_ASSERT_EQUAL_HEX8(0x65, msg[25]);
    TEST_ASSERT_EQUAL_HEX8(0xCD, msg[26]);
    TEST_ASSERT_EQUAL_HEX8(0x1D, msg[27]);
    dl_blf_message(msg, -1500, &rec);
    TEST_ASSERT_EQUAL_HEX8(0x00, msg[24]);
    TEST_ASSERT_EQUAL_HEX8(0x00, msg[27]);
    TEST_ASSERT_EQUAL_HEX8(0x00, msg[31]);
}

void test_mf4_prelude_keeps_the_start_at_the_patch_offsets(void)
{
    static uint8_t buf[DL_MF4_PRELUDE_MAX];
    uint32_t dt = 0;
    uint32_t cg = 0;
    uint64_t ns = 0;
    size_t n = dl_mf4_prelude(buf, sizeof(buf), WALL, &dt, &cg);

    TEST_ASSERT_GREATER_THAN(DL_MF4_FH_START_OFF + 8, (int)n);
    memcpy(&ns, buf + DL_MF4_HD_START_OFF, 8);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)WALL * 1000000ULL, ns);
    memcpy(&ns, buf + DL_MF4_FH_START_OFF, 8);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)WALL * 1000000ULL, ns);
    /* the two offsets are inside their blocks */
    TEST_ASSERT_EQUAL_MEMORY("##HD", buf + 64, 4);
    TEST_ASSERT_EQUAL_MEMORY("##FH", buf + 168, 4);
}
