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
 * @file data_logger_writer.c
 * @brief The writer task and its two streams: the engine binding, the
 *        batches that drain the core's rings into the open files, the
 *        runtime gate, and the hooks the clock journal uses to mark or
 *        rotate the open files (split out of data_logger.c 2026-10-13).
 *        The writer is the ONLY code that opens files (through the store,
 *        data_logger_store.c): it follows external_storage_is_mounted(),
 *        so a yanked card just parks the writer while the rings keep
 *        absorbing (drop-oldest).
 */
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "external_storage.h"

#include "data_logger_private.h"

static const char *TAG = "data_logger";

/* writer task. PSRAM stack per the standard: it only touches the SD
 * card via SDMMC (never internal flash), so the 2 corollary doesn't
 * apply, and 10 KB of internal RAM is exactly what broke the build
 * that kept it internal (bench 2026-07-07: ~2.5 KB internal free =
 * wifi auth AND sdmmc DMA allocations failing). */
static TaskHandle_t s_task;
static StaticTask_t s_tcb;             /* internal: FreeRTOS object */
static StackType_t s_stack[10240] EXT_RAM_BSS_ATTR;

/* ---- streams ---------------------------------------------------------------- */

/* engine contexts: one per (engine kind × stream) so any format combo
 * can have two files open at once */
static dl_append_ctx_t s_ap_ctx[DL_ST_COUNT];
static dl_sq_ctx_t s_sq_ctx[DL_ST_COUNT];
static dl_mf4_ctx_t s_mf4_ctx;   /* CAN stream only (schema)          */
static dl_blf_ctx_t s_blf_ctx;

static dl_stream_t s_stream[DL_ST_COUNT] =
{
    { .prefix = DL_PREFIX_PARAM, .frames = false },
    { .prefix = DL_PREFIX_CAN, .frames = true },
};

static volatile bool s_run;
static volatile bool s_gate = true;  /* logger.enable/disable rules */
static volatile uint32_t s_park_seq; /* bumps each time the writer parks with
                                        both files closed (stop() waits on it) */

static void bind_engine(int idx, const char *fmt)
{
    dl_stream_t *st = &s_stream[idx];

    if (strcmp(fmt, "csv") == 0)
    {
        st->eng = &dl_engine_csv;
        st->ctx = &s_ap_ctx[idx];
    }
    else if (strcmp(fmt, "binary") == 0)
    {
        st->eng = &dl_engine_binary;
        st->ctx = &s_ap_ctx[idx];
    }
    else if (strcmp(fmt, "candump") == 0)
    {
        st->eng = &dl_engine_candump;
        st->ctx = &s_ap_ctx[idx];
    }
    else if (strcmp(fmt, "asc") == 0)
    {
        st->eng = &dl_engine_asc;
        st->ctx = &s_ap_ctx[idx];
    }
    else if (strcmp(fmt, "jsonl") == 0)
    {
        st->eng = &dl_engine_jsonl;
        st->ctx = &s_ap_ctx[idx];
    }
    else if (strcmp(fmt, "mf4") == 0)
    {
        st->eng = &dl_engine_mf4;
        st->ctx = &s_mf4_ctx;
    }
    else if (strcmp(fmt, "blf") == 0)
    {
        st->eng = &dl_engine_blf;
        st->ctx = &s_blf_ctx;
    }
    else
    {
        st->eng = &dl_engine_sqlite;
        st->ctx = &s_sq_ctx[idx];
    }
}

/* on_apply (through dl_core_apply): engine binding, rotation/retention,
 * which streams are active this boot */
void dl_writer_bind(const dl_cfg_t *cfg)
{
    bind_engine(DL_ST_PARAM, cfg->format);
    bind_engine(DL_ST_CAN, cfg->can_format);

    s_stream[DL_ST_PARAM].max_mb = cfg->max_file_mb;
    s_stream[DL_ST_PARAM].max_files = cfg->max_files;
    s_stream[DL_ST_CAN].max_mb = cfg->can_max_file_mb;
    s_stream[DL_ST_CAN].max_files = cfg->can_max_files;
    s_stream[DL_ST_PARAM].active = cfg->enabled;
    s_stream[DL_ST_CAN].active = cfg->enabled && cfg->can.log;
}

bool dl_writer_stream_active(int st)
{
    return st >= 0 && st < DL_ST_COUNT && s_stream[st].active;
}

const char *dl_writer_engine_name(int st)
{
    if (st < 0 || st >= DL_ST_COUNT || s_stream[st].eng == NULL)
    {
        return "?";
    }

    return s_stream[st].eng->ext + 1; /* past the dot */
}

static void close_all(void)
{
    for (int i = 0; i < DL_ST_COUNT; i++)
    {
        if (s_stream[i].eng->is_open(s_stream[i].ctx))
        {
            dl_store_close(&s_stream[i]);
        }
    }
}

/* ---- the clock's hooks (data_logger_journal.c calls them) ------------------- */

int dl_writer_open_files(const char **names, int cap)
{
    int n = 0;

    for (int i = 0; i < DL_ST_COUNT && n < cap; i++)
    {
        if (s_stream[i].eng->is_open(s_stream[i].ctx) &&
            s_stream[i].file[0] != '\0')
        {
            names[n++] = s_stream[i].file;
        }
    }

    return n;
}

/* an in-band marker where the engine has one; a .wdl takes it only as
 * the last record before a close */
static void mark_stream(dl_stream_t *st, const dl_clock_step_t *step,
                        bool closing)
{
    if (st->eng->mark == NULL || !st->eng->is_open(st->ctx) ||
        (st->eng->mark_close_only && !closing))
    {
        return;
    }

    if (st->eng->mark(st->ctx, step) != ESP_OK)
    {
        dl_store_stats()->errors++;
    }
}

int dl_writer_clock_rotate(const dl_clock_step_t *step, dl_rotated_t out[2])
{
    int n = 0;

    for (int i = 0; i < DL_ST_COUNT; i++)
    {
        dl_stream_t *st = &s_stream[i];

        if (!st->active || !st->eng->is_open(st->ctx))
        {
            continue;
        }

        mark_stream(st, step, true);
        out[n].stream = st->frames ? "can" : "params";
        snprintf(out[n].from, sizeof(out[n].from), "%s", st->file);
        dl_store_rotate(st); /* closed, reopened under the corrected clock's name */
        snprintf(out[n].to, sizeof(out[n].to), "%s", st->file);
        n++;
    }

    return n;
}

void dl_writer_clock_mark(const dl_clock_step_t *step)
{
    for (int i = 0; i < DL_ST_COUNT; i++)
    {
        if (s_stream[i].active)
        {
            mark_stream(&s_stream[i], step, false);
        }
    }
}

/* ---- the batches ---------------------------------------------------------------- */

/* drain up to batch_rows records of stream @p idx inside one
 * transaction/flush */
static void write_batch(int idx)
{
    dl_stream_t *st = &s_stream[idx];
    dl_record_t rec;
    uint32_t n = 0;
    bool failed = false;
    uint32_t batch_rows = dl_settings_config()->batch_rows;

    /* a clock step since the last batch: journal, markers, rotation; the
       records below are then re-based one by one (outside any batch) */
    dl_journal_lap();

    if (!st->eng->is_open(st->ctx) || st->eng->begin(st->ctx) != ESP_OK)
    {
        failed = true;
    }

    while (!failed && n < batch_rows && dl_core_pop(idx, &rec))
    {
        dl_param_entry_t *p =
            (rec.kind == DL_REC_PARAM) ? dl_core_param(rec.u.p.param) : NULL;

        dl_journal_rebase(&rec);

        if (st->eng->write(st->ctx, &rec, p) != ESP_OK)
        {
            failed = true;
            break;
        }

        n++;
    }

    if (!failed && st->eng->commit(st->ctx) != ESP_OK)
    {
        failed = true;
    }

    if (failed)
    {
        dl_store_stats()->errors++;
        dl_events_error("write", (int)dl_store_stats()->errors);

        if (st->eng->corrupt != NULL && st->eng->corrupt(st->ctx))
        {
            dl_store_quarantine(st); /* set aside + fresh file: never retry a bad file */
            return;
        }

        /* card yanked mid-write, disk full: drop the file handle and let
         * the mount/open path recover next lap */
        dl_store_close(st);
        return;
    }

    st->written += n;
    st->file_rows += n;

    if (st->eng->bytes(st->ctx) >=
        (uint64_t)st->max_mb * 1024u * 1024u)
    {
        dl_store_rotate(st);
    }
}

static void writer_task(void *arg)
{
    const dl_cfg_t *cfg = dl_settings_config();
    dl_store_stats_t *ss = dl_store_stats();

    (void)arg;

    while (true)
    {
        if (!s_run || !s_gate)
        {
            /* stopped, or paused by a logger.disable rule: the rings
             * keep absorbing (drop-oldest), so re-enabling flushes
             * the newest records from before the trigger */
            close_all();
            ss->storage_ok = false;
            s_park_seq++; /* dl_writer_stop() waits for this */
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200)); /* resume promptly too */
            continue;
        }

        if (!external_storage_is_mounted())
        {
            if (s_stream[DL_ST_PARAM].eng->is_open(s_stream[DL_ST_PARAM].ctx) ||
                s_stream[DL_ST_CAN].eng->is_open(s_stream[DL_ST_CAN].ctx))
            {
                ESP_LOGW(TAG, "card gone; parking");
                close_all();
            }

            ss->storage_ok = false;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        bool all_open = true;
        bool backoff = false;

        for (int i = 0; i < DL_ST_COUNT; i++)
        {
            dl_stream_t *st = &s_stream[i];

            if (st->active && !dl_store_ensure_open(st))
            {
                all_open = false;
                backoff = backoff || st->open_fails >= DL_OPEN_FAILS_FAULT;
            }
        }

        if (!all_open)
        {
            ss->storage_ok = false;
            vTaskDelay(pdMS_TO_TICKS(backoff ? 10000 : 2000));
            continue;
        }

        ss->storage_ok = true;

        /* a quiet logger still journals a clock step and rotates on it
           without waiting for the next record */
        dl_journal_lap();

        if (dl_core_fill(DL_ST_PARAM) >= cfg->batch_rows ||
            dl_core_fill(DL_ST_CAN) >= cfg->batch_rows)
        {
            /* keep draining hot rings without the nap */
            if (dl_core_fill(DL_ST_PARAM) >= cfg->batch_rows)
            {
                write_batch(DL_ST_PARAM);
            }

            if (dl_core_fill(DL_ST_CAN) >= cfg->batch_rows &&
                s_stream[DL_ST_CAN].active)
            {
                write_batch(DL_ST_CAN);
            }

            continue;
        }

        /* the nap ends early when the gate flips (dl_writer_set_gate
           notifies): a pause (a rule, /api/logger/gate or the export
           reading the active file) closes the files right away instead of
           up to flush_ms later (2026-09-06; the export used to find the
           newest file still locked and return nothing) */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(cfg->flush_ms));

        if (!s_run || !s_gate)
        {
            continue; /* gate flipped during the nap: park FIRST, a
                         "paused" logger must not leak a batch */
        }

        if (dl_core_fill(DL_ST_PARAM) > 0 && s_stream[DL_ST_PARAM].active)
        {
            write_batch(DL_ST_PARAM);
        }

        if (dl_core_fill(DL_ST_CAN) > 0 && s_stream[DL_ST_CAN].active)
        {
            write_batch(DL_ST_CAN);
        }
    }
}

/* ---- lifecycle (called by data_logger.c) ------------------------------------- */

esp_err_t dl_writer_start(void)
{
    s_run = true;

    if (s_task == NULL)
    {
        s_task = xTaskCreateStatic(writer_task, "dl_writer",
                                   sizeof(s_stack) / sizeof(s_stack[0]),
                                   NULL, 3, s_stack, &s_tcb);

        if (s_task == NULL)
        {
            return ESP_FAIL;
        }
    }

    if (s_stream[DL_ST_CAN].active && dl_can_start() != ESP_OK)
    {
        ESP_LOGW(TAG, "CAN drain task failed; can stream off");
        s_stream[DL_ST_CAN].active = false;
    }

    return ESP_OK;
}

void dl_writer_stop(void)
{
    uint32_t seq = s_park_seq;

    s_run = false;

    if (s_task == NULL)
    {
        return;
    }

    /* synchronous since 2026-09-07 (ROBUSTNESS.md cases 7/8): sleep entry
       unmounts the card right after this and a restart cuts the IO,
       wait for the writer to finish its batch and close both files */
    xTaskNotifyGive(s_task);

    for (int i = 0; i < 300 && s_park_seq == seq; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (s_park_seq == seq)
    {
        ESP_LOGW(TAG, "stop: the writer did not park within 3 s");
    }
    else
    {
        ESP_LOGI(TAG, "stopped: log files flushed and closed");
    }
}

void dl_writer_set_gate(bool on)
{
    if (s_gate != on)
    {
        ESP_LOGI(TAG, "%s by rule", on ? "enabled" : "paused");
    }

    s_gate = on;

    if (s_task != NULL)
    {
        xTaskNotifyGive(s_task);   /* wake the writer out of its nap */
    }
}

bool dl_writer_gated(void)
{
    return !s_gate;
}

void dl_writer_stats(data_logger_stats_t *out)
{
    const dl_stream_t *p = &s_stream[DL_ST_PARAM];
    const dl_stream_t *c = &s_stream[DL_ST_CAN];
    const dl_store_stats_t *ss = dl_store_stats();

    snprintf(out->file, sizeof(out->file), "%s", p->file);
    out->file_rows = p->file_rows;
    out->files = p->files;
    out->rotations = p->rotations;
    out->written = p->written;

    out->can_enabled = c->active;
    snprintf(out->can_file, sizeof(out->can_file), "%s", c->file);
    out->can_file_rows = c->file_rows;
    out->can_files = c->files;
    out->can_rotations = c->rotations;
    out->frames_written = c->written;

    out->storage_ok = ss->storage_ok;
    out->errors = ss->errors;
    out->corrupt = ss->corrupt;
}
