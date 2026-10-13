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
 * @file data_logger_append.c
 * @brief The two append-log storage engines (meatpi 2026-07-07: sqlite
 *        stays for tooling compatibility, these are the fast options,
 *        BENCHMARKS.md measured raw append at ~170x tuned sqlite).
 *
 * Engines take a dl_append_ctx_t since 2026-07-09 so the param and CAN
 * streams can have two files open at once (TASK addendum A1).
 *
 * csv    param stream: `ts_ms,source.name,value` rows under a header;
 *        CAN stream:   `ts_ms,id,ext,rtr,dlc,data` (hex id + payload).
 * binary dl/can_<epoch>.wdl: "WDL1" magic then framed records:
 *          0x01 def:   u16 id, u8 len, "source.name" (first use only)
 *          0x02 param: i64 ts_ms, u16 id, f64 value  (little-endian)
 *          0x03 frame: i64 ts_ms, u32 id_word, u8 dlc, data[dlc]
 *          0x04 clock: i64 ts_ms, i64 delta_ms, u8 source (2026-10-13:
 *               the clock stepped here; only ever the last record of a
 *               file, so an older reader stops at it as at a torn tail)
 *        Self-contained (the dictionary is inline); a torn tail frame
 *        is detectable by framing. Decoder: tools/wdl_dump.py.
 *
 * Both buffer and fflush+fsync only on commit(), so a batch costs one
 * card touch like a sqlite transaction does. A clock-step marker (csv
 * params, jsonl, the .wdl record) is written and made durable by the
 * mark() call itself, outside any batch.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <unistd.h>

#include "data_logger_private.h"

static const char *TAG = "data_logger";

#define AP_BUF_SZ 4096

/* A torn tail from a power cut mid-write (ROBUSTNESS.md cases 2/3): a text
 * file is cut back to its last newline, a .wdl to its last complete
 * record. Done on a read handle BEFORE the append handle exists (FS_LOCK
 * refuses a second open once we hold the file for writing); the caller's
 * DMA-capable stdio buffer serves the read handle, the scan data sits in
 * PSRAM. */
static void ap_repair_tail(const char *path, uint8_t fmt, char *stdio_buf)
{
    struct stat sb;

    if (stat(path, &sb) != 0 || sb.st_size <= 0)
    {
        return; /* a new file */
    }

    uint64_t size = (uint64_t)sb.st_size;
    uint64_t keep = size;
    uint8_t *buf = heap_caps_malloc(AP_BUF_SZ,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    FILE *r = (buf != NULL) ? fopen(path, "rb") : NULL;

    if (r == NULL)
    {
        heap_caps_free(buf);
        return;
    }

    setvbuf(r, stdio_buf, _IOFBF, AP_BUF_SZ);

    if (fmt == DL_AP_WDL)
    {
        if (size < 4)
        {
            keep = 0; /* not even the WDL1 header */
        }
        else
        {
            size_t carry = 0;
            bool bad = false;

            keep = 4;
            fseek(r, 4, SEEK_SET);

            while (!bad)
            {
                size_t n = fread(buf + carry, 1, AP_BUF_SZ - carry, r);

                if (n == 0)
                {
                    break;
                }

                size_t avail = carry + n;
                size_t done = dl_recover_wdl_scan(buf, avail, &bad);

                keep += done;
                carry = avail - done;

                if (carry > 260) /* longer than any record: garbage */
                {
                    bad = true;
                    break;
                }

                memmove(buf, buf + done, carry);
            }
        }
    }
    else
    {
        size_t n = (size > AP_BUF_SZ) ? AP_BUF_SZ : (size_t)size;

        fseek(r, (long)(size - n), SEEK_SET);

        if (fread(buf, 1, n, r) == n)
        {
            keep = (size - n) + dl_recover_text_keep((const char *)buf, n);
        }
    }

    fclose(r);
    heap_caps_free(buf);

    if (keep < size)
    {
        if (truncate(path, (off_t)keep) == 0)
        {
            ESP_LOGW(TAG, "%s: dropped %llu torn byte(s) at the tail", path,
                     (unsigned long long)(size - keep));
        }
        else
        {
            ESP_LOGE(TAG, "%s: truncate failed (errno %d)", path, errno);
        }
    }
}

/* The stdio buffer must be INTERNAL (DMA-capable), deliberately not
 * PSRAM: FATFS hands big flushes straight to sdmmc, and a non-DMA
 * buffer makes the driver heap-allocate a bounce buffer PER TRANSFER,
 * which fails intermittently under load (bench 2026-07-07: EIO every
 * few thousand rows). Allocated at open / freed at close so an idle
 * stream costs zero internal RAM (~25 KB free at steady state). */

/* the ASC header carries an absolute date; rows are relative seconds */
static int ap_asc_header(FILE *f, int64_t start_ms)
{
    time_t sec = (time_t)(start_ms / 1000);
    struct tm tmv;
    char date[48];

    gmtime_r(&sec, &tmv);
    strftime(date, sizeof(date), "%a %b %d %I:%M:%S", &tmv);
    return fprintf(f, "date %s.%03d %s %04d\n"
                   "base hex  timestamps absolute\n"
                   "internal events logged\n",
                   date, (int)(start_ms % 1000),
                   (tmv.tm_hour < 12) ? "am" : "pm",
                   tmv.tm_year + 1900);
}

static esp_err_t ap_open(dl_append_ctx_t *c, const char *path,
                         uint8_t fmt, bool frames)
{
    if (c->f != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    c->buf = heap_caps_malloc(AP_BUF_SZ,
                              MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

    if (c->buf == NULL)
    {
        ESP_LOGE(TAG, "no internal RAM for the stdio buffer");
        return ESP_ERR_NO_MEM;
    }

    ap_repair_tail(path, fmt, c->buf);

    FILE *f = fopen(path, "a");

    if (f == NULL)
    {
        ESP_LOGE(TAG, "open %s failed (errno %d)", path, errno);
        heap_caps_free(c->buf);
        c->buf = NULL;
        return ESP_FAIL;
    }

    struct timeval tv;

    gettimeofday(&tv, NULL);
    setvbuf(f, c->buf, _IOFBF, AP_BUF_SZ);
    fseek(f, 0, SEEK_END);
    c->f = f;
    c->bytes = (uint64_t)ftell(f);
    c->fmt = fmt;
    c->frames = frames;
    c->start_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    c->mono_open = (uint32_t)(esp_timer_get_time() / 1000);

    if (c->bytes == 0)
    {
        int n = 0;

        switch (fmt)
        {
        case DL_AP_CSV:
            n = fprintf(f, frames ? "ts_ms,id,ext,rtr,dlc,data\n"
                                  : "ts_ms,param,value\n");
            break;
        case DL_AP_WDL:
            n = fprintf(f, "WDL1");
            break;
        case DL_AP_ASC:
            n = ap_asc_header(f, c->start_ms);
            break;
        default: /* candump / jsonl: headerless */
            break;
        }

        if (n < 0)
        {
            fclose(f);
            c->f = NULL;
            heap_caps_free(c->buf);
            c->buf = NULL;
            return ESP_FAIL;
        }

        c->bytes += (uint64_t)n;
    }

    return ESP_OK;
}

static void ap_close(void *ctx)
{
    dl_append_ctx_t *c = ctx;

    if (c->f != NULL)
    {
        fclose((FILE *)c->f);
        c->f = NULL;
    }

    if (c->buf != NULL)
    {
        heap_caps_free(c->buf);
        c->buf = NULL;
    }
}

static bool ap_is_open(void *ctx)
{
    return ((dl_append_ctx_t *)ctx)->f != NULL;
}

static esp_err_t ap_begin(void *ctx)
{
    return (((dl_append_ctx_t *)ctx)->f != NULL) ? ESP_OK
                                                 : ESP_ERR_INVALID_STATE;
}

static esp_err_t ap_commit(void *ctx)
{
    dl_append_ctx_t *c = ctx;

    if (c->f == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (fflush((FILE *)c->f) != 0)
    {
        ESP_LOGE(TAG, "fflush failed (errno %d)", errno);
        return ESP_FAIL;
    }

    if (fsync(fileno((FILE *)c->f)) != 0)
    {
        ESP_LOGE(TAG, "fsync failed (errno %d)", errno);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static uint64_t ap_bytes(void *ctx)
{
    return ((dl_append_ctx_t *)ctx)->bytes;
}

/* one row or record written and made durable on the spot */
static esp_err_t ap_put_durable(dl_append_ctx_t *c, const void *data,
                                size_t n)
{
    if (c->f == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (fwrite(data, 1, n, (FILE *)c->f) != n)
    {
        return ESP_FAIL;
    }

    c->bytes += n;
    return ap_commit(c);
}

/* ---- csv ------------------------------------------------------------------ */

static esp_err_t csv_open(void *ctx, const char *path, bool frames)
{
    return ap_open(ctx, path, DL_AP_CSV, frames);
}

static esp_err_t csv_write(void *ctx, const dl_record_t *rec,
                           dl_param_entry_t *p)
{
    dl_append_ctx_t *c = ctx;
    int n;

    if (rec->kind == DL_REC_FRAME)
    {
        char row[64];

        n = dl_csv_frame_row(row, sizeof(row), rec->ts_ms, rec->u.f.id,
                             rec->u.f.flags, rec->u.f.data,
                             rec->u.f.dlc);

        if (n <= 0 || fwrite(row, 1, (size_t)n, (FILE *)c->f) !=
                          (size_t)n)
        {
            return ESP_FAIL;
        }
    }
    else
    {
        n = fprintf((FILE *)c->f, "%lld,%s.%s,%.6g\n",
                    (long long)rec->ts_ms, p->source, p->name,
                    rec->u.p.value);

        if (n <= 0)
        {
            return ESP_FAIL;
        }
    }

    c->bytes += (uint64_t)n;
    return ESP_OK;
}

/* the params csv has a row for it; the CAN csv has no column (journal only) */
static esp_err_t csv_mark(void *ctx, const dl_clock_step_t *step)
{
    dl_append_ctx_t *c = ctx;
    char row[64];

    if (c->frames)
    {
        return ESP_OK;
    }

    int n = dl_csv_step_row(row, sizeof(row), step);

    return (n > 0) ? ap_put_durable(c, row, (size_t)n) : ESP_FAIL;
}

const dl_engine_t dl_engine_csv =
{
    .ext = ".csv",
    .open = csv_open,
    .close = ap_close,
    .is_open = ap_is_open,
    .begin = ap_begin,
    .write = csv_write,
    .commit = ap_commit,
    .bytes = ap_bytes,
    .mark = csv_mark,
};

/* ---- binary ---------------------------------------------------------------- */

static esp_err_t bin_open(void *ctx, const char *path, bool frames)
{
    esp_err_t err = ap_open(ctx, path, DL_AP_WDL, frames);

    /* ids restart per open, definitions re-emitted on first use: a
     * resumed file carries a second dictionary block; a def frame
     * applies to the records AFTER it, so sequential readers stay
     * correct */
    ((dl_append_ctx_t *)ctx)->next_id = 0;
    return err;
}

static size_t put_u16(uint8_t *b, uint16_t v)
{
    b[0] = (uint8_t)(v & 0xFF);
    b[1] = (uint8_t)(v >> 8);
    return 2;
}

static esp_err_t bin_write(void *ctx, const dl_record_t *rec,
                           dl_param_entry_t *p)
{
    dl_append_ctx_t *c = ctx;

    if (rec->kind == DL_REC_FRAME)
    {
        uint8_t frame[24];
        size_t n = dl_wdl_encode_frame(frame, rec->ts_ms, rec->u.f.id,
                                       rec->u.f.flags, rec->u.f.data,
                                       rec->u.f.dlc);

        if (fwrite(frame, 1, n, (FILE *)c->f) != n)
        {
            return ESP_FAIL;
        }

        c->bytes += n;
        return ESP_OK;
    }

    uint8_t frame[1 + 8 + 2 + 8];
    size_t off;

    if (p->db_id <= 0)
    {
        char full[DL_SOURCE_MAX + DL_NAME_MAX + 1];
        uint8_t def[1 + 2 + 1];

        p->db_id = (int)(++c->next_id);
        snprintf(full, sizeof(full), "%s.%s", p->source, p->name);
        def[0] = 0x01;
        put_u16(&def[1], (uint16_t)p->db_id);
        def[3] = (uint8_t)strlen(full);

        if (fwrite(def, 1, sizeof(def), (FILE *)c->f) != sizeof(def) ||
            fwrite(full, 1, def[3], (FILE *)c->f) != def[3])
        {
            return ESP_FAIL;
        }

        c->bytes += sizeof(def) + def[3];
    }

    frame[0] = 0x02;
    off = 1;
    memcpy(&frame[off], &rec->ts_ms, 8);   /* LE on xtensa/riscv */
    off += 8;
    off += put_u16(&frame[off], (uint16_t)p->db_id);
    memcpy(&frame[off], &rec->u.p.value, 8);
    off += 8;

    if (fwrite(frame, 1, off, (FILE *)c->f) != off)
    {
        return ESP_FAIL;
    }

    c->bytes += off;
    return ESP_OK;
}

/* the 0x04 record: only as the last one before a close (mark_close_only) */
static esp_err_t bin_mark(void *ctx, const dl_clock_step_t *step)
{
    uint8_t rec[DL_WDL_STEP_LEN];
    size_t n = dl_wdl_encode_step(rec, step);

    return ap_put_durable(ctx, rec, n);
}

const dl_engine_t dl_engine_binary =
{
    .ext = ".wdl",
    .resume_max = 16u * 1024u * 1024u, /* the torn-tail check reads it all */
    .open = bin_open,
    .close = ap_close,
    .is_open = ap_is_open,
    .begin = ap_begin,
    .write = bin_write,
    .commit = ap_commit,
    .bytes = ap_bytes,
    .mark = bin_mark,
    .mark_close_only = true,
};

/* ---- candump / asc / jsonl (addendum 2) -------------------------------------

   Text rows via the pure encoders in data_logger_files.c; same
   buffered-append + fflush/fsync-per-commit contract as csv. Each
   engine instance only ever sees its stream's record kind (the rings
   route by kind): a mismatched record is skipped defensively. */

static esp_err_t row_write(dl_append_ctx_t *c, const char *row, int n)
{
    if (n <= 0)
    {
        return ESP_FAIL;
    }

    if (fwrite(row, 1, (size_t)n, (FILE *)c->f) != (size_t)n)
    {
        return ESP_FAIL;
    }

    c->bytes += (uint64_t)n;
    return ESP_OK;
}

static esp_err_t candump_open(void *ctx, const char *path, bool frames)
{
    return ap_open(ctx, path, DL_AP_CANDUMP, frames);
}

static esp_err_t candump_write(void *ctx, const dl_record_t *rec,
                               dl_param_entry_t *p)
{
    (void)p;

    if (rec->kind != DL_REC_FRAME)
    {
        return ESP_OK; /* frames-only format */
    }

    char row[64];

    return row_write(ctx, row,
                     dl_candump_row(row, sizeof(row), rec->ts_ms,
                                    rec->u.f.id, rec->u.f.flags,
                                    rec->u.f.data, rec->u.f.dlc));
}

const dl_engine_t dl_engine_candump =
{
    .ext = ".log",
    .open = candump_open,
    .close = ap_close,
    .is_open = ap_is_open,
    .begin = ap_begin,
    .write = candump_write,
    .commit = ap_commit,
    .bytes = ap_bytes,
};

static esp_err_t asc_open(void *ctx, const char *path, bool frames)
{
    return ap_open(ctx, path, DL_AP_ASC, frames);
}

static esp_err_t asc_write(void *ctx, const dl_record_t *rec,
                           dl_param_entry_t *p)
{
    (void)p;

    dl_append_ctx_t *c = ctx;

    if (rec->kind != DL_REC_FRAME)
    {
        return ESP_OK; /* frames-only format */
    }

    char row[80];

    /* relative to the open by the monotonic clock: a wall step cannot
       make the time run backwards (the header date stays as written) */
    return row_write(c, row,
                     dl_asc_row(row, sizeof(row),
                                dl_clock_rel_ms(rec, c->start_ms,
                                                c->mono_open),
                                rec->u.f.id, rec->u.f.flags,
                                rec->u.f.data, rec->u.f.dlc));
}

const dl_engine_t dl_engine_asc =
{
    .ext = ".asc",
    .single_use = true, /* rows are relative to the header date */
    .open = asc_open,
    .close = ap_close,
    .is_open = ap_is_open,
    .begin = ap_begin,
    .write = asc_write,
    .commit = ap_commit,
    .bytes = ap_bytes,
};

static esp_err_t jsonl_open(void *ctx, const char *path, bool frames)
{
    return ap_open(ctx, path, DL_AP_JSONL, frames);
}

static esp_err_t jsonl_write(void *ctx, const dl_record_t *rec,
                             dl_param_entry_t *p)
{
    char row[128];
    int n;

    if (rec->kind == DL_REC_FRAME)
    {
        n = dl_jsonl_frame_row(row, sizeof(row), rec->ts_ms,
                               rec->u.f.id, rec->u.f.flags,
                               rec->u.f.data, rec->u.f.dlc);
    }
    else
    {
        n = dl_jsonl_param_row(row, sizeof(row), rec->ts_ms, p->source,
                               p->name, rec->u.p.value);
    }

    return row_write(ctx, row, n);
}

/* a line without `param` / `id`: consumers that read by key skip it */
static esp_err_t jsonl_mark(void *ctx, const dl_clock_step_t *step)
{
    char row[128];
    int n = dl_jsonl_step_row(row, sizeof(row), step);

    return (n > 0) ? ap_put_durable(ctx, row, (size_t)n) : ESP_FAIL;
}

const dl_engine_t dl_engine_jsonl =
{
    .ext = ".jsonl",
    .open = jsonl_open,
    .close = ap_close,
    .is_open = ap_is_open,
    .begin = ap_begin,
    .write = jsonl_write,
    .commit = ap_commit,
    .bytes = ap_bytes,
    .mark = jsonl_mark,
};
