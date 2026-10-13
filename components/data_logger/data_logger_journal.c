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
 * @file data_logger_journal.c
 * @brief The writer's view of the clock (issue #984, 2026-10-13): catches
 *        up with rtc_manager's clock steps by generation before every
 *        batch, re-bases the records queued before a step, writes the
 *        clock journal `/sd/logs/clock.jsonl` (one JSON line per event:
 *        boot, valid, step, rotate; bounded to the newest 32 KB past
 *        64 KB), asks the writer to mark the open files or rotate them.
 *        Writer task only: it is the card's single toucher. The rules
 *        are pure (data_logger_clock.c); this file is the glue.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "rtc_manager.h"

#include "data_logger_private.h"

static const char *TAG = "data_logger";

#define JOURNAL_PATH DL_DIR "/" DL_JOURNAL_NAME
#define JOURNAL_TMP  JOURNAL_PATH ".tmp"
#define JOURNAL_BUF  512          /* DMA-capable stdio buffer per write */

static uint32_t s_boot;           /* the restart tracker's boot count  */
static bool     s_boot_logged;
static bool     s_caught_up;      /* the first lap took the generation */
static uint32_t s_seen_gen;
static bool     s_valid_seen;     /* a valid clock was seen this boot  */
static dl_clock_step_t s_steps[DL_CLOCK_STEPS_KEEP]; /* for re-basing  */
static uint32_t s_nsteps;         /* pushed in all; ring = n % KEEP    */
static uint32_t s_rot_by_clock;
static uint32_t s_journal_bytes;
static bool     s_journal_warned;

static uint32_t mono_ms_now(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static int64_t wall_ms_now(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* ---- the file ----------------------------------------------------------------- */

/* keep the newest DL_JOURNAL_KEEP bytes, cut at a line boundary: a
 * rewrite through a temp file (FAT refuses a rename over an existing
 * name: the old file is unlinked first) */
static void journal_trim(uint64_t size, char *stdio_buf)
{
    size_t keep = DL_JOURNAL_KEEP;
    char *tail = heap_caps_malloc(keep, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (tail == NULL)
    {
        return;
    }

    FILE *r = fopen(JOURNAL_PATH, "rb");
    size_t n = 0;

    if (r != NULL)
    {
        setvbuf(r, stdio_buf, _IOFBF, JOURNAL_BUF);
        fseek(r, (long)(size - keep), SEEK_SET);
        n = fread(tail, 1, keep, r);
        fclose(r);
    }

    size_t off = dl_journal_trim_offset(tail, n);
    FILE *w = (n > off) ? fopen(JOURNAL_TMP, "wb") : NULL;
    bool ok = false;

    if (w != NULL)
    {
        setvbuf(w, stdio_buf, _IOFBF, JOURNAL_BUF);
        ok = fwrite(tail + off, 1, n - off, w) == n - off &&
             fflush(w) == 0 && fsync(fileno(w)) == 0;
        fclose(w);
    }

    heap_caps_free(tail);

    if (ok)
    {
        unlink(JOURNAL_PATH);
        ok = rename(JOURNAL_TMP, JOURNAL_PATH) == 0;
    }

    if (!ok)
    {
        unlink(JOURNAL_TMP);
        ESP_LOGW(TAG, "clock journal: trim failed (errno %d)", errno);
    }
}

static void journal_append(const char *line, int n)
{
    if (n <= 0)
    {
        return;
    }

    char *buf = heap_caps_malloc(JOURNAL_BUF,
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

    if (buf == NULL)
    {
        return;
    }

    struct stat sb;

    if (stat(JOURNAL_PATH, &sb) == 0 && (uint64_t)sb.st_size > DL_JOURNAL_MAX)
    {
        journal_trim((uint64_t)sb.st_size, buf);
    }

    FILE *f = fopen(JOURNAL_PATH, "a");
    bool ok = false;

    if (f != NULL)
    {
        setvbuf(f, buf, _IOFBF, JOURNAL_BUF);
        ok = fwrite(line, 1, (size_t)n, f) == (size_t)n &&
             fflush(f) == 0 && fsync(fileno(f)) == 0;
        fclose(f);
    }

    heap_caps_free(buf);

    if (ok)
    {
        s_journal_bytes = (stat(JOURNAL_PATH, &sb) == 0)
                              ? (uint32_t)sb.st_size : s_journal_bytes + n;
    }
    else if (!s_journal_warned)
    {
        ESP_LOGW(TAG, "clock journal: cannot write %s (errno %d)",
                 JOURNAL_PATH, errno);
        s_journal_warned = true;
    }
}

/* ---- the lap ------------------------------------------------------------------- */

static void journal_step(const char *ev, const dl_clock_step_t *step)
{
    const char *files[2];
    int nfiles = dl_writer_open_files(files, 2);
    char line[320];

    journal_append(line, dl_journal_step_line(line, sizeof(line), ev, s_boot,
                                              step, files, nfiles));
}

static void push_step(const dl_clock_step_t *step)
{
    s_steps[s_nsteps % DL_CLOCK_STEPS_KEEP] = *step;
    s_nsteps++;
}

void dl_journal_set_boot(uint32_t boot)
{
    s_boot = boot;
}

void dl_journal_lap(void)
{
    rtc_clock_state_t st;

    rtc_manager_clock_state(&st);

    if (!s_caught_up)
    {
        /* the boot restore (and anything else before the writer's first
         * file) predates every record of this boot: nothing to re-base,
         * nothing to rotate; the boot line below records what was found */
        s_seen_gen = st.generation;
        s_valid_seen = st.valid;
        s_caught_up = true;
    }

    if (!s_boot_logged)
    {
        char line[160];

        journal_append(line, dl_journal_boot_line(line, sizeof(line), s_boot,
                                                  wall_ms_now(),
                                                  mono_ms_now(), st.valid,
                                                  st.source));
        s_boot_logged = true;
    }

    if (st.generation == s_seen_gen)
    {
        return;
    }

    rtc_clock_step_t fresh[DL_CLOCK_STEPS_KEEP];
    int n = rtc_manager_steps_since(s_seen_gen, fresh, DL_CLOCK_STEPS_KEEP);

    s_seen_gen = st.generation;

    for (int i = 0; i < n; i++)
    {
        dl_clock_step_t step =
        {
            .generation = fresh[i].generation,
            .mono_ms = fresh[i].mono_ms,
            .delta_ms = fresh[i].delta_ms,
            .wall_ms = fresh[i].wall_ms,
            .source = fresh[i].source,
        };
        bool first_valid = !s_valid_seen;

        s_valid_seen = true;
        push_step(&step);
        journal_step(first_valid ? "valid" : "step", &step);

        if (dl_clock_rotates(&step, first_valid))
        {
            dl_rotated_t rot[2];
            int k = dl_writer_clock_rotate(&step, rot);

            for (int j = 0; j < k; j++)
            {
                char line[200];

                journal_append(line, dl_journal_rotate_line(
                                         line, sizeof(line), s_boot,
                                         wall_ms_now(), rot[j].stream,
                                         rot[j].from, rot[j].to));
            }

            s_rot_by_clock += (uint32_t)k;
            ESP_LOGI(TAG, "the clock %s by %s (%+lld ms): %d file(s) "
                     "rotated, the journal has it",
                     first_valid ? "became valid" : "stepped",
                     dl_clock_source_name(step.source),
                     (long long)step.delta_ms, k);
        }
        else
        {
            dl_writer_clock_mark(&step);
            ESP_LOGI(TAG, "the clock stepped by %s (%+lld ms): marked in "
                     "the open files and the journal",
                     dl_clock_source_name(step.source),
                     (long long)step.delta_ms);
        }
    }
}

void dl_journal_rebase(dl_record_t *rec)
{
    uint32_t kept = (s_nsteps < DL_CLOCK_STEPS_KEEP) ? s_nsteps
                                                     : DL_CLOCK_STEPS_KEEP;

    if (kept > 0)
    {
        (void)dl_clock_rebase(rec, s_steps, (int)kept);
    }
}

void dl_journal_stats(dl_clock_stats_t *out)
{
    rtc_clock_state_t st;

    rtc_manager_clock_state(&st);
    out->valid = st.valid;
    out->source = st.source;
    out->steps_this_boot = st.steps_this_boot;
    out->rotations_by_clock = s_rot_by_clock;
    out->journal_bytes = s_journal_bytes;
}
