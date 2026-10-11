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
 * @file ip4_routes_core.c
 * @brief PURE: the CIDR parser and formatter, and the table operations
 *        (set / clear / longest-prefix match). No IDF, no lwIP: the host
 *        test compiles this file alone.
 */
#include <stdio.h>
#include <string.h>

#include "ip4_routes_private.h"

static bool parse_octets(const char *s, size_t len, uint32_t *out)
{
    uint32_t ip = 0;
    unsigned octet = 0;
    int digits = 0;
    int dots = 0;

    for (size_t i = 0; i < len; i++)
    {
        char c = s[i];

        if (c >= '0' && c <= '9')
        {
            if (++digits > 3)
            {
                return false;
            }

            octet = octet * 10 + (unsigned)(c - '0');

            if (octet > 255)
            {
                return false;
            }
        }
        else if (c == '.')
        {
            if (digits == 0 || ++dots > 3)
            {
                return false;
            }

            ip = (ip << 8) | octet;
            octet = 0;
            digits = 0;
        }
        else
        {
            return false;
        }
    }

    if (dots != 3 || digits == 0)
    {
        return false;
    }

    *out = (ip << 8) | octet;
    return true;
}

bool ip4_routes_parse_cidr(const char *text, ip4_routes_entry_t *out)
{
    if (text == NULL || out == NULL)
    {
        return false;
    }

    while (*text == ' ' || *text == '\t')
    {
        text++;
    }

    size_t len = strlen(text);

    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t'))
    {
        len--;
    }

    if (len == 0)
    {
        return false;
    }

    const char *slash = memchr(text, '/', len);
    size_t ip_len = slash != NULL ? (size_t)(slash - text) : len;
    unsigned prefix = 32;

    if (slash != NULL)
    {
        const char *p = slash + 1;
        size_t n = len - ip_len - 1;

        if (n == 0 || n > 2)
        {
            return false;
        }

        prefix = 0;

        for (size_t i = 0; i < n; i++)
        {
            if (p[i] < '0' || p[i] > '9')
            {
                return false;
            }

            prefix = prefix * 10 + (unsigned)(p[i] - '0');
        }

        if (prefix > 32)
        {
            return false;
        }
    }

    uint32_t ip;

    if (!parse_octets(text, ip_len, &ip))
    {
        return false;
    }

    uint32_t mask = prefix == 0 ? 0u : (0xFFFFFFFFu << (32 - prefix));

    out->mask = mask;
    out->network = ip & mask;
    return true;
}

int ip4_routes_parse_list(const char *list, ip4_routes_entry_t *out,
                          size_t max, char *err, size_t err_len)
{
    size_t n = 0;

    if (err != NULL && err_len > 0)
    {
        err[0] = '\0';
    }

    if (list == NULL)
    {
        return 0;
    }

    const char *p = list;

    while (*p != '\0')
    {
        while (*p == ',' || *p == ' ' || *p == '\t' || *p == '\n' ||
               *p == '\r' || *p == ';')
        {
            p++;
        }

        if (*p == '\0')
        {
            break;
        }

        const char *end = p;

        while (*end != '\0' && *end != ',' && *end != ' ' && *end != '\t' &&
               *end != '\n' && *end != '\r' && *end != ';')
        {
            end++;
        }

        char token[48];
        size_t tlen = (size_t)(end - p);

        if (tlen >= sizeof(token))
        {
            tlen = sizeof(token) - 1;
        }

        memcpy(token, p, tlen);
        token[tlen] = '\0';

        ip4_routes_entry_t e;

        if (!ip4_routes_parse_cidr(token, &e))
        {
            if (err != NULL && err_len > 0)
            {
                snprintf(err, err_len, "'%s' is not a network in CIDR form "
                         "(a.b.c.d/n)", token);
            }

            return -1;
        }

        if (n >= max)
        {
            if (err != NULL && err_len > 0)
            {
                snprintf(err, err_len, "more than %u routes", (unsigned)max);
            }

            return -1;
        }

        if (out != NULL)
        {
            out[n] = e;
        }

        n++;
        p = end;
    }

    return (int)n;
}

void ip4_routes_format(const ip4_routes_entry_t *e, char *out, size_t len)
{
    unsigned prefix = 0;
    uint32_t m = e->mask;

    while (m & 0x80000000u)
    {
        prefix++;
        m <<= 1;
    }

    snprintf(out, len, "%u.%u.%u.%u/%u", (unsigned)(e->network >> 24) & 255,
             (unsigned)(e->network >> 16) & 255,
             (unsigned)(e->network >> 8) & 255,
             (unsigned)e->network & 255, prefix);
}

int ip4_routes_core_set(ip4_routes_slot_t *table, size_t count, size_t cap,
                        void *owner, const ip4_routes_entry_t *entries,
                        size_t n)
{
    size_t others = 0;

    for (size_t i = 0; i < count; i++)
    {
        if (table[i].owner != owner)
        {
            others++;
        }
    }

    if (others + n > cap)
    {
        return -1;
    }

    size_t w = (size_t)ip4_routes_core_clear(table, count, owner);

    for (size_t i = 0; i < n; i++)
    {
        table[w].owner = owner;
        table[w].e = entries[i];
        w++;
    }

    return (int)w;
}

size_t ip4_routes_core_clear(ip4_routes_slot_t *table, size_t count,
                             void *owner)
{
    size_t w = 0;

    for (size_t i = 0; i < count; i++)
    {
        if (table[i].owner != owner)
        {
            if (w != i)
            {
                table[w] = table[i];
            }

            w++;
        }
    }

    return w;
}

void *ip4_routes_core_match(const ip4_routes_slot_t *table, size_t count,
                            uint32_t dest)
{
    void *best = NULL;
    uint32_t best_mask = 0;
    bool found = false;

    for (size_t i = 0; i < count; i++)
    {
        const ip4_routes_entry_t *e = &table[i].e;

        if ((dest & e->mask) != e->network)
        {
            continue;
        }

        /* a longer prefix is a numerically larger contiguous mask */
        if (!found || e->mask > best_mask)
        {
            found = true;
            best_mask = e->mask;
            best = table[i].owner;
        }
    }

    return best;
}
