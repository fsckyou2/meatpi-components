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
 * @file config_backup_files.c
 * @brief The growable PSRAM buffer and the file helpers the document
 *        builder and the restore share: a whole file into PSRAM, a
 *        directory read into a list (the callback runs under the fs lock
 *        and only collects), a JSON string literal, a JSON file appended
 *        verbatim after a validity scan, a file as base64 in blocks, a
 *        directory's files as a list of base64 entries, a directory wipe.
 *
 * Every function here reads the filesystem: callers run on an internal
 * stack (the httpd task, the copies worker, the console).
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "filesystem.h"

#include "config_backup_private.h"

static const char *TAG = "config_backup";

#define B64_BLOCK 3072 /* a multiple of 3: no padding until the last block */

/* ---- the buffer ---------------------------------------------------------------------- */

esp_err_t cb_buf_init(cb_buf_t *b, size_t cap0)
{
    memset(b, 0, sizeof(*b));
    b->data = heap_caps_malloc(cap0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (b->data == NULL)
    {
        b->failed = true;
        return ESP_ERR_NO_MEM;
    }

    b->cap = cap0;
    return ESP_OK;
}

esp_err_t cb_buf_append(cb_buf_t *b, const void *data, size_t len)
{
    if (b->failed)
    {
        return ESP_ERR_NO_MEM;
    }

    if (b->len + len + 1 > b->cap)
    {
        size_t cap = b->cap ? b->cap : 4096;

        while (cap < b->len + len + 1)
        {
            cap *= 2;
        }

        if (cap > CB_DOC_MAX + 65536)
        {
            b->failed = true;
            return ESP_ERR_INVALID_SIZE;
        }

        char *n = heap_caps_realloc(b->data, cap,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        if (n == NULL)
        {
            b->failed = true;
            return ESP_ERR_NO_MEM;
        }

        b->data = n;
        b->cap = cap;
    }

    memcpy(b->data + b->len, data, len);
    b->len += len;
    b->data[b->len] = '\0';
    return ESP_OK;
}

esp_err_t cb_buf_puts(cb_buf_t *b, const char *s)
{
    return cb_buf_append(b, s, strlen(s));
}

esp_err_t cb_buf_printf(cb_buf_t *b, const char *fmt, ...)
{
    char    tmp[192];
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t)n >= sizeof(tmp))
    {
        b->failed = true;
        return ESP_ERR_INVALID_SIZE;
    }

    return cb_buf_append(b, tmp, (size_t)n);
}

void cb_buf_free(cb_buf_t *b)
{
    free(b->data);
    memset(b, 0, sizeof(*b));
}

/* a JSON string literal with the escapes the names and PEM need */
esp_err_t cb_json_string_append(cb_buf_t *b, const char *s, size_t n)
{
    esp_err_t err = cb_buf_append(b, "\"", 1);

    for (size_t i = 0; i < n && err == ESP_OK; i++)
    {
        unsigned char c = (unsigned char)s[i];

        if (c == '"' || c == '\\')
        {
            char e[2] = { '\\', (char)c };

            err = cb_buf_append(b, e, 2);
        }
        else if (c == '\n')
        {
            err = cb_buf_append(b, "\\n", 2);
        }
        else if (c == '\r')
        {
            err = cb_buf_append(b, "\\r", 2);
        }
        else if (c == '\t')
        {
            err = cb_buf_append(b, "\\t", 2);
        }
        else if (c < 0x20)
        {
            err = cb_buf_printf(b, "\\u%04x", c);
        }
        else
        {
            err = cb_buf_append(b, s + i, 1);
        }
    }

    if (err == ESP_OK)
    {
        err = cb_buf_append(b, "\"", 1);
    }

    return err;
}

/* ---- files ------------------------------------------------------------------------------ */

esp_err_t cb_read_file(const char *path, char **out, size_t *out_len,
                       size_t cap)
{
    size_t    size = 0;
    esp_err_t err = filesystem_size(path, &size);

    if (err != ESP_OK)
    {
        return err;
    }

    if (size > cap)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    char *buf = heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (buf == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    size_t got = 0;

    err = filesystem_read(path, buf, size + 1, &got);

    if (err != ESP_OK)
    {
        free(buf);
        return err;
    }

    buf[got] = '\0';
    *out = buf;
    *out_len = got;
    return ESP_OK;
}

/* runs under the fs lock: collect only, no fs call. What the list cannot
   take is recorded, not dropped (issue #989: a name with a space or the
   33rd file vanished from every backup with nothing said). */
static esp_err_t list_cb(const char *name, bool is_dir, size_t size, void *ctx)
{
    cb_dir_t *d = ctx;
    size_t    len = strlen(name);

    if (!cb_name_ok(name, len))
    {
        if (!is_dir)
        {
            cb_left_out_add(&d->left, name, len, CB_WHY_NAME);
        }

        return ESP_OK;
    }

    if (d->n >= CB_LIST_MAX)
    {
        d->overflow = true;

        if (!is_dir)
        {
            cb_left_out_add(&d->left, name, len, CB_WHY_COUNT);
        }

        return ESP_OK;
    }

    snprintf(d->names[d->n], CB_NAME_MAX, "%s", name);
    d->sizes[d->n] = size;
    d->dirs[d->n] = is_dir;
    d->n++;
    return ESP_OK;
}

cb_dir_t *cb_dir_read(const char *path)
{
    cb_dir_t *d = heap_caps_calloc(1, sizeof(*d),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (d == NULL)
    {
        return NULL;
    }

    esp_err_t err = filesystem_list(path, list_cb, d);

    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND)
    {
        ESP_LOGD(TAG, "list %s: %s", path, esp_err_to_name(err));
    }

    if (d->overflow)
    {
        /* the sweep reads every folder every two minutes: the status route
           and the document carry the fact, the log only at DEBUG */
        ESP_LOGD(TAG, "%s: more than %d entries, the rest is not backed up",
                 path, CB_LIST_MAX);
    }

    return d;
}

esp_err_t cb_wipe_dir_files(const char *dir)
{
    cb_dir_t *d = cb_dir_read(dir);

    if (d == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;

    if (d->overflow)
    {
        /* a restore (a user action): what the list could not take stays */
        ESP_LOGW(TAG, "%s: more than %d entries, the rest is not cleared",
                 dir, CB_LIST_MAX);
    }

    for (size_t i = 0; i < d->n && err == ESP_OK; i++)
    {
        if (d->dirs[i])
        {
            continue;
        }

        char path[128];

        snprintf(path, sizeof(path), "%s/%s", dir, d->names[i]);
        err = filesystem_delete(path);
    }

    free(d);
    return err;
}

esp_err_t cb_append_json_file(cb_buf_t *b, const char *path)
{
    char     *buf = NULL;
    size_t    len = 0;
    esp_err_t err = cb_read_file(path, &buf, &len, CB_FILE_MAX);

    if (err != ESP_OK)
    {
        return (err == ESP_ERR_INVALID_SIZE) ? err : ESP_ERR_NOT_FOUND;
    }

    bool ok = cb_json_valid(buf, len);

    if (ok)
    {
        /* trim the whitespace around it so the value is one token */
        size_t s = 0;
        size_t e = len;

        while (s < e && (buf[s] == ' ' || buf[s] == '\n' || buf[s] == '\r' ||
                         buf[s] == '\t'))
        {
            s++;
        }

        while (e > s && (buf[e - 1] == ' ' || buf[e - 1] == '\n' ||
                         buf[e - 1] == '\r' || buf[e - 1] == '\t'))
        {
            e--;
        }

        err = cb_buf_append(b, buf + s, e - s);
    }
    else
    {
        /* the caller lists it (issue #989); every sweep would log this */
        ESP_LOGD(TAG, "%s is not JSON: left out", path);
        err = ESP_ERR_INVALID_RESPONSE;
    }

    free(buf);
    return err;
}

esp_err_t cb_append_file_b64(cb_buf_t *b, const char *dir, const char *name,
                             size_t size)
{
    char path[128];

    snprintf(path, sizeof(path), "%s/%s", dir, name);

    if (size > CB_FILE_MAX)
    {
        ESP_LOGD(TAG, "%s: %u bytes is over the limit, left out", path,
                 (unsigned)size);
        return ESP_ERR_INVALID_SIZE;
    }

    /* the whole file into PSRAM through filesystem_read (the copies worker
       runs on a PSRAM stack: the filesystem marshals that read onto its
       internal-stack task, which a stdio stream from filesystem_open()
       cannot be: the first build of 2026-10-13 panicked right here in
       lfs_file_opencfg, caught by the config backup bench) */
    char  *raw = NULL;
    size_t len = 0;
    esp_err_t err = cb_read_file(path, &raw, &len, CB_FILE_MAX);

    if (err != ESP_OK)
    {
        return (err == ESP_ERR_INVALID_SIZE) ? err : ESP_ERR_NOT_FOUND;
    }

    err = cb_buf_puts(b, "{\"name\":");

    if (err == ESP_OK)
    {
        err = cb_json_string_append(b, name, strlen(name));
    }

    if (err == ESP_OK)
    {
        err = cb_buf_printf(b, ",\"size\":%u,\"b64\":\"", (unsigned)len);
    }

    char *enc = heap_caps_malloc(cb_b64_size(B64_BLOCK),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (enc == NULL)
    {
        err = ESP_ERR_NO_MEM;
    }

    for (size_t off = 0; err == ESP_OK && off < len; off += B64_BLOCK)
    {
        size_t n = (len - off > B64_BLOCK) ? B64_BLOCK : len - off;
        size_t olen = 0;

        err = cb_b64_encode((const uint8_t *)raw + off, n, enc,
                            cb_b64_size(B64_BLOCK), &olen);

        if (err == ESP_OK)
        {
            err = cb_buf_append(b, enc, olen);
        }
    }

    free(enc);
    free(raw);

    if (err == ESP_OK)
    {
        err = cb_buf_puts(b, "\"}");
    }

    return err;
}

esp_err_t cb_append_dir_b64(cb_buf_t *b, const char *dir, cb_left_out_t *left)
{
    cb_dir_t *d = cb_dir_read(dir);

    if (d == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = cb_buf_puts(b, "[");
    bool      first = true;

    for (size_t i = 0; i < d->n && err == ESP_OK; i++)
    {
        size_t nlen = strlen(d->names[i]);

        if (d->dirs[i])
        {
            continue;
        }

        if (d->sizes[i] > CB_FILE_MAX)
        {
            /* listed, not fatal (issue #989: one big file used to fail the
               whole download and stall that part of the copies) */
            cb_left_out_add(left, d->names[i], nlen, CB_WHY_SIZE);
            continue;
        }

        size_t mark = b->len;

        if (!first)
        {
            err = cb_buf_puts(b, ",");
        }

        if (err == ESP_OK)
        {
            err = cb_append_file_b64(b, dir, d->names[i], d->sizes[i]);
        }

        if (err != ESP_OK && err != ESP_ERR_NO_MEM && !b->failed)
        {
            /* gone or grown between the listing and the read: the entry
               rolled back, the file listed, the document goes on */
            b->len = mark;
            b->data[mark] = '\0';
            cb_left_out_add(left, d->names[i], nlen,
                            err == ESP_ERR_INVALID_SIZE ? CB_WHY_SIZE
                                                        : CB_WHY_READ);
            err = ESP_OK;
            continue;
        }

        first = false;
    }

    cb_left_out_merge(left, &d->left);
    free(d);

    if (err == ESP_OK)
    {
        err = cb_buf_puts(b, "]");
    }

    return err;
}
