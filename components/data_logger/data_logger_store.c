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
 * @file data_logger_store.c
 * @brief The card-side file operations on a stream (split out of
 *        data_logger.c 2026-10-13): open the newest file or a fresh one,
 *        close, rotate, age the oldest out, count and prune the set-aside
 *        copies, quarantine a corrupt file (ROBUSTNESS.md case 5) and
 *        latch the faults. Writer task only; the writer's loop
 *        (data_logger_writer.c) decides when, this file decides how.
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dev_status_manager.h"
#include "rtc_manager.h"

#include "data_logger_private.h"

static const char *TAG = "data_logger";

static dl_store_stats_t s_stats;

dl_store_stats_t *dl_store_stats(void)
{
    return &s_stats;
}

/* ---- the directory ---------------------------------------------------------- */

static void scan_dir(dl_scan_t *scan, const char *prefix)
{
    DIR *dir = opendir(DL_DIR);

    dl_scan_init(scan, prefix);

    if (dir == NULL)
    {
        return;
    }

    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL)
    {
        dl_scan_add(scan, ent->d_name);
    }

    closedir(dir);
}

static void enforce_retention(dl_stream_t *st)
{
    dl_scan_t scan;

    scan_dir(&scan, st->prefix);
    st->files = (uint32_t)scan.count;

    while (scan.count > (int)st->max_files)
    {
        char path[80];

        snprintf(path, sizeof(path), DL_DIR "/%s", scan.oldest);

        if (unlink(path) != 0)
        {
            ESP_LOGW(TAG, "retention unlink %s failed", path);
            break;
        }

        ESP_LOGI(TAG, "retention: deleted %s", scan.oldest);
        scan_dir(&scan, st->prefix);
        st->files = (uint32_t)scan.count;
    }
}

/* set-aside files (<name>.corrupt) of one stream; the oldest by name */
static uint32_t count_corrupt(const char *prefix, char *oldest, size_t cap)
{
    DIR *dir = opendir(DL_DIR);
    uint32_t n = 0;

    if (oldest != NULL && cap > 0)
    {
        oldest[0] = '\0';
    }

    if (dir == NULL)
    {
        return 0;
    }

    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL)
    {
        if (!dl_recover_is_corrupt_name(ent->d_name, prefix, NULL))
        {
            continue;
        }

        n++;

        if (oldest != NULL && cap > strlen(ent->d_name) &&
            (oldest[0] == '\0' || strcmp(ent->d_name, oldest) < 0))
        {
            strcpy(oldest, ent->d_name);
        }
    }

    closedir(dir);
    return n;
}

/* both streams' set-aside counts, the stream's own into its stats */
static void count_corrupt_all(dl_stream_t *st)
{
    uint32_t params = count_corrupt(DL_PREFIX_PARAM, NULL, 0);
    uint32_t can = count_corrupt(DL_PREFIX_CAN, NULL, 0);

    st->corrupt_files = st->frames ? can : params;
    s_stats.corrupt = params + can;
}

/* the newest file is resumable only if the current engine wrote it */
static bool ext_matches(const char *fname, const char *ext)
{
    size_t fl = strlen(fname);
    size_t el = strlen(ext);

    return fl > el && strcmp(fname + fl - el, ext) == 0;
}

/* ---- faults ------------------------------------------------------------------ */

/* A dev_status fault latches to NVS: a flash write. The writer task's
 * stack is in PSRAM, and a task on a PSRAM stack must never write flash:
 * the cache is off during the write and the stack is gone with it
 * (cache_utils.c:126 assert, three panics on the bench, 2026-09-07).
 * Raise from a short-lived task on an internal stack instead. */
typedef struct
{
    char code[24];
    char detail[48];
} dl_fault_msg_t;

static void fault_task(void *arg)
{
    dl_fault_msg_t *m = arg;

    dev_status_manager_fault_raise(m->code, m->detail);
    heap_caps_free(m);
    vTaskDelete(NULL);
}

void dl_store_fault(const char *code, const char *detail)
{
    dl_fault_msg_t *m = heap_caps_malloc(sizeof(*m),
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (m == NULL)
    {
        return;
    }

    snprintf(m->code, sizeof(m->code), "%s", code);
    snprintf(m->detail, sizeof(m->detail), "%s", (detail != NULL) ? detail : "");

    if (xTaskCreate(fault_task, "dl_fault", 4096, m, 2, NULL) != pdPASS)
    {
        heap_caps_free(m);
        ESP_LOGW(TAG, "fault %s not latched (no task)", code);
    }
}

/* ---- open / close / rotate ------------------------------------------------------ */

/* open the newest existing file, or a fresh one when none/rotating */
esp_err_t dl_store_open(dl_stream_t *st, bool force_new)
{
    dl_scan_t scan;
    char fname[48];

    if (st->eng->single_use)
    {
        force_new = true; /* relative-time / patched-header formats */
    }

    mkdir(DL_DIR, 0775);
    scan_dir(&scan, st->prefix);

    /* a boot without a valid clock (1970 + uptime) never appends to a file
       whose rows were stamped by a real clock: it starts its own, and the
       first valid clock rotates again (README "Time") */
    bool resume = !force_new && scan.count > 0 &&
                  ext_matches(scan.newest, st->eng->ext) &&
                  rtc_manager_time_valid();

    if (resume && st->eng->resume_max != 0)
    {
        struct stat sb;
        char p[80];

        snprintf(p, sizeof(p), DL_DIR "/%s", scan.newest);

        if (stat(p, &sb) == 0 && (uint64_t)sb.st_size > st->eng->resume_max)
        {
            ESP_LOGI(TAG, "%s is too big for the torn-tail check; starting "
                     "a new file", scan.newest);
            resume = false;
        }
    }

    if (resume)
    {
        strcpy(fname, scan.newest);
    }
    else
    {
        int64_t epoch = (int64_t)time(NULL);
        int64_t newest_epoch = 0;

        /* clock rolled back / rotated within 1 s: keep names monotonic
           (a backward clock step rotates into newest + 1 as well: the
           journal says what the clock did, the order on the card stays
           the order of writing) */
        if (scan.count > 0 && dl_files_parse(scan.newest, &newest_epoch) &&
            epoch <= newest_epoch)
        {
            epoch = newest_epoch + 1;
        }

        dl_files_make(fname, sizeof(fname), st->prefix, epoch,
                      st->eng->ext);
    }

    snprintf(st->cur_path, sizeof(st->cur_path), DL_DIR "/%s", fname);
    snprintf(st->last_try, sizeof(st->last_try), "%s", fname);

    esp_err_t err = st->eng->open(st->ctx, st->cur_path, st->frames);

    if (err != ESP_OK)
    {
        st->cur_path[0] = '\0';
        return err;
    }

    if (!st->frames)
    {
        dl_core_forget_db_ids(); /* interned per open file */
    }

    snprintf(st->file, sizeof(st->file), "%s", fname);
    st->file_rows = 0;
    enforce_retention(st);
    count_corrupt_all(st);
    ESP_LOGI(TAG, "logging to %s (rotate at %lu MB)", fname,
             (unsigned long)st->max_mb);
    return ESP_OK;
}

void dl_store_close(dl_stream_t *st)
{
    st->eng->close(st->ctx);
    st->cur_path[0] = '\0';
    st->file[0] = '\0';
}

void dl_store_rotate(dl_stream_t *st)
{
    char rotated[48];

    snprintf(rotated, sizeof(rotated), "%s", st->file);
    dl_store_close(st);
    st->rotations++;

    if (dl_store_open(st, true) == ESP_OK)
    {
        dl_events_rotated(rotated);
    }
}

/* ---- corrupt files (ROBUSTNESS.md case 5) ---------------------------------------- */

/* keep at most DL_CORRUPT_KEEP set-aside files per stream */
static void prune_corrupt(dl_stream_t *st)
{
    char oldest[64];

    for (int guard = 0; guard < 8; guard++)
    {
        uint32_t n = count_corrupt(st->prefix, oldest, sizeof(oldest));

        st->corrupt_files = n;

        if (n <= DL_CORRUPT_KEEP || oldest[0] == '\0')
        {
            return;
        }

        char path[96];

        snprintf(path, sizeof(path), DL_DIR "/%s", oldest);

        if (unlink(path) != 0)
        {
            return;
        }

        ESP_LOGI(TAG, "set-aside cap: deleted %s", oldest);
    }
}

/* The file just failed as CORRUPT: set it aside as <name>.corrupt, drop
 * its journal, latch a fault, copy what sqlite can still read into a
 * fresh file with the old name, and go on in a new file. Never retry a
 * bad file: that was the loop that dropped every record. */
void dl_store_quarantine(dl_stream_t *st)
{
    char orig[48], bad[64], origpath[96], badpath[96], jpath[112];

    snprintf(orig, sizeof(orig), "%s",
             st->file[0] != '\0' ? st->file : st->last_try);
    dl_store_close(st);

    if (orig[0] == '\0' || !dl_recover_corrupt_name(orig, bad, sizeof(bad)))
    {
        return;
    }

    snprintf(origpath, sizeof(origpath), DL_DIR "/%s", orig);
    snprintf(badpath, sizeof(badpath), DL_DIR "/%s", bad);
    snprintf(jpath, sizeof(jpath), "%s-journal", origpath);
    unlink(badpath); /* an older set-aside copy of the same name */

    if (rename(origpath, badpath) != 0)
    {
        ESP_LOGE(TAG, "%s: cannot set aside (errno %d); deleting it so "
                 "logging can go on", orig, errno);
        unlink(origpath);
        bad[0] = '\0';
    }

    unlink(jpath); /* a hot journal must never meet a new file of that name */
    dl_store_fault("logger_file_corrupt", orig);
    s_stats.corrupt++;
    dl_events_error("corrupt", (int)s_stats.corrupt);
    ESP_LOGE(TAG, "%s is corrupt: set aside as %s; logging continues in a "
             "fresh file", orig, bad[0] != '\0' ? bad : "(deleted)");
    prune_corrupt(st);

    if (bad[0] != '\0' && st->eng == &dl_engine_sqlite)
    {
        uint32_t rows = 0;

        if (dl_sq_salvage(badpath, origpath, st->frames, 120000, &rows) ==
                ESP_OK && rows > 0)
        {
            ESP_LOGW(TAG, "salvaged %lu row(s) from %s into %s",
                     (unsigned long)rows, bad, orig);
        }
    }

    if (dl_store_open(st, true) != ESP_OK)
    {
        s_stats.errors++;
    }
}

/* open the stream's file; a corrupt one is set aside on the spot; repeated
 * failures latch a fault and slow the retries down (case 10) */
bool dl_store_ensure_open(dl_stream_t *st)
{
    if (st->eng->is_open(st->ctx))
    {
        return true;
    }

    if (dl_store_open(st, false) == ESP_OK)
    {
        st->open_fails = 0;
        return true;
    }

    if (st->eng->corrupt != NULL && st->eng->corrupt(st->ctx))
    {
        dl_store_quarantine(st);

        if (st->eng->is_open(st->ctx))
        {
            st->open_fails = 0;
            return true;
        }
    }

    s_stats.errors++;

    if (++st->open_fails == DL_OPEN_FAILS_FAULT)
    {
        ESP_LOGE(TAG, "%s stream: cannot open a file on the card; backing "
                 "off to 10 s retries", st->frames ? "CAN" : "params");
        dl_store_fault("logger_storage_error",
                       st->frames ? "can" : "params");
    }

    return false;
}
