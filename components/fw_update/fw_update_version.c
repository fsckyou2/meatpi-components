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
 * @file fw_update_version.c
 * @brief The version grammar (strict for the manifest, tolerant for what a
 *        device reports), the order, the channel words and the URL rules.
 *        Pure: host-tested.
 */
#include "fw_update_core.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ---- numbers -------------------------------------------------------------------- */

/** Read decimal digits into @p out (< 65536). @p strict refuses a leading
 *  zero before another digit. Returns the characters consumed, 0 = none. */
static size_t read_num(const char *s, bool strict, uint16_t *out)
{
    size_t n = 0;
    uint32_t v = 0;

    while (isdigit((unsigned char)s[n]))
    {
        v = v * 10U + (uint32_t)(s[n] - '0');

        if (v > 65535U)
        {
            return 0;
        }

        n++;
    }

    if (n == 0 || (strict && n > 1 && s[0] == '0'))
    {
        return 0;
    }

    *out = (uint16_t)v;
    return n;
}

static bool is_sep(char c)
{
    return c == '-' || c == '_' || c == '.';
}

/** "alfa" / "alpha" / "beta" at @p s: the pre kind and the word's length. */
static size_t read_pre(const char *s, bool strict, uint8_t *pre)
{
    if (strncmp(s, "alfa", 4) == 0)
    {
        *pre = FWU_PRE_ALFA;
        return 4;
    }

    if (strncmp(s, "beta", 4) == 0)
    {
        *pre = FWU_PRE_BETA;
        return 4;
    }

    if (!strict && strncmp(s, "alpha", 5) == 0)
    {
        *pre = FWU_PRE_ALFA;
        return 5;
    }

    return 0;
}

/* ---- the strict grammar --------------------------------------------------------- */

bool fwu_version_parse_strict(const char *s, fwu_version_t *out)
{
    fwu_version_t v = { 0 };
    size_t n;

    if (s == NULL || out == NULL)
    {
        return false;
    }

    n = read_num(s, true, &v.major);

    if (n == 0 || s[n] != '.')
    {
        return false;
    }

    s += n + 1;
    n = read_num(s, true, &v.minor);

    if (n == 0 || s[n] != '.')
    {
        return false;
    }

    s += n + 1;
    n = read_num(s, true, &v.patch);

    if (n == 0)
    {
        return false;
    }

    s += n;

    if (*s == '-')
    {
        s++;
        n = read_pre(s, true, &v.pre);

        if (n == 0 || s[n] != '.')
        {
            return false;
        }

        s += n + 1;
        n = read_num(s, true, &v.pre_n);

        if (n == 0)
        {
            return false;
        }

        s += n;
    }

    if (*s != '\0')
    {
        return false;
    }

    *out = v;
    return true;
}

/* ---- the tolerant grammar ------------------------------------------------------- */

/** A git describe tail: -<digits>-g<hex>[-dirty], or -dirty alone. */
static bool is_build_tail(const char *s)
{
    if (strcmp(s, "-dirty") == 0 || *s == '\0')
    {
        return true;
    }

    if (*s != '-' || !isdigit((unsigned char)s[1]))
    {
        return false;
    }

    s++;

    while (isdigit((unsigned char)*s))
    {
        s++;
    }

    if (s[0] != '-' || s[1] != 'g' || !isxdigit((unsigned char)s[2]))
    {
        return false;
    }

    s += 2;

    while (isxdigit((unsigned char)*s))
    {
        s++;
    }

    /* "-dirty", or a prefix of it: IDF cuts the version at 31 characters */
    return *s == '\0' || strncmp("-dirty", s, strlen(s)) == 0;
}

bool fwu_version_parse_tolerant(const char *s, fwu_version_t *out)
{
    fwu_version_t v = { 0 };
    size_t n;

    if (s == NULL || out == NULL)
    {
        return false;
    }

    if (*s == 'v' || *s == 'V')
    {
        s++;
    }

    n = read_num(s, false, &v.major);

    if (n == 0 || s[n] != '.')
    {
        return false;
    }

    s += n + 1;
    n = read_num(s, false, &v.minor);

    if (n == 0)
    {
        return false;
    }

    s += n;

    if (*s == '.' && isdigit((unsigned char)s[1]))
    {
        s++;
        n = read_num(s, false, &v.patch);
        s += n;
    }

    if (*s == 'p' || *s == 'u')
    {
        s++; /* the hardware letter of a wican-fw tag */
    }

    if (is_sep(*s))
    {
        uint8_t pre = FWU_PRE_NONE;

        n = read_pre(s + 1, false, &pre);

        if (n > 0 && is_sep(s[1 + n]))
        {
            uint16_t pre_n = 0;
            size_t m = read_num(s + 2 + n, false, &pre_n);

            if (m > 0)
            {
                v.pre = pre;
                v.pre_n = pre_n;
                s += 2 + n + m;
            }
        }
    }

    if (*s != '\0')
    {
        if (!is_build_tail(s))
        {
            return false;
        }

        v.dev = true;
    }

    *out = v;
    return true;
}

/* ---- order and text ------------------------------------------------------------- */

int fwu_version_cmp(const fwu_version_t *a, const fwu_version_t *b)
{
    if (a->major != b->major)
    {
        return (a->major < b->major) ? -1 : 1;
    }

    if (a->minor != b->minor)
    {
        return (a->minor < b->minor) ? -1 : 1;
    }

    if (a->patch != b->patch)
    {
        return (a->patch < b->patch) ? -1 : 1;
    }

    /* a release is greater than any prerelease of the same number */
    if ((a->pre == FWU_PRE_NONE) != (b->pre == FWU_PRE_NONE))
    {
        return (a->pre == FWU_PRE_NONE) ? 1 : -1;
    }

    if (a->pre != b->pre)
    {
        return (a->pre < b->pre) ? -1 : 1; /* alfa (1) below beta (2) */
    }

    if (a->pre_n != b->pre_n)
    {
        return (a->pre_n < b->pre_n) ? -1 : 1;
    }

    return 0;
}

int fwu_version_str(const fwu_version_t *v, char *out, size_t cap)
{
    if (v->pre == FWU_PRE_NONE)
    {
        return snprintf(out, cap, "%u.%u.%u", (unsigned)v->major,
                        (unsigned)v->minor, (unsigned)v->patch);
    }

    return snprintf(out, cap, "%u.%u.%u-%s.%u", (unsigned)v->major,
                    (unsigned)v->minor, (unsigned)v->patch,
                    (v->pre == FWU_PRE_BETA) ? "beta" : "alfa",
                    (unsigned)v->pre_n);
}

/* ---- channels ------------------------------------------------------------------- */

int fwu_channel_from_str(const char *s)
{
    if (s == NULL)
    {
        return -1;
    }

    if (strcmp(s, "stable") == 0)
    {
        return FWU_CH_STABLE;
    }

    if (strcmp(s, "beta") == 0)
    {
        return FWU_CH_BETA;
    }

    if (strcmp(s, "alfa") == 0)
    {
        return FWU_CH_ALFA;
    }

    return -1;
}

const char *fwu_channel_str(uint8_t ch)
{
    switch (ch)
    {
        case FWU_CH_STABLE: return "stable";
        case FWU_CH_BETA:   return "beta";
        case FWU_CH_ALFA:   return "alfa";
        default:            return "?";
    }
}

bool fwu_channel_visible(uint8_t device_channel, uint8_t release_channel)
{
    return release_channel <= device_channel;
}

const char *fwu_layout_str(fwu_layout_t v)
{
    switch (v)
    {
        case FWU_LAYOUT_NONE:     return "none";
        case FWU_LAYOUT_LOSSLESS: return "lossless";
        case FWU_LAYOUT_WIPE:     return "wipe";
        case FWU_LAYOUT_SERIAL:   return "serial";
        default:                  return "unknown";
    }
}

/* ---- URLs ----------------------------------------------------------------------- */

bool fwu_base_url_valid(const char *base_url, bool *is_http)
{
    size_t n;
    bool http;

    if (base_url == NULL)
    {
        return false;
    }

    n = strlen(base_url);

    if (strncmp(base_url, "https://", 8) == 0)
    {
        http = false;
    }
    else if (strncmp(base_url, "http://", 7) == 0)
    {
        http = true;
    }
    else
    {
        return false;
    }

    if (n < (http ? 9U : 10U) || base_url[n - 1] != '/')
    {
        return false; /* no host, or no trailing slash */
    }

    for (size_t i = 0; i < n; i++)
    {
        unsigned char c = (unsigned char)base_url[i];

        if (c <= 0x20U || c >= 0x7FU || c == '"' || c == '\\')
        {
            return false;
        }
    }

    if (is_http != NULL)
    {
        *is_http = http;
    }

    return true;
}

bool fwu_compose_url(const char *base_url, const char *path, char *out,
                     size_t cap)
{
    int n;

    if (base_url == NULL || path == NULL || out == NULL || cap == 0)
    {
        return false;
    }

    while (*path == '/')
    {
        path++; /* the base ends with the slash */
    }

    n = snprintf(out, cap, "%s%s", base_url, path);
    return n > 0 && (size_t)n < cap;
}
