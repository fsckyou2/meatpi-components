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
 * @file config_backup_core.c
 * @brief PURE (no IDF): the part names, the top-level JSON member scanner
 *        (ranges, never a parse of the big strings), the name checks, the
 *        ISO time. The redaction rule, the key-file codec and the manifest
 *        codec are in config_backup_codec.c. Host-tested in host_test/.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "config_backup_private.h"

const char *const CB_PART_NAMES[CB_PART_COUNT] =
{
    "settings", "vehicles", "databases", "scripts", "dashboard", "certs",
};

int cb_part_from_name(const char *name, size_t len)
{
    if (name == NULL)
    {
        return -1;
    }

    for (int p = 0; p < CB_PART_COUNT; p++)
    {
        if (strlen(CB_PART_NAMES[p]) == len &&
            memcmp(CB_PART_NAMES[p], name, len) == 0)
        {
            return p;
        }
    }

    return -1;
}

/* ---- the scanner ------------------------------------------------------------------- */

static size_t skip_ws(const char *b, size_t n, size_t i)
{
    while (i < n &&
           (b[i] == ' ' || b[i] == '\t' || b[i] == '\r' || b[i] == '\n'))
    {
        i++;
    }

    return i;
}

/* *i at the opening quote; on success *i is past the closing quote */
static bool scan_string(const char *b, size_t n, size_t *i)
{
    size_t p = *i + 1;

    while (p < n)
    {
        if (b[p] == '\\')
        {
            p += 2;
            continue;
        }

        if (b[p] == '"')
        {
            *i = p + 1;
            return true;
        }

        p++;
    }

    return false;
}

/* *i at the first char of a value; on success *i is past its last char */
static bool scan_value(const char *b, size_t n, size_t *i)
{
    size_t p = *i;

    if (p >= n)
    {
        return false;
    }

    if (b[p] == '"')
    {
        return scan_string(b, n, i);
    }

    if (b[p] == '{' || b[p] == '[')
    {
        int depth = 0;

        while (p < n)
        {
            char c = b[p];

            if (c == '"')
            {
                if (!scan_string(b, n, &p))
                {
                    return false;
                }

                continue;
            }

            if (c == '{' || c == '[')
            {
                depth++;
            }
            else if (c == '}' || c == ']')
            {
                depth--;

                if (depth == 0)
                {
                    *i = p + 1;
                    return true;
                }
            }

            p++;
        }

        return false;
    }

    while (p < n && b[p] != ',' && b[p] != '}' && b[p] != ']' &&
           b[p] != ' ' && b[p] != '\n' && b[p] != '\r' && b[p] != '\t')
    {
        p++;
    }

    if (p == *i)
    {
        return false;
    }

    *i = p;
    return true;
}

esp_err_t cb_json_members(const char *b, size_t n, cb_member_cb_t cb,
                          void *ctx)
{
    if (b == NULL || cb == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    size_t i = skip_ws(b, n, 0);

    if (i >= n || b[i] != '{')
    {
        return ESP_ERR_INVALID_ARG;
    }

    i = skip_ws(b, n, i + 1);

    if (i < n && b[i] == '}')
    {
        return ESP_OK;
    }

    for (;;)
    {
        if (i >= n || b[i] != '"')
        {
            return ESP_ERR_INVALID_ARG;
        }

        size_t ks = i + 1;
        size_t ke = i;

        if (!scan_string(b, n, &ke))
        {
            return ESP_ERR_INVALID_ARG;
        }

        cb_member_t m = { .key = b + ks, .key_len = ke - 1 - ks };

        i = skip_ws(b, n, ke);

        if (i >= n || b[i] != ':')
        {
            return ESP_ERR_INVALID_ARG;
        }

        i = skip_ws(b, n, i + 1);

        size_t vs = i;
        size_t ve = i;

        if (!scan_value(b, n, &ve))
        {
            return ESP_ERR_INVALID_ARG;
        }

        m.val = b + vs;
        m.val_len = ve - vs;

        esp_err_t r = cb(&m, ctx);

        if (r == CB_STOP)
        {
            return ESP_OK;
        }

        if (r != ESP_OK)
        {
            return r;
        }

        i = skip_ws(b, n, ve);

        if (i >= n)
        {
            return ESP_ERR_INVALID_ARG;
        }

        if (b[i] == ',')
        {
            i = skip_ws(b, n, i + 1);
            continue;
        }

        return (b[i] == '}') ? ESP_OK : ESP_ERR_INVALID_ARG;
    }
}

typedef struct
{
    const char  *key;
    size_t       key_len;
    cb_member_t *out;
    bool         found;
} find_ctx_t;

static esp_err_t find_cb(const cb_member_t *m, void *ctx)
{
    find_ctx_t *f = ctx;

    if (m->key_len == f->key_len && memcmp(m->key, f->key, f->key_len) == 0)
    {
        *f->out = *m;
        f->found = true;
        return CB_STOP;
    }

    return ESP_OK;
}

bool cb_json_find(const char *buf, size_t len, const char *key,
                  cb_member_t *out)
{
    if (key == NULL || out == NULL)
    {
        return false;
    }

    find_ctx_t f = { .key = key, .key_len = strlen(key), .out = out };

    (void)cb_json_members(buf, len, find_cb, &f);
    return f.found;
}

bool cb_json_valid(const char *buf, size_t len)
{
    if (buf == NULL)
    {
        return false;
    }

    size_t i = skip_ws(buf, len, 0);

    if (!scan_value(buf, len, &i))
    {
        return false;
    }

    return skip_ws(buf, len, i) == len;
}

bool cb_json_string(const cb_member_t *m, char *out, size_t out_len)
{
    if (m == NULL || out == NULL || out_len == 0 || m->val_len < 2 ||
        m->val[0] != '"')
    {
        return false;
    }

    size_t o = 0;

    for (size_t i = 1; i + 1 < m->val_len; i++)
    {
        char c = m->val[i];

        if (c == '\\' && i + 2 < m->val_len)
        {
            char e = m->val[++i];

            switch (e)
            {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'u': c = '?'; i += 4; break;
                default:  c = e;    break; /* \" \\ \/ */
            }
        }

        if (o + 1 >= out_len)
        {
            return false;
        }

        out[o++] = c;
    }

    out[o] = '\0';
    return true;
}

bool cb_json_bool(const cb_member_t *m)
{
    return m != NULL && m->val_len == 4 && memcmp(m->val, "true", 4) == 0;
}

bool cb_json_is_null(const cb_member_t *m)
{
    return m == NULL || (m->val_len == 4 && memcmp(m->val, "null", 4) == 0);
}

long cb_json_long(const cb_member_t *m)
{
    if (m == NULL || m->val_len == 0 || m->val_len > 20)
    {
        return 0;
    }

    char tmp[24];

    memcpy(tmp, m->val, m->val_len);
    tmp[m->val_len] = '\0';
    return strtol(tmp, NULL, 10);
}

/* ---- names ------------------------------------------------------------------------- */

bool cb_name_ok(const char *name, size_t len)
{
    /* the filesystem's own segment rule (filesystem_path.c). Until issue
       #989 (2026-10-16) this refused a space, a leading dot and the double
       quote, so a legitimate "my car.dbc" vanished from every backup with
       nothing said; the JSON escaper carries any printable character. */
    if (name == NULL || len == 0 || len >= CB_NAME_MAX)
    {
        return false;
    }

    if ((len == 1 && name[0] == '.') ||
        (len == 2 && name[0] == '.' && name[1] == '.'))
    {
        return false;
    }

    for (size_t i = 0; i < len; i++)
    {
        char c = name[i];

        if (c < 0x20 || c > 0x7e || c == '/' || c == '\\')
        {
            return false;
        }
    }

    return true;
}

/* ---- what a part leaves out ------------------------------------------------------- */

void cb_left_out_add(cb_left_out_t *lo, const char *name, size_t len,
                     cb_why_t why)
{
    if (lo == NULL)
    {
        return;
    }

    if (lo->n >= CB_LEFT_MAX)
    {
        lo->more++;
        return;
    }

    cb_left_item_t *it = &lo->items[lo->n++];

    if (name == NULL)
    {
        len = 0;
    }

    if (len >= sizeof(it->name))
    {
        len = sizeof(it->name) - 1;
    }

    /* a name the rule refused may hold anything: the printable part only */
    size_t o = 0;

    for (size_t i = 0; i < len; i++)
    {
        unsigned char c = (unsigned char)name[i];

        it->name[o++] = (c < 0x20 || c > 0x7e) ? '?' : (char)c;
    }

    it->name[o] = '\0';
    it->why = (uint8_t)why;
}

void cb_left_out_merge(cb_left_out_t *dst, const cb_left_out_t *src)
{
    if (dst == NULL || src == NULL)
    {
        return;
    }

    for (size_t i = 0; i < src->n; i++)
    {
        cb_left_out_add(dst, src->items[i].name, strlen(src->items[i].name),
                        (cb_why_t)src->items[i].why);
    }

    dst->more += src->more;
}

/* the words carry the limits: a change of either macro lands here */
_Static_assert(CB_FILE_MAX == 512u * 1024u, "cb_why_str names the size limit");
_Static_assert(CB_LIST_MAX == 128, "cb_why_str names the folder cap");

const char *cb_why_str(cb_why_t why)
{
    switch (why)
    {
        case CB_WHY_NAME:  return "a name the backup cannot carry";
        case CB_WHY_SIZE:  return "over 512 KB";
        case CB_WHY_COUNT: return "more than 128 files in its folder";
        case CB_WHY_READ:  return "could not be read";
        case CB_WHY_JSON:  return "not valid JSON";
        default:           return "left out";
    }
}

bool cb_set_name_ok(const char *name, size_t len)
{
    if (name == NULL || len == 0 || len > 24)
    {
        return false;
    }

    for (size_t i = 0; i < len; i++)
    {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-';

        if (!ok)
        {
            return false;
        }
    }

    return true;
}

bool cb_vehicle_key_ok(const char *name, size_t len)
{
    if (name == NULL || len == 0 || len > 40)
    {
        return false;
    }

    for (size_t i = 0; i < len; i++)
    {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == ':' ||
                  c == '.' || c == '-';

        if (!ok)
        {
            return false;
        }
    }

    return true;
}

/* ---- time ------------------------------------------------------------------------------ */

void cb_iso_from_epoch(int64_t t, char out[CB_ISO_LEN])
{
    out[0] = '\0';

    if (t <= 0)
    {
        return;
    }

    time_t    tt = (time_t)t;
    struct tm tm;

    if (gmtime_r(&tt, &tm) != NULL)
    {
        strftime(out, CB_ISO_LEN, "%Y-%m-%dT%H:%M:%SZ", &tm);
    }
}
