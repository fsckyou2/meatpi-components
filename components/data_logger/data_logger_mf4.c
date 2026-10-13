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
 * @file data_logger_mf4.c
 * @brief MDF 4.10 (ASAM) storage engine: the de-facto bus-logging
 *        standard (asammdf / Vector CANoe / MATLAB / INCA; what a
 *        CANedge emits). Byte layout comes from the PURE builders in
 *        data_logger_files.c (host-tested); this file is only the
 *        FILE plumbing.
 *
 * Streaming model (MCU-writable): the metadata prelude (ID..CN blocks
 * + the DT block header) is written once at open, then fixed 22-byte
 * CAN_DataFrame records append into the DT block. Every commit()
 * seeks back and patches the DT block length + CG cycle_count, so a
 * power cut loses at most the last batch: no UNFINALIZED-recovery
 * dance. single_use: the master time channel is relative to the file
 * start, so files never resume across opens.
 *
 * The clock (2026-10-13): records are relative to the open by the
 * monotonic clock, and the header's start time is re-patched at every
 * commit from the wall clock as it is NOW, so a file whose clock was
 * set or stepped while it was open carries the corrected start and
 * right relative times.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <unistd.h>

#include "data_logger_private.h"

static const char *TAG = "data_logger";

#define MF4_BUF_SZ 4096

static int64_t wall_ms_now(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static uint32_t mono_ms_now(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static esp_err_t mf4_open(void *ctx, const char *path, bool frames)
{
    dl_mf4_ctx_t *c = ctx;

    (void)frames; /* CAN-stream only (schema-enforced) */

    if (c->f != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    c->buf = heap_caps_malloc(MF4_BUF_SZ,
                              MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

    if (c->buf == NULL)
    {
        ESP_LOGE(TAG, "no internal RAM for the stdio buffer");
        return ESP_ERR_NO_MEM;
    }

    /* prelude built in PSRAM scratch (~1.2 KB) */
    uint8_t *pre = heap_caps_malloc(DL_MF4_PRELUDE_MAX,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (pre == NULL)
    {
        heap_caps_free(c->buf);
        c->buf = NULL;
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(path, "w"); /* single_use: always a fresh file */

    if (f == NULL)
    {
        ESP_LOGE(TAG, "open %s failed (errno %d)", path, errno);
        heap_caps_free(pre);
        heap_caps_free(c->buf);
        c->buf = NULL;
        return ESP_FAIL;
    }

    setvbuf(f, c->buf, _IOFBF, MF4_BUF_SZ);

    c->mono_open = mono_ms_now();
    c->start_ms = wall_ms_now();

    size_t n = dl_mf4_prelude(pre, DL_MF4_PRELUDE_MAX, c->start_ms,
                              &c->dt_len_off, &c->cg_cycle_off);
    bool ok = (n > 0) && (fwrite(pre, 1, n, f) == n);

    heap_caps_free(pre);

    if (!ok)
    {
        fclose(f);
        heap_caps_free(c->buf);
        c->buf = NULL;
        return ESP_FAIL;
    }

    c->f = f;
    c->bytes = n;
    c->records = 0;
    return ESP_OK;
}

static esp_err_t mf4_patch_start(dl_mf4_ctx_t *c, FILE *f);

static void mf4_close(void *ctx)
{
    dl_mf4_ctx_t *c = ctx;

    if (c->f != NULL)
    {
        /* a clock step rotates the file without a batch in between: the
           header's start follows the corrected clock at the close too */
        (void)mf4_patch_start(c, (FILE *)c->f);
        fclose((FILE *)c->f);
        c->f = NULL;
    }

    if (c->buf != NULL)
    {
        heap_caps_free(c->buf);
        c->buf = NULL;
    }
}

static bool mf4_is_open(void *ctx)
{
    return ((dl_mf4_ctx_t *)ctx)->f != NULL;
}

static esp_err_t mf4_begin(void *ctx)
{
    return (((dl_mf4_ctx_t *)ctx)->f != NULL) ? ESP_OK
                                              : ESP_ERR_INVALID_STATE;
}

static esp_err_t mf4_write(void *ctx, const dl_record_t *rec,
                           dl_param_entry_t *p)
{
    (void)p;

    dl_mf4_ctx_t *c = ctx;

    if (rec->kind != DL_REC_FRAME)
    {
        return ESP_OK; /* frames-only format */
    }

    uint8_t r[DL_MF4_REC_SIZE];
    size_t n = dl_mf4_record(r, dl_clock_rel_ms(rec, c->start_ms,
                                                c->mono_open), rec);

    if (fwrite(r, 1, n, (FILE *)c->f) != n)
    {
        return ESP_FAIL;
    }

    c->bytes += n;
    c->records++;
    return ESP_OK;
}

static void put_u64_local(uint8_t *b, uint64_t v)
{
    for (int i = 0; i < 8; i++)
    {
        b[i] = (uint8_t)(v >> (8 * i));
    }
}

/* the start time as the wall clock says now: unchanged while the clock
 * holds (within the ms rounding of two clocks), moved by every step */
static esp_err_t mf4_patch_start(dl_mf4_ctx_t *c, FILE *f)
{
    int64_t start = dl_clock_start_ms(wall_ms_now(), mono_ms_now(),
                                      c->mono_open);

    if (start - c->start_ms < 2 && c->start_ms - start < 2)
    {
        return ESP_OK;
    }

    uint8_t v[8];

    put_u64_local(v, (uint64_t)start * 1000000ULL);

    if (fseek(f, DL_MF4_HD_START_OFF, SEEK_SET) != 0 ||
        fwrite(v, 1, 8, f) != 8 ||
        fseek(f, DL_MF4_FH_START_OFF, SEEK_SET) != 0 ||
        fwrite(v, 1, 8, f) != 8)
    {
        return ESP_FAIL;
    }

    c->start_ms = start;
    return ESP_OK;
}

static esp_err_t mf4_commit(void *ctx)
{
    dl_mf4_ctx_t *c = ctx;
    FILE *f = c->f;

    if (f == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    /* patch the start time, the DT block length + CG cycle count, back
       to the tail */
    uint8_t v[8];

    if (mf4_patch_start(c, f) != ESP_OK)
    {
        return ESP_FAIL;
    }

    put_u64_local(v, 24 + c->records * DL_MF4_REC_SIZE);

    if (fseek(f, (long)c->dt_len_off, SEEK_SET) != 0 ||
        fwrite(v, 1, 8, f) != 8)
    {
        return ESP_FAIL;
    }

    put_u64_local(v, c->records);

    if (fseek(f, (long)c->cg_cycle_off, SEEK_SET) != 0 ||
        fwrite(v, 1, 8, f) != 8 ||
        fseek(f, 0, SEEK_END) != 0)
    {
        return ESP_FAIL;
    }

    if (fflush(f) != 0 || fsync(fileno(f)) != 0)
    {
        ESP_LOGE(TAG, "mf4 flush failed (errno %d)", errno);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static uint64_t mf4_bytes(void *ctx)
{
    return ((dl_mf4_ctx_t *)ctx)->bytes;
}

const dl_engine_t dl_engine_mf4 =
{
    .ext = ".mf4",
    .single_use = true,
    .open = mf4_open,
    .close = mf4_close,
    .is_open = mf4_is_open,
    .begin = mf4_begin,
    .write = mf4_write,
    .commit = mf4_commit,
    .bytes = mf4_bytes,
};
