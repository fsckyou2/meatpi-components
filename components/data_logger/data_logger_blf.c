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
 * @file data_logger_blf.c
 * @brief Vector BLF storage engine: ubiquitous in Vector shops,
 *        python-can-readable. Byte layout comes from the PURE builders
 *        in data_logger_files.c (host-tested); this file is only the
 *        container buffering + FILE plumbing.
 *
 * Layout: the 144-byte "LOGG" file header (object count / sizes / end
 * time, re-written on every commit so a torn file reads to the last
 * commit) followed by LOG_CONTAINER objects (compressionMethod=0,
 * UNCOMPRESSED v1: python-can reads method 0; a zlib variant via the
 * ESP ROM miniz is the v2 candidate) each holding concatenated
 * 48-byte CAN_MESSAGE objects. Messages accumulate in a PSRAM buffer
 * and flush as one container per commit (or when the buffer fills).
 * single_use: timestamps are ns relative to the header start time.
 *
 * The clock (2026-10-13): messages are relative to the open by the
 * monotonic clock; the header's start and end times are re-computed
 * from the wall clock as it is NOW at every commit, so a clock step
 * while the file is open corrects them.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <unistd.h>

#include "data_logger_private.h"

static const char *TAG = "data_logger";

#define BLF_BUF_SZ  4096
#define BLF_CONT_SZ 32768 /* container payload cap (PSRAM) */

/* one engine instance can exist (CAN stream only): the container
 * accumulator can be a static PSRAM buffer */
static uint8_t s_cont[BLF_CONT_SZ] EXT_RAM_BSS_ATTR;

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

static esp_err_t blf_flush_container(dl_blf_ctx_t *c)
{
    if (c->cfill == 0)
    {
        return ESP_OK;
    }

    uint8_t hdr[DL_BLF_CONT_HDR];

    dl_blf_container_header(hdr, c->cfill);

    FILE *f = c->f;

    if (fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        fwrite(s_cont, 1, c->cfill, f) != c->cfill)
    {
        return ESP_FAIL;
    }

    c->bytes += sizeof(hdr) + c->cfill;
    c->uncomp += c->cfill;
    c->cfill = 0;
    return ESP_OK;
}

static esp_err_t blf_patch_header(dl_blf_ctx_t *c)
{
    uint8_t hdr[DL_BLF_HDR_SIZE];
    FILE *f = c->f;
    int64_t start = dl_clock_start_ms(wall_ms_now(), mono_ms_now(),
                                      c->mono_open);

    /* the start as the wall clock says now (the ms rounding of two
       clocks aside): a step while the file was open corrects it */
    if (start - c->start_ms >= 2 || c->start_ms - start >= 2)
    {
        c->start_ms = start;
    }

    dl_blf_file_header(hdr, c->start_ms, c->start_ms + c->last_rel_ms,
                       c->bytes, DL_BLF_HDR_SIZE + c->uncomp, c->objects);

    if (fseek(f, 0, SEEK_SET) != 0 ||
        fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        fseek(f, 0, SEEK_END) != 0)
    {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t blf_open(void *ctx, const char *path, bool frames)
{
    dl_blf_ctx_t *c = ctx;

    (void)frames; /* CAN-stream only (schema-enforced) */

    if (c->f != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    c->buf = heap_caps_malloc(BLF_BUF_SZ,
                              MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

    if (c->buf == NULL)
    {
        ESP_LOGE(TAG, "no internal RAM for the stdio buffer");
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(path, "w"); /* single_use: always a fresh file */

    if (f == NULL)
    {
        ESP_LOGE(TAG, "open %s failed (errno %d)", path, errno);
        heap_caps_free(c->buf);
        c->buf = NULL;
        return ESP_FAIL;
    }

    setvbuf(f, c->buf, _IOFBF, BLF_BUF_SZ);

    c->mono_open = mono_ms_now();
    c->start_ms = wall_ms_now();
    c->last_rel_ms = 0;
    c->uncomp = 0;
    c->objects = 0;
    c->cfill = 0;

    uint8_t hdr[DL_BLF_HDR_SIZE];

    dl_blf_file_header(hdr, c->start_ms, c->start_ms, DL_BLF_HDR_SIZE,
                       DL_BLF_HDR_SIZE, 0);

    if (fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr))
    {
        fclose(f);
        heap_caps_free(c->buf);
        c->buf = NULL;
        return ESP_FAIL;
    }

    c->f = f;
    c->bytes = DL_BLF_HDR_SIZE;
    return ESP_OK;
}

static void blf_close(void *ctx)
{
    dl_blf_ctx_t *c = ctx;

    if (c->f != NULL)
    {
        (void)blf_flush_container(c);
        (void)blf_patch_header(c);
        fclose((FILE *)c->f);
        c->f = NULL;
    }

    if (c->buf != NULL)
    {
        heap_caps_free(c->buf);
        c->buf = NULL;
    }
}

static bool blf_is_open(void *ctx)
{
    return ((dl_blf_ctx_t *)ctx)->f != NULL;
}

static esp_err_t blf_begin(void *ctx)
{
    return (((dl_blf_ctx_t *)ctx)->f != NULL) ? ESP_OK
                                              : ESP_ERR_INVALID_STATE;
}

static esp_err_t blf_write(void *ctx, const dl_record_t *rec,
                           dl_param_entry_t *p)
{
    (void)p;

    dl_blf_ctx_t *c = ctx;

    if (rec->kind != DL_REC_FRAME)
    {
        return ESP_OK; /* frames-only format */
    }

    if (c->cfill + DL_BLF_MSG_SIZE > BLF_CONT_SZ &&
        blf_flush_container(c) != ESP_OK)
    {
        return ESP_FAIL;
    }

    int64_t rel = dl_clock_rel_ms(rec, c->start_ms, c->mono_open);

    c->cfill += (uint32_t)dl_blf_message(s_cont + c->cfill, rel, rec);
    c->objects++;

    if (rel > c->last_rel_ms)
    {
        c->last_rel_ms = rel;
    }

    return ESP_OK;
}

static esp_err_t blf_commit(void *ctx)
{
    dl_blf_ctx_t *c = ctx;
    FILE *f = c->f;

    if (f == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (blf_flush_container(c) != ESP_OK ||
        blf_patch_header(c) != ESP_OK)
    {
        return ESP_FAIL;
    }

    if (fflush(f) != 0 || fsync(fileno(f)) != 0)
    {
        ESP_LOGE(TAG, "blf flush failed (errno %d)", errno);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static uint64_t blf_bytes(void *ctx)
{
    return ((dl_blf_ctx_t *)ctx)->bytes;
}

const dl_engine_t dl_engine_blf =
{
    .ext = ".blf",
    .single_use = true,
    .open = blf_open,
    .close = blf_close,
    .is_open = blf_is_open,
    .begin = blf_begin,
    .write = blf_write,
    .commit = blf_commit,
    .bytes = blf_bytes,
};
