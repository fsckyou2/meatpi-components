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
 * @file autopid_dialect.c
 * @brief PURE: the OBD dialects (host-tested, host_test/main/
 *        test_dialect.c). What a dialect asks for, how its answers read,
 *        who answered, and how one responder is addressed. The dialects
 *        themselves are described in autopid_dialect.h.
 *
 * The reply shapes are the OBD chip's own, captured on the bench
 * (2026-10-03, a two-ECU ISO 27145 vehicle on 29-bit ids):
 *
 *   headers off   62 F4 00 98 1B 80 03
 *                 62 F4 00 80 00 00 01
 *   headers on    18 DA F1 3D 07 62 F4 00 80 00 00 01
 *                 18 DA F1 00 07 62 F4 00 98 1B 80 03
 *
 * With headers on the chip prints a 29-bit id as FOUR byte tokens, then
 * the PCI byte. No IDF dependencies.
 */
#include "autopid_private.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_BYTES_MAX 16   /* four id tokens + eight data bytes, and room */
#define ID29_TOKENS    4    /* the id of a 29-bit line, as byte tokens      */

/* ---- names and requests -------------------------------------------------- */

const char *ap_dialect_name(ap_dialect_t dialect)
{
    switch (dialect)
    {
    case AP_DIALECT_UDS:   return "uds";
    case AP_DIALECT_J1939: return "j1939";
    default:               return "obd2";
    }
}

ap_dialect_t ap_dialect_from_name(const char *name)
{
    if (name != NULL && strcmp(name, "uds") == 0)
    {
        return AP_DIALECT_UDS;
    }

    if (name != NULL && strcmp(name, "j1939") == 0)
    {
        return AP_DIALECT_J1939;
    }

    return AP_DIALECT_OBD2;
}

bool ap_dialect_has_requests(ap_dialect_t dialect)
{
    return dialect == AP_DIALECT_OBD2 || dialect == AP_DIALECT_UDS;
}

size_t ap_dialect_pid_cmd(ap_dialect_t dialect, uint8_t pid, char *out,
                          size_t cap)
{
    int n = 0;

    if (out == NULL || cap == 0)
    {
        return 0;
    }

    out[0] = '\0';

    if (dialect == AP_DIALECT_OBD2)
    {
        n = snprintf(out, cap, "01%02X", (unsigned)pid);
    }
    else if (dialect == AP_DIALECT_UDS)
    {
        n = snprintf(out, cap, "22F4%02X", (unsigned)pid);
    }

    if (n <= 0 || (size_t)n >= cap)
    {
        out[0] = '\0';
        return 0;
    }

    return (size_t)n;
}

const char *ap_dialect_vin_cmd(ap_dialect_t dialect)
{
    switch (dialect)
    {
    case AP_DIALECT_OBD2: return "0902";
    case AP_DIALECT_UDS:  return "22F802";
    default:              return "";
    }
}

int ap_dialect_ranges(ap_dialect_t dialect)
{
    switch (dialect)
    {
    case AP_DIALECT_OBD2: return 6;     /* 00..A0, as before dialects     */
    case AP_DIALECT_UDS:  return AP_DIALECT_RANGES_MAX;
    default:              return 0;
    }
}

int ap_dialect_data_shift(ap_dialect_t dialect)
{
    /* `41 <pid> A B` against `62 F4 <pid> A B` */
    return (dialect == AP_DIALECT_UDS) ? 1 : 0;
}

/* ---- reply lines --------------------------------------------------------- */

/** Tokenize one reply line: an optional 3- or 8-hex CAN id as the first
 *  token, then 2-hex bytes. @return byte count, -1 = not a data line. */
static int line_tokens(const char *line, size_t len, uint32_t *id,
                       uint8_t *bytes, size_t max)
{
    size_t pos = 0, n = 0;
    bool first = true;

    *id = UINT32_MAX;

    while (pos < len)
    {
        while (pos < len && (line[pos] == ' ' || line[pos] == '\t'))
        {
            pos++;
        }

        size_t start = pos;

        while (pos < len && line[pos] != ' ' && line[pos] != '\t')
        {
            pos++;
        }

        size_t tlen = pos - start;

        if (tlen == 0)
        {
            continue;
        }

        for (size_t i = 0; i < tlen; i++)
        {
            if (!isxdigit((unsigned char)line[start + i]))
            {
                return -1;
            }
        }

        if (first && (tlen == 3 || tlen == 8))
        {
            char tok[9];

            memcpy(tok, line + start, tlen);
            tok[tlen] = '\0';
            *id = (uint32_t)strtoul(tok, NULL, 16) & 0x1FFFFFFFu;
        }
        else if (tlen == 2)
        {
            if (n < max)
            {
                char tok[3] = { line[start], line[start + 1], '\0' };

                bytes[n++] = (uint8_t)strtol(tok, NULL, 16);
            }
        }
        else
        {
            return -1;          /* "014", "0:", SEARCHING... */
        }

        first = false;
    }

    return (int)n;
}

/** Call @p fn for every line of @p resp with its tokens. */
typedef bool (*line_fn_t)(uint32_t id, const uint8_t *bytes, int n,
                          void *ctx);

static void each_line(const char *resp, line_fn_t fn, void *ctx)
{
    const char *p = resp;

    while (*p != '\0')
    {
        const char *eol = p;

        while (*eol != '\0' && *eol != '\r' && *eol != '\n')
        {
            eol++;
        }

        uint32_t id;
        uint8_t bytes[LINE_BYTES_MAX];
        int n = line_tokens(p, (size_t)(eol - p), &id, bytes, sizeof(bytes));

        if (n > 0 && !fn(id, bytes, n, ctx))
        {
            return;
        }

        p = eol;

        while (*p == '\r' || *p == '\n')
        {
            p++;
        }
    }
}

/** Where @p pfx starts in the line's bytes with @p tail bytes after it;
 *  -1 = not there. */
static int find_prefix(const uint8_t *bytes, int n, const uint8_t *pfx,
                       int plen, int tail)
{
    for (int i = 0; i + plen + tail <= n; i++)
    {
        if (memcmp(&bytes[i], pfx, (size_t)plen) == 0)
        {
            return i;
        }
    }

    return -1;
}

/** The responder of a line whose answer starts at byte @p at: the id token
 *  when there was one, the four leading byte tokens of the chip's 29-bit
 *  print (then the PCI byte, then the answer), else unknown. */
static uint32_t line_responder(uint32_t id, const uint8_t *bytes, int at)
{
    if (id != UINT32_MAX)
    {
        return id;
    }

    if (at == ID29_TOKENS + 1 && bytes[0] <= 0x1F)
    {
        return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
               ((uint32_t)bytes[2] << 8) | bytes[3];
    }

    return UINT32_MAX;
}

/* ---- support bitmaps ----------------------------------------------------- */

typedef struct
{
    uint8_t       pfx[3];
    int           plen;
    ap_veh_ecu_t *out;
    size_t        max;
    int           count;
} bitmap_ctx_t;

static bool bitmap_line(uint32_t id, const uint8_t *bytes, int n, void *arg)
{
    bitmap_ctx_t *ctx = arg;
    int at = find_prefix(bytes, n, ctx->pfx, ctx->plen, 4);

    if (at < 0)
    {
        return true;
    }

    const uint8_t *b = &bytes[at + ctx->plen];
    uint32_t bm = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                  ((uint32_t)b[2] << 8) | b[3];
    uint32_t who = line_responder(id, bytes, at);
    int k = 0;

    while (k < ctx->count && ctx->out[k].id != who)
    {
        k++;
    }

    if (k < ctx->count)
    {
        ctx->out[k].bitmap |= bm;       /* the same id twice: merge      */
    }
    else if ((size_t)ctx->count < ctx->max)
    {
        ctx->out[ctx->count].id = who;
        ctx->out[ctx->count].bitmap = bm;
        ctx->count++;
    }

    return true;
}

int ap_dialect_bitmaps(ap_dialect_t dialect, const char *resp, uint8_t base,
                       ap_veh_ecu_t *out, size_t max)
{
    bitmap_ctx_t ctx = { .out = out, .max = max, .count = 0 };

    if (resp == NULL || out == NULL || max == 0)
    {
        return 0;
    }

    if (dialect == AP_DIALECT_OBD2)
    {
        ctx.pfx[0] = 0x41;
        ctx.pfx[1] = base;
        ctx.plen = 2;
    }
    else if (dialect == AP_DIALECT_UDS)
    {
        ctx.pfx[0] = 0x62;
        ctx.pfx[1] = 0xF4;
        ctx.pfx[2] = base;
        ctx.plen = 3;
    }
    else
    {
        return 0;
    }

    each_line(resp, bitmap_line, &ctx);
    return ctx.count;
}

bool ap_dialect_vin(ap_dialect_t dialect, const char *resp,
                    char vin[AP_VIN_LEN])
{
    if (dialect == AP_DIALECT_OBD2)
    {
        return ap_veh_parse_vin_0902(resp, vin);
    }

    if (dialect == AP_DIALECT_UDS)
    {
        return ap_veh_parse_vin_22f802(resp, vin);
    }

    if (vin != NULL)
    {
        vin[0] = '\0';
    }

    return false;
}

typedef struct
{
    uint8_t id;
    bool    found;
} proto_id_ctx_t;

static bool proto_id_line(uint32_t id, const uint8_t *bytes, int n, void *arg)
{
    static const uint8_t PFX[3] = { 0x62, 0xF8, 0x10 };
    proto_id_ctx_t *ctx = arg;
    int at = find_prefix(bytes, n, PFX, 3, 1);

    (void)id;

    if (at < 0)
    {
        return true;
    }

    ctx->id = bytes[at + 3];
    ctx->found = true;
    return false;               /* the first responder's word is enough */
}

bool ap_dialect_uds_protocol_id(const char *resp, uint8_t *id)
{
    proto_id_ctx_t ctx = { 0, false };

    if (resp == NULL || id == NULL)
    {
        return false;
    }

    each_line(resp, proto_id_line, &ctx);

    if (ctx.found)
    {
        *id = ctx.id;
    }

    return ctx.found;
}

/* ---- a scan's table: every responder, every range ------------------------- */

int ap_dialect_table_add(ap_dialect_ecu_t *tab, int n, int max, int range,
                         const ap_veh_ecu_t *row, int n_row)
{
    if (tab == NULL || row == NULL || range < 0 ||
        range >= AP_DIALECT_RANGES_MAX)
    {
        return n;
    }

    for (int i = 0; i < n_row; i++)
    {
        int k = 0;

        while (k < n && tab[k].id != row[i].id)
        {
            k++;
        }

        if (k == n)
        {
            if (n >= max)
            {
                continue;       /* full: this responder is not kept      */
            }

            memset(&tab[n], 0, sizeof(tab[n]));
            tab[n].id = row[i].id;
            n++;
        }

        tab[k].bitmap[range] |= row[i].bitmap;
    }

    return n;
}

bool ap_dialect_table_more(const ap_dialect_ecu_t *tab, int n, int range)
{
    if (tab == NULL || range < 0 || range >= AP_DIALECT_RANGES_MAX)
    {
        return false;
    }

    for (int i = 0; i < n; i++)
    {
        if (tab[i].bitmap[range] & 1u)
        {
            return true;        /* bit 0 = the next range's bitmap PID   */
        }
    }

    return false;
}

/** Has responder @p e PID @p pid? The bitmap PIDs themselves (00, 20, ..)
 *  are the walk's business, never a row. */
static bool ecu_has(const ap_dialect_ecu_t *e, uint8_t pid)
{
    if (pid == 0 || (pid % 0x20) == 0)
    {
        return false;
    }

    int range = (pid - 1) / 0x20;
    int bit = (pid - 1) % 0x20;         /* 0 = the MSB                  */

    return (e->bitmap[range] >> (31 - bit)) & 1u;
}

bool ap_dialect_table_has(const ap_dialect_ecu_t *tab, int n, uint8_t pid)
{
    for (int i = 0; tab != NULL && i < n; i++)
    {
        if (ecu_has(&tab[i], pid))
        {
            return true;
        }
    }

    return false;
}

uint32_t ap_dialect_pid_owner(const ap_dialect_ecu_t *tab, int n,
                              uint8_t pid)
{
    uint32_t owner = UINT32_MAX;
    bool found = false;

    for (int i = 0; tab != NULL && i < n; i++)
    {
        if (ecu_has(&tab[i], pid) && (!found || tab[i].id < owner))
        {
            owner = tab[i].id;
            found = true;
        }
    }

    return owner;
}

/* ---- addressing ---------------------------------------------------------- */

bool ap_dialect_request_id(uint32_t responder, uint32_t *request)
{
    if (request == NULL)
    {
        return false;
    }

    if (responder >= 0x7E8 && responder <= 0x7EF)
    {
        *request = responder - 8;       /* ISO 15765-4, 11-bit          */
        return true;
    }

    if ((responder & 0x1FFFFF00u) == 0x18DAF100u)
    {
        /* normal fixed addressing: target and source swap places */
        *request = 0x18DA00F1u | ((responder & 0xFFu) << 8);
        return true;
    }

    return false;
}

size_t ap_dialect_header_cmd(uint32_t responder, char *out, size_t cap)
{
    uint32_t req = 0;

    if (out == NULL || cap == 0)
    {
        return 0;
    }

    out[0] = '\0';

    if (!ap_dialect_request_id(responder, &req))
    {
        return 0;
    }

    int n = snprintf(out, cap, (req <= 0x7FF) ? "ATSH%03X" : "ATSH%08X",
                     (unsigned)req);

    if (n <= 0 || (size_t)n >= cap)
    {
        out[0] = '\0';
        return 0;
    }

    return (size_t)n;
}

bool ap_dialect_init_sets_header(const char *init)
{
    /* the chain reader of autopid_chain.c (issue #964) knows every command
       the tables move the chip with; the header is one of its bits */
    return (ap_chain_sets(init) & AP_SETS_HEADER) != 0;
}

int ap_dialect_can_candidates(const ap_bus_t *bus,
                              char out[AP_DIALECT_CAND_LEN])
{
    /* 500 kbit/s before 250, 11-bit before 29-bit: the order a tester
       tries them in; the bus guard strikes what this bus cannot be */
    static const char ORDER[] = "6789";
    int n = 0;

    if (out == NULL)
    {
        return 0;
    }

    for (size_t i = 0; i < sizeof(ORDER) - 1; i++)
    {
        if (ap_guard_decide(bus, ORDER[i], true) == AP_GUARD_ALLOW)
        {
            out[n++] = ORDER[i];
        }
    }

    out[n] = '\0';
    return n;
}
