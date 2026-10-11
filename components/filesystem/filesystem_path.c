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
 * @file filesystem_path.c
 * @brief Pure path validation/routing for the filesystem component, and
 *        the LittleFS superblock probe the mount runs first (2026-10-06).
 *        No IDF/VFS dependencies: compiled as-is by the host unit tests.
 */
#include "filesystem_private.h"

#include <string.h>

typedef struct
{
    const char  *prefix;
    size_t       len;
    fs_backend_t backend;
} prefix_map_t;

static const prefix_map_t PREFIXES[] =
{
    { FS_PREFIX_INTERNAL, sizeof(FS_PREFIX_INTERNAL) - 1, FS_BACKEND_INTERNAL },
    { FS_PREFIX_SD,       sizeof(FS_PREFIX_SD) - 1,       FS_BACKEND_SD       },
};

static bool char_ok(char c)
{
    /* printable ASCII, minus backslash (path-separator confusion) */
    return (c >= 0x20) && (c <= 0x7e) && (c != '\\');
}

static bool segment_ok(const char *seg, size_t len)
{
    if (len == 0)
    {
        return false; /* "//" */
    }

    if ((len == 1 && seg[0] == '.') ||
        (len == 2 && seg[0] == '.' && seg[1] == '.'))
    {
        return false; /* "." / ".." */
    }

    for (size_t i = 0; i < len; i++)
    {
        if (!char_ok(seg[i]))
        {
            return false;
        }
    }

    return true;
}

esp_err_t fs_path_resolve(const char *path, fs_backend_t *out_backend)
{
    if (path == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strnlen(path, FS_PATH_MAX);

    if (len == 0 || len >= FS_PATH_MAX || path[0] != '/')
    {
        return ESP_ERR_INVALID_ARG;
    }

    const prefix_map_t *hit = NULL;

    for (size_t i = 0; i < sizeof(PREFIXES) / sizeof(PREFIXES[0]); i++)
    {
        const prefix_map_t *p = &PREFIXES[i];

        if (strncmp(path, p->prefix, p->len) == 0 &&
            (path[p->len] == '\0' || path[p->len] == '/'))
        {
            hit = p;
            break;
        }
    }

    if (hit == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* validate the remainder segment by segment */
    const char *s = path + hit->len;

    while (*s == '/')
    {
        const char *seg = s + 1;
        const char *end = strchr(seg, '/');
        size_t      seg_len = (end != NULL) ? (size_t)(end - seg)
                                            : strlen(seg);

        if (!segment_ok(seg, seg_len))
        {
            return ESP_ERR_INVALID_ARG;
        }

        s = seg + seg_len;
    }

    if (*s != '\0')
    {
        return ESP_ERR_INVALID_ARG; /* junk directly after the prefix */
    }

    if (path[len - 1] == '/')
    {
        return ESP_ERR_INVALID_ARG; /* trailing slash (covers "/data/") */
    }

    if (out_backend != NULL)
    {
        *out_backend = hit->backend;
    }

    return ESP_OK;
}

esp_err_t fs_path_temp_name(const char *path, char *out, size_t out_len)
{
    if (path == NULL || out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    size_t need = strlen(path) + sizeof(FS_TEMP_SUFFIX); /* incl. NUL */

    if (need > out_len || need > FS_PATH_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }

    strcpy(out, path);
    strcat(out, FS_TEMP_SUFFIX);
    return ESP_OK;
}

esp_err_t fs_path_parent(const char *path, char *out, size_t out_len)
{
    if (path == NULL || out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const char *last = strrchr(path, '/');

    if (last == NULL || last == path)
    {
        return ESP_ERR_INVALID_ARG;
    }

    size_t plen = (size_t)(last - path);

    if (plen + 1 > out_len)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(out, path, plen);
    out[plen] = '\0';
    return ESP_OK;
}

/* the folders the HTTP file surface never reaches (key material lives there) */
static const struct
{
    const char *prefix;
    bool        fold_case; /* FAT on the card is case-insensitive */
} HIDDEN[] =
{
    { "/data/certs",   false },
    { "/data/backups", false },
    { "/sd/backups",   true  },
};

static int prefix_cmp(const char *a, const char *b, size_t n, bool fold)
{
    for (size_t i = 0; i < n; i++)
    {
        int ca = (unsigned char)a[i];
        int cb = (unsigned char)b[i];

        if (fold)
        {
            ca = (ca >= 'A' && ca <= 'Z') ? ca + 32 : ca;
            cb = (cb >= 'A' && cb <= 'Z') ? cb + 32 : cb;
        }

        if (ca != cb || ca == 0)
        {
            return ca - cb;
        }
    }

    return 0;
}

bool filesystem_api_hidden(const char *path)
{
    if (path == NULL)
    {
        return false;
    }

    for (size_t i = 0; i < sizeof(HIDDEN) / sizeof(HIDDEN[0]); i++)
    {
        size_t len = strlen(HIDDEN[i].prefix);
        int    cmp = prefix_cmp(path, HIDDEN[i].prefix, len, HIDDEN[i].fold_case);

        if (cmp == 0 && (path[len] == '\0' || path[len] == '/'))
        {
            return true;
        }
    }

    return false;
}

bool fs_region_is_blank(const uint8_t *buf, size_t len)
{
    if (buf == NULL || len == 0)
    {
        return false;
    }

    for (size_t i = 0; i < len; i++)
    {
        if (buf[i] != 0xFF)
        {
            return false;
        }
    }

    return true;
}

/* ---- the superblock probe (2026-10-06) ---------------------------------- */

#define FS_LFS_MAGIC      "littlefs"
#define FS_LFS_MAGIC_LEN  8u
#define FS_LFS_TAG_LEN    4u
#define FS_LFS_STRUCT_LEN 24u   /* six little-endian u32s */
#define FS_LFS_MAX_BLOCKS (1u << 24)

static uint32_t fs_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* the first superblock entry of a block whose struct makes sense */
static bool fs_block_superblock(const uint8_t *blk, size_t len,
                                uint32_t block_size, uint32_t *block_count)
{
    size_t need = FS_LFS_MAGIC_LEN + FS_LFS_TAG_LEN + FS_LFS_STRUCT_LEN;

    for (size_t m = 0; m + need <= len; m++)
    {
        if (memcmp(blk + m, FS_LFS_MAGIC, FS_LFS_MAGIC_LEN) != 0)
        {
            continue;
        }

        const uint8_t *st = blk + m + FS_LFS_MAGIC_LEN + FS_LFS_TAG_LEN;
        uint32_t version = fs_le32(st);
        uint32_t bsize = fs_le32(st + 4);
        uint32_t bcount = fs_le32(st + 8);

        if ((version >> 16) == 2u && bsize == block_size && bcount >= 2u &&
            bcount <= FS_LFS_MAX_BLOCKS)
        {
            *block_count = bcount;
            return true;
        }
    }

    return false;
}

fs_lfs_kind_t fs_lfs_probe(const uint8_t *blk0, const uint8_t *blk1,
                           size_t len, uint32_t block_size,
                           uint32_t *block_count)
{
    if (blk0 == NULL || blk1 == NULL || block_count == NULL ||
        len < FS_LFS_MAGIC_LEN + FS_LFS_TAG_LEN + FS_LFS_STRUCT_LEN)
    {
        return FS_LFS_OTHER;
    }

    if (fs_region_is_blank(blk0, len) && fs_region_is_blank(blk1, len))
    {
        return FS_LFS_BLANK;
    }

    if (fs_block_superblock(blk0, len, block_size, block_count) ||
        fs_block_superblock(blk1, len, block_size, block_count))
    {
        return FS_LFS_LITTLEFS;
    }

    return FS_LFS_OTHER;
}
