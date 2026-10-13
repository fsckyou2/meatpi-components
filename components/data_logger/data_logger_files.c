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
 * @file data_logger_files.c
 * @brief Pure helpers (host-tested; no esp deps): file names per
 *        stream prefix, the directory-scan fold, the hex filter
 *        parser, and the record/row encoders for every non-sqlite
 *        engine (.wdl, csv, candump, asc, jsonl, MDF4, BLF).
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "data_logger_private.h"

#define DL_EPOCH_LEN  10

/* every stream's prefix and every engine's extension: retention spans
 * engine switches, so a user who flips sqlite->csv still ages out
 * their old .db files (within the same stream prefix) */
static const char *const PREFIXES[] = { DL_PREFIX_PARAM, DL_PREFIX_CAN };
static const char *const EXTS[] =
{
    ".db", ".csv", ".wdl", ".log", ".asc", ".jsonl", ".mf4", ".blf",
};

bool dl_files_parse(const char *fname, int64_t *epoch_out)
{
    if (fname == NULL)
    {
        return false;
    }

    const char *digits = NULL;

    for (size_t i = 0; i < sizeof(PREFIXES) / sizeof(PREFIXES[0]); i++)
    {
        size_t n = strlen(PREFIXES[i]);

        if (strncmp(fname, PREFIXES[i], n) == 0)
        {
            digits = fname + n;
            break;
        }
    }

    if (digits == NULL)
    {
        return false;
    }

    int64_t epoch = 0;

    for (int i = 0; i < DL_EPOCH_LEN; i++)
    {
        if (digits[i] < '0' || digits[i] > '9')
        {
            return false;
        }

        epoch = epoch * 10 + (digits[i] - '0');
    }

    const char *ext = digits + DL_EPOCH_LEN;
    bool known = false;

    for (size_t i = 0; i < sizeof(EXTS) / sizeof(EXTS[0]); i++)
    {
        if (strcmp(ext, EXTS[i]) == 0)
        {
            known = true;
            break;
        }
    }

    if (!known)
    {
        return false;
    }

    if (epoch_out != NULL)
    {
        *epoch_out = epoch;
    }

    return true;
}

void dl_files_make(char *out, size_t len, const char *prefix,
                   int64_t epoch, const char *ext)
{
    if (epoch < 0)
    {
        epoch = 0;
    }

    snprintf(out, len, "%s%010lld%s", prefix, (long long)epoch, ext);
}

void dl_scan_init(dl_scan_t *s, const char *prefix)
{
    memset(s, 0, sizeof(*s));
    s->prefix = prefix;
}

void dl_scan_add(dl_scan_t *s, const char *fname)
{
    if (!dl_files_parse(fname, NULL) ||
        strlen(fname) >= sizeof(s->oldest))
    {
        return;
    }

    if (s->prefix != NULL &&
        strncmp(fname, s->prefix, strlen(s->prefix)) != 0)
    {
        return; /* the other stream's file */
    }

    /* zero-padded epoch => strcmp order IS chronological order */
    if (s->count == 0 || strcmp(fname, s->oldest) < 0)
    {
        strcpy(s->oldest, fname);
    }

    if (s->count == 0 || strcmp(fname, s->newest) > 0)
    {
        strcpy(s->newest, fname);
    }

    s->count++;
}

bool dl_parse_hex_u32(const char *s, uint32_t *out)
{
    if (s == NULL || out == NULL)
    {
        return false;
    }

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
    {
        s += 2;
    }

    if (s[0] == '\0')
    {
        return false;
    }

    uint32_t v = 0;
    int n = 0;

    for (; *s != '\0'; s++, n++)
    {
        int d;

        if (*s >= '0' && *s <= '9')
        {
            d = *s - '0';
        }
        else if (*s >= 'a' && *s <= 'f')
        {
            d = *s - 'a' + 10;
        }
        else if (*s >= 'A' && *s <= 'F')
        {
            d = *s - 'A' + 10;
        }
        else
        {
            return false;
        }

        if (n >= 8)
        {
            return false; /* > 32 bits */
        }

        v = (v << 4) | (uint32_t)d;
    }

    *out = v;
    return true;
}

/* ---- frame encoders --------------------------------------------------------

   .wdl 0x03 frame record (little-endian, variable length):
     u8 0x03 | i64 ts_ms | u32 id_word | u8 dlc | data[dlc]
   id_word = id (bits 0..28) | ext << 31 | rtr << 30. A torn tail frame
   is detectable by framing (type byte + fixed head + dlc). */

size_t dl_wdl_encode_frame(uint8_t *buf, int64_t ts_ms, uint32_t id,
                           uint8_t flags, const uint8_t *data,
                           uint8_t dlc)
{
    if (dlc > 8)
    {
        dlc = 8;
    }

    uint32_t idw = (id & 0x1FFFFFFFu) |
                   ((flags & DL_FRAME_EXT) ? 0x80000000u : 0) |
                   ((flags & DL_FRAME_RTR) ? 0x40000000u : 0);
    size_t off = 0;

    buf[off++] = 0x03;

    for (int i = 0; i < 8; i++)
    {
        buf[off++] = (uint8_t)((uint64_t)ts_ms >> (8 * i));
    }

    for (int i = 0; i < 4; i++)
    {
        buf[off++] = (uint8_t)(idw >> (8 * i));
    }

    buf[off++] = dlc;

    for (uint8_t i = 0; i < dlc; i++)
    {
        buf[off++] = data[i];
    }

    return off;
}

int dl_csv_frame_row(char *out, size_t cap, int64_t ts_ms, uint32_t id,
                     uint8_t flags, const uint8_t *data, uint8_t dlc)
{
    if (dlc > 8)
    {
        dlc = 8;
    }

    int n = snprintf(out, cap, "%lld,%lX,%d,%d,%u,", (long long)ts_ms,
                     (unsigned long)(id & 0x1FFFFFFFu),
                     (flags & DL_FRAME_EXT) ? 1 : 0,
                     (flags & DL_FRAME_RTR) ? 1 : 0, dlc);

    if (n < 0 || (size_t)n + (size_t)dlc * 2 + 2 > cap)
    {
        return -1;
    }

    for (uint8_t i = 0; i < dlc; i++)
    {
        n += snprintf(out + n, cap - (size_t)n, "%02X", data[i]);
    }

    out[n++] = '\n';
    out[n] = '\0';
    return n;
}

/* ---- more text rows (addendum 2) -------------------------------------------

   candump: "(sec.usec) can0 ID#HEXDATA", can-utils / SavvyCAN;
   asc:     Vector CANalyzer text data row (seconds since file start);
   jsonl:   one JSON object per line (names come from the bounded
            registry: no quote escaping by design). */

int dl_candump_row(char *out, size_t cap, int64_t ts_ms, uint32_t id,
                   uint8_t flags, const uint8_t *data, uint8_t dlc)
{
    if (dlc > 8)
    {
        dlc = 8;
    }

    int n = snprintf(out, cap, "(%lld.%06d) can0 ",
                     (long long)(ts_ms / 1000),
                     (int)(ts_ms % 1000) * 1000);

    if (n < 0 || (size_t)n >= cap)
    {
        return -1;
    }

    n += snprintf(out + n, cap - (size_t)n,
                  (flags & DL_FRAME_EXT) ? "%08lX#" : "%03lX#",
                  (unsigned long)(id & 0x1FFFFFFFu));

    if ((size_t)n + (size_t)dlc * 2 + 3 > cap)
    {
        return -1;
    }

    if (flags & DL_FRAME_RTR)
    {
        out[n++] = 'R';
    }
    else
    {
        for (uint8_t i = 0; i < dlc; i++)
        {
            n += snprintf(out + n, cap - (size_t)n, "%02X", data[i]);
        }
    }

    out[n++] = '\n';
    out[n] = '\0';
    return n;
}

int dl_asc_row(char *out, size_t cap, int64_t rel_ms, uint32_t id,
               uint8_t flags, const uint8_t *data, uint8_t dlc)
{
    if (dlc > 8)
    {
        dlc = 8;
    }

    char ids[16];

    snprintf(ids, sizeof(ids), (flags & DL_FRAME_EXT) ? "%lXx" : "%lX",
             (unsigned long)(id & 0x1FFFFFFFu));

    double t = (double)rel_ms / 1000.0;
    int n;

    if (flags & DL_FRAME_RTR)
    {
        n = snprintf(out, cap, "%11.6f 1  %-15s Rx   r\n", t, ids);
        return (n > 0 && (size_t)n < cap) ? n : -1;
    }

    n = snprintf(out, cap, "%11.6f 1  %-15s Rx   d %u", t, ids, dlc);

    if (n < 0 || (size_t)n + (size_t)dlc * 3 + 2 > cap)
    {
        return -1;
    }

    for (uint8_t i = 0; i < dlc; i++)
    {
        n += snprintf(out + n, cap - (size_t)n, " %02X", data[i]);
    }

    out[n++] = '\n';
    out[n] = '\0';
    return n;
}

int dl_jsonl_param_row(char *out, size_t cap, int64_t ts_ms,
                       const char *source, const char *name,
                       double value)
{
    int n = snprintf(out, cap,
                     "{\"ts\":%lld,\"param\":\"%s.%s\","
                     "\"value\":%.10g}\n",
                     (long long)ts_ms, source, name, value);

    return (n > 0 && (size_t)n < cap) ? n : -1;
}

int dl_jsonl_frame_row(char *out, size_t cap, int64_t ts_ms,
                       uint32_t id, uint8_t flags, const uint8_t *data,
                       uint8_t dlc)
{
    if (dlc > 8)
    {
        dlc = 8;
    }

    int n = snprintf(out, cap,
                     "{\"ts\":%lld,\"id\":\"%lX\",\"ext\":%d,"
                     "\"rtr\":%d,\"data\":\"",
                     (long long)ts_ms,
                     (unsigned long)(id & 0x1FFFFFFFu),
                     (flags & DL_FRAME_EXT) ? 1 : 0,
                     (flags & DL_FRAME_RTR) ? 1 : 0);

    if (n < 0 || (size_t)n + (size_t)dlc * 2 + 4 > cap)
    {
        return -1;
    }

    for (uint8_t i = 0; i < dlc; i++)
    {
        n += snprintf(out + n, cap - (size_t)n, "%02X", data[i]);
    }

    n += snprintf(out + n, cap - (size_t)n, "\"}\n");
    return n;
}

/* ---- little-endian scalar emitters (MF4 + BLF builders) --------------------- */

static void put_u16le(uint8_t *b, uint16_t v)
{
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
}

static void put_u32le(uint8_t *b, uint32_t v)
{
    put_u16le(b, (uint16_t)v);
    put_u16le(b + 2, (uint16_t)(v >> 16));
}

static void put_u64le(uint8_t *b, uint64_t v)
{
    put_u32le(b, (uint32_t)v);
    put_u32le(b + 4, (uint32_t)(v >> 32));
}

static void put_f64le(uint8_t *b, double v)
{
    /* both the S3 and every host-test target are little-endian IEEE */
    memcpy(b, &v, 8);
}

/* ---- MDF 4.10 (ASAM) --------------------------------------------------------

   Minimal spec-valid layout, ONE sorted data group of CAN_DataFrame
   records (22 B): t f64 [s since HD start] | ID u32 | Flags u8 |
   DLC u8 | DataBytes[8]. Physical order:
     ID HD FH MD(fh) DG CG SI TXacq TXsi (CN TX)x5 DT
   commit() patches the DT block length + CG cycle_count in place, so
   a torn file is readable up to the last commit. */

#define MF4_XML_FH "<FHcomment><TX>WiCAN data_logger</TX>" \
    "<tool_id>WiCAN</tool_id><tool_vendor>meatPi</tool_vendor>" \
    "<tool_version>v6</tool_version></FHcomment>"

static size_t mf4_pad8(size_t n)
{
    return (n + 7) & ~(size_t)7;
}

static size_t mf4_txsz(const char *s)
{
    return 24 + mf4_pad8(strlen(s) + 1);
}

/* block header at buf+off; returns the offset of the first link */
static size_t mf4_block(uint8_t *buf, size_t off, const char *id,
                        uint64_t length, uint64_t nlinks)
{
    memset(buf + off, 0, (size_t)length);
    memcpy(buf + off, id, 4);
    put_u64le(buf + off + 8, length);
    put_u64le(buf + off + 16, nlinks);
    return off + 24;
}

static size_t mf4_tx(uint8_t *buf, size_t off, const char *s)
{
    size_t sz = mf4_txsz(s);

    mf4_block(buf, off, "##TX", sz, 0);
    memcpy(buf + off + 24, s, strlen(s));
    return sz;
}

static void mf4_cn(uint8_t *buf, size_t off, uint64_t next_off,
                   uint64_t tx_off, uint8_t ch_type, uint8_t sync_type,
                   uint8_t data_type, uint32_t byte_offset,
                   uint32_t bit_count)
{
    size_t lk = mf4_block(buf, off, "##CN", 160, 8);

    put_u64le(buf + lk, next_off);          /* cn_next            */
    put_u64le(buf + lk + 16, tx_off);       /* cn_tx_name         */
    /* composition/source/conversion/data/unit/comment = 0 */

    uint8_t *d = buf + off + 24 + 64;

    d[0] = ch_type;
    d[1] = sync_type;
    d[2] = data_type;
    d[3] = 0;                               /* bit offset          */
    put_u32le(d + 4, byte_offset);
    put_u32le(d + 8, bit_count);
    /* flags/inval/precision/attachment + min/max/limits stay 0 */
}

size_t dl_mf4_prelude(uint8_t *buf, size_t cap, int64_t start_ms,
                      uint32_t *dt_len_off, uint32_t *cg_cycle_off)
{
    static const char *const CN_NAMES[] =
    {
        "t", "CAN_DataFrame.ID", "CAN_DataFrame.Flags",
        "CAN_DataFrame.DLC", "CAN_DataFrame.DataBytes",
    };

    /* layout offsets (sequential) */
    size_t off_hd = 64;
    size_t off_fh = off_hd + 104;
    size_t off_md = off_fh + 56;
    size_t md_sz = 24 + mf4_pad8(strlen(MF4_XML_FH) + 1);
    size_t off_dg = off_md + md_sz;
    size_t off_cg = off_dg + 64;
    size_t off_si = off_cg + 104;
    size_t off_txacq = off_si + 56;
    size_t off_txsi = off_txacq + mf4_txsz("CAN_DataFrame");
    size_t off_cn[5];
    size_t off_tx[5];
    size_t cursor = off_txsi + mf4_txsz("CAN");

    for (int i = 0; i < 5; i++)
    {
        off_cn[i] = cursor;
        off_tx[i] = cursor + 160;
        cursor = off_tx[i] + mf4_txsz(CN_NAMES[i]);
    }

    size_t off_dt = cursor;
    size_t total = off_dt + 24;

    if (total > cap)
    {
        return 0;
    }

    uint64_t start_ns = (uint64_t)start_ms * 1000000ULL;
    size_t lk;

    /* ID block (64 B, plain bytes not a ##-block) */
    memset(buf, 0, 64);
    memcpy(buf, "MDF     ", 8);
    memcpy(buf + 8, "4.10    ", 8);
    memcpy(buf + 16, "WiCAN   ", 8);
    put_u16le(buf + 28, 410);

    /* HD (its start time sits at DL_MF4_HD_START_OFF: commit re-patches) */
    lk = mf4_block(buf, off_hd, "##HD", 104, 6);
    put_u64le(buf + lk, off_dg);            /* hd_dg_first        */
    put_u64le(buf + lk + 8, off_fh);        /* hd_fh_first        */
    put_u64le(buf + DL_MF4_HD_START_OFF, start_ns);

    /* FH + its MD comment (start at DL_MF4_FH_START_OFF) */
    lk = mf4_block(buf, off_fh, "##FH", 56, 2);
    put_u64le(buf + lk + 8, off_md);        /* fh_md_comment      */
    put_u64le(buf + DL_MF4_FH_START_OFF, start_ns);
    mf4_block(buf, off_md, "##MD", md_sz, 0);
    memcpy(buf + off_md + 24, MF4_XML_FH, strlen(MF4_XML_FH));

    /* DG (record id size 0 = sorted) */
    lk = mf4_block(buf, off_dg, "##DG", 64, 4);
    put_u64le(buf + lk + 8, off_cg);        /* dg_cg_first        */
    put_u64le(buf + lk + 16, off_dt);       /* dg_data            */

    /* CG */
    lk = mf4_block(buf, off_cg, "##CG", 104, 6);
    put_u64le(buf + lk + 8, off_cn[0]);     /* cg_cn_first        */
    put_u64le(buf + lk + 16, off_txacq);    /* cg_tx_acq_name     */
    put_u64le(buf + lk + 24, off_si);       /* cg_si_acq_source   */
    put_u32le(buf + off_cg + 24 + 48 + 24, DL_MF4_REC_SIZE);

    /* SI (source: BUS / CAN) */
    lk = mf4_block(buf, off_si, "##SI", 56, 3);
    put_u64le(buf + lk, off_txsi);          /* si_tx_name         */
    buf[off_si + 24 + 24] = 2;              /* si_type = BUS      */
    buf[off_si + 24 + 24 + 1] = 2;          /* si_bus_type = CAN  */

    mf4_tx(buf, off_txacq, "CAN_DataFrame");
    mf4_tx(buf, off_txsi, "CAN");

    /* channels: t f64 master | ID u32 | Flags u8 | DLC u8 | data[8] */
    mf4_cn(buf, off_cn[0], off_cn[1], off_tx[0], 2, 1, 4, 0, 64);
    mf4_cn(buf, off_cn[1], off_cn[2], off_tx[1], 0, 0, 0, 8, 32);
    mf4_cn(buf, off_cn[2], off_cn[3], off_tx[2], 0, 0, 0, 12, 8);
    mf4_cn(buf, off_cn[3], off_cn[4], off_tx[3], 0, 0, 0, 13, 8);
    mf4_cn(buf, off_cn[4], 0, off_tx[4], 0, 0, 10, 14, 64);

    for (int i = 0; i < 5; i++)
    {
        mf4_tx(buf, off_tx[i], CN_NAMES[i]);
    }

    /* DT header: length grows by 22 per record (patched at commit) */
    mf4_block(buf, off_dt, "##DT", 24, 0);

    *dt_len_off = (uint32_t)(off_dt + 8);
    *cg_cycle_off = (uint32_t)(off_cg + 24 + 48 + 8);
    return total;
}

size_t dl_mf4_record(uint8_t *buf, int64_t rel_ms, const dl_record_t *rec)
{
    put_f64le(buf, (double)rel_ms / 1000.0);
    put_u32le(buf + 8, rec->u.f.id & 0x1FFFFFFFu);
    buf[12] = rec->u.f.flags;
    buf[13] = (rec->u.f.dlc <= 8) ? rec->u.f.dlc : 8;
    memset(buf + 14, 0, 8);
    memcpy(buf + 14, rec->u.f.data, buf[13]);
    return DL_MF4_REC_SIZE;
}

/* ---- Vector BLF --------------------------------------------------------------

   python-can-compatible: "LOGG" 144-byte file header (counters + end
   time re-written on every commit) + LOG_CONTAINER objects
   (compressionMethod=0, uncompressed v1: the zlib variant is a v2
   candidate via the ESP ROM miniz) of concatenated 48-byte
   CAN_MESSAGE objects (ns timestamps relative to the header start). */

static void blf_systemtime(uint8_t *b, int64_t ms)
{
    time_t sec = (time_t)(ms / 1000);
    struct tm tmv;

    gmtime_r(&sec, &tmv);
    put_u16le(b, (uint16_t)(tmv.tm_year + 1900));
    put_u16le(b + 2, (uint16_t)(tmv.tm_mon + 1));
    put_u16le(b + 4, (uint16_t)tmv.tm_wday);
    put_u16le(b + 6, (uint16_t)tmv.tm_mday);
    put_u16le(b + 8, (uint16_t)tmv.tm_hour);
    put_u16le(b + 10, (uint16_t)tmv.tm_min);
    put_u16le(b + 12, (uint16_t)tmv.tm_sec);
    put_u16le(b + 14, (uint16_t)(ms % 1000));
}

size_t dl_blf_file_header(uint8_t *buf, int64_t start_ms, int64_t end_ms,
                          uint64_t file_size, uint64_t uncompressed,
                          uint32_t objects)
{
    memset(buf, 0, DL_BLF_HDR_SIZE);
    memcpy(buf, "LOGG", 4);
    put_u32le(buf + 4, DL_BLF_HDR_SIZE);
    /* app id/version 0 (unknown); binary log version 2.0 */
    buf[12] = 2;
    put_u64le(buf + 16, file_size);
    put_u64le(buf + 24, uncompressed);
    put_u32le(buf + 32, objects);
    put_u32le(buf + 36, 0);                 /* objects read        */
    blf_systemtime(buf + 40, start_ms);
    blf_systemtime(buf + 56, end_ms);
    return DL_BLF_HDR_SIZE;
}

size_t dl_blf_container_header(uint8_t *buf, uint32_t payload_len)
{
    memset(buf, 0, DL_BLF_CONT_HDR);
    memcpy(buf, "LOBJ", 4);
    put_u16le(buf + 4, 16);                 /* base header size    */
    put_u16le(buf + 6, 1);                  /* header version      */
    put_u32le(buf + 8, DL_BLF_CONT_HDR + payload_len);
    put_u32le(buf + 12, 10);                /* LOG_CONTAINER       */
    put_u16le(buf + 16, 0);                 /* method 0 = none     */
    put_u32le(buf + 24, payload_len);       /* uncompressed size   */
    return DL_BLF_CONT_HDR;
}

size_t dl_blf_message(uint8_t *buf, int64_t rel_ms, const dl_record_t *rec)
{
    uint8_t dlc = (rec->u.f.dlc <= 8) ? rec->u.f.dlc : 8;
    uint64_t ns = (rel_ms > 0) ? (uint64_t)rel_ms * 1000000ULL : 0;
    uint32_t id = rec->u.f.id & 0x1FFFFFFFu;

    if (rec->u.f.flags & DL_FRAME_EXT)
    {
        id |= 0x80000000u;
    }

    memset(buf, 0, DL_BLF_MSG_SIZE);
    memcpy(buf, "LOBJ", 4);
    put_u16le(buf + 4, 16);
    put_u16le(buf + 6, 1);
    put_u32le(buf + 8, DL_BLF_MSG_SIZE);
    put_u32le(buf + 12, 1);                 /* CAN_MESSAGE         */
    put_u32le(buf + 16, 2);                 /* flags: 1 ns time    */
    put_u64le(buf + 24, ns);
    put_u16le(buf + 32, 1);                 /* channel 1           */
    buf[34] = (rec->u.f.flags & DL_FRAME_RTR) ? 0x80 : 0;
    buf[35] = dlc;
    put_u32le(buf + 36, id);
    memcpy(buf + 40, rec->u.f.data, dlc);
    return DL_BLF_MSG_SIZE;
}
