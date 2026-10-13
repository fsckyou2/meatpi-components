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
 * @file data_logger_clock.c
 * @brief PURE clock rules (issue #984, 2026-10-13; host-tested): how a
 *        queued record is re-based after a clock step, where the
 *        relative formats take their time from, when a step rotates the
 *        files, and the byte-exact journal lines and in-band markers.
 *        No IO, no RTOS, no clocks read here.
 */
#include <stdio.h>
#include <string.h>

#include "data_logger_private.h"

/* ---- the rules ------------------------------------------------------------- */

int dl_clock_rebase(dl_record_t *rec, const dl_clock_step_t *steps, int n)
{
    if (rec == NULL || (rec->flags & DL_REC_ABS) != 0)
    {
        return 0;
    }

    int applied = 0;

    for (int i = 0; i < n; i++)
    {
        /* the step happened after the record was taken: modulo-32 so a
         * stamp taken just before the 49.7-day wrap still compares right */
        if ((int32_t)(steps[i].mono_ms - rec->mono_ms) > 0)
        {
            rec->ts_ms += steps[i].delta_ms;
            applied++;
        }
    }

    return applied;
}

int64_t dl_clock_rel_ms(const dl_record_t *rec, int64_t start_ms,
                        uint32_t mono_open_ms)
{
    if (rec->flags & DL_REC_ABS)
    {
        return rec->ts_ms - start_ms; /* a stamp from another boot */
    }

    return (int64_t)(uint32_t)(rec->mono_ms - mono_open_ms);
}

int64_t dl_clock_start_ms(int64_t wall_now_ms, uint32_t mono_now_ms,
                          uint32_t mono_open_ms)
{
    return wall_now_ms - (int64_t)(uint32_t)(mono_now_ms - mono_open_ms);
}

bool dl_clock_rotates(const dl_clock_step_t *step, bool first_valid)
{
    return first_valid || step->delta_ms >= DL_CLOCK_ROTATE_MS ||
           step->delta_ms <= -(int64_t)DL_CLOCK_ROTATE_MS;
}

const char *dl_clock_source_name(uint8_t source)
{
    /* rtc_manager's rtc_clock_source_t order */
    static const char *const NAMES[] = { "none", "rtc", "sntp", "manual" };

    return (source < sizeof(NAMES) / sizeof(NAMES[0])) ? NAMES[source] : "?";
}

/* ---- journal lines ----------------------------------------------------------- */

int dl_journal_boot_line(char *out, size_t cap, uint32_t boot,
                         int64_t ts_ms, uint32_t up_ms, bool valid,
                         uint8_t source)
{
    int n = snprintf(out, cap,
                     "{\"ev\":\"boot\",\"boot\":%lu,\"ts\":%lld,"
                     "\"up_ms\":%lu,\"valid\":%s,\"source\":\"%s\"}\n",
                     (unsigned long)boot, (long long)ts_ms,
                     (unsigned long)up_ms, valid ? "true" : "false",
                     dl_clock_source_name(source));

    return (n > 0 && (size_t)n < cap) ? n : -1;
}

int dl_journal_step_line(char *out, size_t cap, const char *ev,
                         uint32_t boot, const dl_clock_step_t *step,
                         const char *const *files, int nfiles)
{
    int n = snprintf(out, cap,
                     "{\"ev\":\"%s\",\"boot\":%lu,\"ts_before\":%lld,"
                     "\"ts_after\":%lld,\"delta_ms\":%lld,\"source\":\"%s\","
                     "\"files\":[",
                     ev, (unsigned long)boot,
                     (long long)(step->wall_ms - step->delta_ms),
                     (long long)step->wall_ms, (long long)step->delta_ms,
                     dl_clock_source_name(step->source));

    if (n < 0 || (size_t)n >= cap)
    {
        return -1;
    }

    for (int i = 0; i < nfiles; i++)
    {
        int m = snprintf(out + n, cap - (size_t)n, "%s\"%s\"",
                         (i > 0) ? "," : "", files[i]);

        if (m < 0 || (size_t)(n + m) >= cap)
        {
            return -1;
        }

        n += m;
    }

    int m = snprintf(out + n, cap - (size_t)n, "]}\n");

    if (m < 0 || (size_t)(n + m) >= cap)
    {
        return -1;
    }

    return n + m;
}

int dl_journal_rotate_line(char *out, size_t cap, uint32_t boot,
                           int64_t ts_ms, const char *stream,
                           const char *from, const char *to)
{
    int n = snprintf(out, cap,
                     "{\"ev\":\"rotate\",\"boot\":%lu,\"ts\":%lld,"
                     "\"stream\":\"%s\",\"from\":\"%s\",\"to\":\"%s\","
                     "\"why\":\"clock\"}\n",
                     (unsigned long)boot, (long long)ts_ms, stream, from,
                     to);

    return (n > 0 && (size_t)n < cap) ? n : -1;
}

size_t dl_journal_trim_offset(const char *tail, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        if (tail[i] == '\n')
        {
            return i + 1; /* the first whole line starts after it */
        }
    }

    return len; /* no line boundary in the tail: keep nothing */
}

/* ---- in-band markers --------------------------------------------------------- */

int dl_jsonl_step_row(char *out, size_t cap, const dl_clock_step_t *step)
{
    int n = snprintf(out, cap,
                     "{\"ts\":%lld,\"event\":\"clock_step\","
                     "\"delta_ms\":%lld,\"source\":\"%s\"}\n",
                     (long long)step->wall_ms, (long long)step->delta_ms,
                     dl_clock_source_name(step->source));

    return (n > 0 && (size_t)n < cap) ? n : -1;
}

int dl_csv_step_row(char *out, size_t cap, const dl_clock_step_t *step)
{
    int n = snprintf(out, cap, "%lld,logger.clock_step,%lld\n",
                     (long long)step->wall_ms, (long long)step->delta_ms);

    return (n > 0 && (size_t)n < cap) ? n : -1;
}

size_t dl_wdl_encode_step(uint8_t *buf, const dl_clock_step_t *step)
{
    size_t off = 0;

    buf[off++] = 0x04;

    for (int i = 0; i < 8; i++)
    {
        buf[off++] = (uint8_t)((uint64_t)step->wall_ms >> (8 * i));
    }

    for (int i = 0; i < 8; i++)
    {
        buf[off++] = (uint8_t)((uint64_t)step->delta_ms >> (8 * i));
    }

    buf[off++] = step->source;
    return off; /* DL_WDL_STEP_LEN */
}
