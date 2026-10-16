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
 * @file translator_slcan_codec.c
 * @brief PURE slcan (Lawicel) ASCII ↔ CAN-wire codec as a bridge_translator_t.
 *        Ported from legacy/slcan.c, all file-scope state moved into the
 *        per-direction ctx (host-testable, reentrant). See
 *        bridge_manager/DESIGN_translators.md §4.
 *
 *   decode : CAN-wire chunk (from the `can` endpoint) → slcan ASCII line
 *   encode : slcan ASCII stream (from the client) → CAN-wire chunk(s)
 *
 * Bridge config rule: the CAN endpoint is side `a` (a→b = decode). Channel
 * commands (O/C/S/Z/M/m/V/N/F…) are ABSORBED: the bus is owned by
 * can_manager settings, and python-can's `slcan` interface needs no replies.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bridge_manager.h"
#include "can_frame_wire.h"

#include "translator_slcan_private.h"

/* client command assembly: one '\r'-terminated line at a time. SLCAN_MTU is
 * ~30; 48 gives headroom. Used by encode; decode ignores it. */
typedef struct
{
    char    line[48];
    uint8_t len;
} slcan_ctx_t;

/* ---- helpers ----------------------------------------------------------- */

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char up_hex(uint8_t n)
{
    n &= 0xF;
    return (char)(n < 10 ? '0' + n : 'A' + (n - 10));
}

/* parse `id_len` hex chars at p into id; returns bytes consumed or -1 */
static int parse_id(const char *p, int id_len, uint32_t *id)
{
    uint32_t v = 0;

    for (int i = 0; i < id_len; i++)
    {
        int n = hex_nibble(p[i]);
        if (n < 0) return -1;
        v = (v << 4) | (uint32_t)n;
    }
    *id = v;
    return id_len;
}

/* ---- ctx_init ---------------------------------------------------------- */

static esp_err_t slcan_ctx_init(void *ctx)
{
    memset(ctx, 0, sizeof(slcan_ctx_t));
    return ESP_OK;
}

/* ---- decode: CAN-wire chunk → slcan ASCII ------------------------------ */

static size_t slcan_format_line(const can_core_frame_t *fp, char *out);

static esp_err_t slcan_decode(void *ctx, const uint8_t *in, size_t len,
                              bridge_sink_fn_t sink, void *sink_arg)
{
    can_core_frame_t f;
    size_t off = 0;
    size_t n;

    (void)ctx;

    /* chunks may be COALESCED (several wire frames per chunk: the can
       endpoint packs them since 2026-07-18); the lines are batched into
       ONE sink call per chunk so frame-bound transports (ws_can: one
       sink call = one WS frame) ride the same lift. Clients must accept
       multiple CR-terminated slcan records per transport frame: that
       is stream-legal slcan. Malformed tail = dropped, stream keeps
       going. */
    char batch[512];
    size_t blen = 0;

    while ((n = can_wire_decode_next(in + off, len - off, &f)) > 0)
    {
        if (blen + 32 > sizeof(batch))
        {
            esp_err_t err = sink(sink_arg, (const uint8_t *)batch, blen);

            if (err != ESP_OK)
            {
                return err;
            }

            blen = 0;
        }

        blen += slcan_format_line(&f, batch + blen);
        off += n;
    }

    return (blen > 0) ? sink(sink_arg, (const uint8_t *)batch, blen)
                      : ESP_OK;
}

/* format one frame as an slcan line into @p out (>= 32 bytes); returns
 * the line length */
static size_t slcan_format_line(const can_core_frame_t *fp, char *out)
{
    const can_core_frame_t f = *fp;
    int i = 0;
    int id_len;

    if (f.ext)
    {
        out[i++] = f.rtr ? 'R' : 'T';
        id_len = 8;
    }
    else
    {
        out[i++] = f.rtr ? 'r' : 't';
        id_len = 3;
    }

    for (int s = (id_len - 1) * 4; s >= 0; s -= 4)
    {
        out[i++] = up_hex((uint8_t)(f.id >> s));
    }

    out[i++] = (char)('0' + (f.dlc > 8 ? 8 : f.dlc));

    if (!f.rtr)
    {
        for (int b = 0; b < f.dlc && b < 8; b++)
        {
            out[i++] = up_hex(f.data[b] >> 4);
            out[i++] = up_hex(f.data[b] & 0xF);
        }
    }

    out[i++] = '\r';

    return (size_t)i;
}

/* ---- the stated bitrate (translator_slcan_private.h) ------------------- */

void (*slcan_bitrate_hook)(uint16_t kbps) = NULL;

uint16_t slcan_bitrate_kbps(char code)
{
    static const uint16_t KBPS[] =
        { 10, 20, 50, 100, 125, 250, 500, 800, 1000, 83 }; /* S0 .. S9 */

    if (code < '0' || code > '9')
    {
        return 0;
    }

    return KBPS[code - '0'];
}

/* ---- encode: one completed slcan line → CAN-wire chunk ----------------- */

static esp_err_t emit_frame(const char *line, uint8_t n,
                            bridge_sink_fn_t sink, void *sink_arg)
{
    can_core_frame_t f;
    uint8_t wire[CAN_WIRE_MAX];
    char type = line[0];
    int id_len;
    int pos = 1;
    int consumed;

    memset(&f, 0, sizeof(f));

    if (type == 't' || type == 'r') { f.ext = false; id_len = 3; }
    else if (type == 'T' || type == 'R') { f.ext = true; id_len = 8; }
    else if (type == 'S' && n == 2)
    {
        /* the client states the bus bitrate (python-can: S6 on open);
           the hook takes it when the node has no verdict of its own */
        uint16_t kbps = slcan_bitrate_kbps(line[1]);

        if (kbps != 0 && slcan_bitrate_hook != NULL)
        {
            slcan_bitrate_hook(kbps);
        }

        return ESP_OK;
    }
    else return ESP_OK; /* not a frame line */

    f.rtr = (type == 'r' || type == 'R');

    if (n < (uint8_t)(1 + id_len + 1)) return ESP_OK; /* too short */

    consumed = parse_id(&line[pos], id_len, &f.id);
    if (consumed < 0) return ESP_OK;
    pos += consumed;

    int dlc = hex_nibble(line[pos++]);
    if (dlc < 0 || dlc > 8) return ESP_OK;
    f.dlc = (uint8_t)dlc;

    if (!f.rtr)
    {
        if (n < (uint8_t)(pos + dlc * 2)) return ESP_OK; /* short data */
        for (int b = 0; b < dlc; b++)
        {
            int hi = hex_nibble(line[pos++]);
            int lo = hex_nibble(line[pos++]);
            if (hi < 0 || lo < 0) return ESP_OK;
            f.data[b] = (uint8_t)((hi << 4) | lo);
        }
    }

    size_t wlen = can_wire_encode(&f, wire);
    return sink(sink_arg, wire, wlen);
}

static esp_err_t slcan_encode(void *vctx, const uint8_t *in, size_t len,
                              bridge_sink_fn_t sink, void *sink_arg)
{
    slcan_ctx_t *ctx = vctx;

    for (size_t i = 0; i < len; i++)
    {
        char c = (char)in[i];

        if (c == '\r')
        {
            if (ctx->len > 0)
            {
                esp_err_t err = emit_frame(ctx->line, ctx->len, sink, sink_arg);
                ctx->len = 0;
                if (err != ESP_OK) return err;
            }
            /* empty line / bare '\r' → nothing */
            continue;
        }

        if (c == '\n')
        {
            continue; /* tolerate CRLF */
        }

        if (ctx->len < sizeof(ctx->line) - 1)
        {
            ctx->line[ctx->len++] = c;
        }
        else
        {
            ctx->len = 0; /* overflow → drop the line, no corruption */
        }
    }

    return ESP_OK;
}

/* ---- the translator descriptor (referenced by init + host tests) ------- */

const bridge_translator_t translator_slcan_desc =
{
    .name     = "slcan",
    .ctx_size = sizeof(slcan_ctx_t),
    .ctx_init = slcan_ctx_init,
    .decode   = slcan_decode,
    .encode   = slcan_encode,
};
