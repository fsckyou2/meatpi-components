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
 * @file autopid_std.c
 * @brief Standard (SAE mode-01) PIDs: the vendored legacy table as data,
 *        the PURE bit_start->expression mapping + scan-response parser
 *        (host-tested), the mode-02 freeze-frame decode and the table's
 *        JSON view. The async support scan itself lives in
 *        autopid_std_scan.c (split 2026-10-01, vehicle identity phases).
 *        This is the ONE file that includes obd2_standard_pids.h (the
 *        header has no include guard and carries the 84 KB table).
 */
#include "autopid_private.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef AUTOPID_HOST_TEST
#include "cJSON.h"
#endif

#include "obd2_standard_pids.h"

/* ---- PURE: legacy table row -> v6 expression ---------------------------------- */

/** Print @p v as a parser-safe decimal literal: %.10g normally, but the
 *  grammar has no exponent form, tiny scales (1/32768) fall back to
 *  fixed-point with trailing zeros trimmed. */
static void fmt_literal(double v, char *out, size_t len)
{
    snprintf(out, len, "%.10g", v);

    if (strchr(out, 'e') != NULL)
    {
        snprintf(out, len, "%.15f", v);

        char *dot = strchr(out, '.');

        if (dot != NULL)
        {
            char *end = out + strlen(out) - 1;

            while (end > dot + 1 && *end == '0')
            {
                *end-- = '\0';
            }
        }
    }
}

/**
 * The literal of @p scale. The ratio of two integers `N/D` when the row's
 * @p max is the value at full scale (every raw bit set, within float noise)
 * and N = max - offset is an integer: full scale then decodes to max
 * EXACTLY. The table's scales are floats printed with ten digits: a percent
 * row's 0.3921568627f prints as 0.3921568692 and a raw 0xFF decoded to
 * 100.000001646, a hair above the row's max (issue #1004); `B2*100/255`
 * decodes it to 100. Otherwise the decimal literal. @return true for the
 * ratio form.
 */
static bool scale_literal(double scale, double offset, int bits, double max,
                          char *out, size_t len)
{
    double raw_max = ldexp(1.0, bits) - 1.0;
    double full = raw_max * scale + offset;
    double n = max - offset;
    double tol = fabs(max) * AP_BOUNDS_TOL_REL;

    if (tol < AP_BOUNDS_TOL_ABS)
    {
        tol = AP_BOUNDS_TOL_ABS;
    }

    if (!isnan(max) && scale != 1.0 && fabs(full - max) <= tol && n > 0 &&
        fabs(n - nearbyint(n)) < 1e-9)
    {
        snprintf(out, len, "%.0f/%.0f", nearbyint(n), raw_max);
        return true;
    }

    fmt_literal(scale, out, len);
    return false;
}

esp_err_t ap_std_expression(uint8_t bit_start, uint8_t bit_length,
                            double scale, double offset, char *buf,
                            size_t buf_len)
{
    return ap_std_expression_bounded(bit_start, bit_length, scale, offset,
                                     NAN, NAN, 0, buf, buf_len);
}

esp_err_t ap_std_expression_shift(uint8_t bit_start, uint8_t bit_length,
                                  double scale, double offset, int shift,
                                  char *buf, size_t buf_len)
{
    return ap_std_expression_bounded(bit_start, bit_length, scale, offset,
                                     NAN, NAN, shift, buf, buf_len);
}

esp_err_t ap_std_expression_bounded(uint8_t bit_start, uint8_t bit_length,
                                    double scale, double offset, double min,
                                    double max, int shift, char *buf,
                                    size_t buf_len)
{
    if (buf == NULL || buf_len == 0 || shift < 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* placeholder rows (bit_start 0 pairs with bit_length 0) */
    if (bit_length == 0 || bit_start < 8)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* drop legacy's PCI byte; a dialect with a longer echo moves on */
    int first = bit_start / 8 - 1 + shift;
    int count = (bit_length + 7) / 8;
    char base[32];

    /* a range below zero with no offset is a two's complement raw (the
       evaporative system vapour pressure, -8192..8192 kPa at 0.25 per
       bit): the parser's signed span, else the plain bytes */
    char kind = (!isnan(min) && min < 0.0 && offset == 0.0) ? 'S' : 'B';

    if (count == 1)
    {
        snprintf(base, sizeof(base), "%c%d", kind, first);
    }
    else
    {
        snprintf(base, sizeof(base), "[%c%d:%c%d]", kind, first, kind,
                 first + count - 1);
    }

    /* value = raw * scale + offset */
    char sc[32], of[32];

    (void)scale_literal(scale, offset, bit_length, (kind == 'B') ? max : NAN,
                        sc, sizeof(sc));
    fmt_literal(fabs(offset), of, sizeof(of));

    int n;

    if (scale == 1.0 && offset == 0.0)
    {
        n = snprintf(buf, buf_len, "%s", base);
    }
    else if (offset == 0.0)
    {
        n = snprintf(buf, buf_len, "%s*%s", base, sc);
    }
    else if (scale == 1.0)
    {
        n = snprintf(buf, buf_len, "%s%c%s", base,
                     (offset < 0) ? '-' : '+', of);
    }
    else
    {
        n = snprintf(buf, buf_len, "%s*%s%c%s", base, sc,
                     (offset < 0) ? '-' : '+', of);
    }

    return (n > 0 && (size_t)n < buf_len) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

/* ---- PURE: support-bitmap response parser -------------------------------------- */

/** Parse one line's hex byte tokens into @p bytes; -1 on non-hex noise. */
static int line_bytes(const char *line, size_t len, uint8_t *bytes,
                      size_t max)
{
    size_t n = 0, pos = 0;

    while (pos < len && n < max)
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

        /* accept 2-hex byte tokens and 3-hex CAN headers (skipped by
           the 41-anchor below); anything else = not a data line */
        if (tlen > 3)
        {
            return -1;
        }

        for (size_t i = 0; i < tlen; i++)
        {
            if (!isxdigit((unsigned char)line[start + i]))
            {
                return -1;
            }
        }

        if (tlen <= 2)
        {
            char tok[3] = { line[start], (tlen == 2) ? line[start + 1]
                                                     : '\0', '\0' };

            bytes[n++] = (uint8_t)strtol(tok, NULL, 16);
        }
        /* 3-hex header token: ignore (the anchor search skips it) */
    }

    return (int)n;
}

bool ap_std_scan_parse(const char *resp, uint8_t expect_pid,
                       uint32_t *bitmap)
{
    if (resp == NULL || bitmap == NULL)
    {
        return false;
    }

    *bitmap = 0;

    bool found = false;
    const char *p = resp;

    while (*p != '\0')
    {
        const char *eol = p;

        while (*eol != '\0' && *eol != '\r' && *eol != '\n')
        {
            eol++;
        }

        uint8_t bytes[16];
        int n = line_bytes(p, (size_t)(eol - p), bytes,
                           sizeof(bytes));

        /* anchor on "41 <pid>" anywhere in the line: headers-on rows
           (11-bit, 3-hex header + PCI) work because the anchor skips
           leading bytes. Noise lines (SEARCHING, "N:" ISO rows, 8-hex
           29-bit headers) fail tokenization and are skipped; the scan
           prelude pins ATH0 so bitmap rows are plain "41 XX ..". */
        for (int i = 0; n > 0 && i + 1 < n; i++)
        {
            if (bytes[i] == 0x41 && bytes[i + 1] == expect_pid)
            {
                uint32_t bm = 0;
                int have = 0;

                for (int j = 0; j < 4 && i + 2 + j < n; j++)
                {
                    bm = (bm << 8) | bytes[i + 2 + j];
                    have++;
                }

                if (have == 4)
                {
                    *bitmap |= bm;      /* multi-ECU rows OR together */
                    found = true;
                }

                break;
            }
        }

        p = eol;

        while (*p == '\r' || *p == '\n')
        {
            p++;
        }
    }

    return found;
}

/* ---- PURE: mode-02 freeze-frame decode (TASK_dtc §14) --------------------------- */

int ap_frz_decode(const uint8_t *payload, size_t len, ap_frz_val_t *out,
                  int n, int max)
{
    /* [0x42, pid, frame, A, B, …], same byte semantics as the mode-01
       expressions, shifted by the extra frame byte */
    if (payload == NULL || out == NULL || len < 4 || payload[0] != 0x42)
    {
        return n;
    }

    const std_pid_t *info = get_pid(payload[1]);

    if (info == NULL || info->base_name == NULL)
    {
        return n;
    }

    for (int i = 0; i < info->num_params && n < max; i++)
    {
        const std_parameter_t *prm = &info->params[i];

        /* the ap_std_expression support rule: placeholders + sub-PCI
           rows are not decodable */
        if (prm->name == NULL || prm->bit_length == 0 || prm->bit_start < 8)
        {
            continue;
        }

        /* mode-01 payload index (B0 = service echo), then +1 for the
           mode-02 frame byte */
        int first = prm->bit_start / 8 - 1;
        int count = (prm->bit_length + 7) / 8;

        if ((size_t)(first + 1 + count) > len)
        {
            continue;   /* short payload: skip, keep what fits */
        }

        uint32_t raw = 0;

        for (int b = 0; b < count; b++)
        {
            raw = (raw << 8) | payload[first + 1 + b];
        }

        snprintf(out[n].name, sizeof(out[n].name), "%s", prm->name);
        snprintf(out[n].unit, sizeof(out[n].unit), "%s",
                 (prm->unit != NULL) ? prm->unit : "");
        out[n].value = (float)raw * prm->scale + prm->offset;
        n++;
    }

    return n;
}

/* ---- target half: table JSON (the scan job: autopid_std_scan.c) ---------------- */

#ifndef AUTOPID_HOST_TEST

/** Append one table entry (with generated expressions) to @p arr. */
bool ap_std_entry_to_json(cJSON *arr, uint8_t pid, ap_dialect_t dialect,
                          uint32_t owner)
{
    const std_pid_t *info = get_pid(pid);
    char cmd[12];

    if (info == NULL || info->base_name == NULL ||
        ap_dialect_pid_cmd(dialect, pid, cmd, sizeof(cmd)) == 0)
    {
        return false;
    }

    cJSON *o = cJSON_CreateObject();

    if (o == NULL)
    {
        return false;
    }

    int shift = ap_dialect_data_shift(dialect);
    char hdr[AP_DIALECT_HDR_CMD_LEN];

    cJSON_AddNumberToObject(o, "pid", pid);
    cJSON_AddStringToObject(o, "cmd", cmd);
    cJSON_AddStringToObject(o, "name", info->base_name);

    /* a UDS-dialect row asks the ECU that owns the value, and only it:
       one answer, its flow control, no second unit with its own scaling */
    if (dialect == AP_DIALECT_UDS &&
        ap_dialect_header_cmd(owner, hdr, sizeof(hdr)) > 0)
    {
        cJSON_AddStringToObject(o, "init", hdr);
    }

    cJSON *params = cJSON_AddArrayToObject(o, "parameters");

    for (int i = 0; i < info->num_params; i++)
    {
        const std_parameter_t *prm = &info->params[i];
        char expr[AP_EXPR_LEN];

        /* the row's max makes a percent row's full scale decode to exactly
           max (`B2*100/255`, issue #1004); a row without a range (0, 0)
           gets the plain literal */
        bool ranged = !(prm->min == 0.0f && prm->max == 0.0f);

        if (prm->name == NULL ||
            ap_std_expression_bounded(prm->bit_start, prm->bit_length,
                                      (double)prm->scale,
                                      (double)prm->offset,
                                      ranged ? (double)prm->min : NAN,
                                      ranged ? (double)prm->max : NAN,
                                      shift, expr, sizeof(expr)) != ESP_OK)
        {
            continue;   /* placeholder row */
        }

        cJSON *pj = cJSON_CreateObject();

        if (pj == NULL)
        {
            break;
        }

        cJSON_AddStringToObject(pj, "name", prm->name);
        cJSON_AddStringToObject(pj, "expression", expr);
        cJSON_AddStringToObject(pj, "unit", prm->unit ? prm->unit : "");
        cJSON_AddStringToObject(pj, "class",
                                prm->class ? prm->class : "");

        if (!(prm->min == 0.0f && prm->max == 0.0f))
        {
            cJSON_AddNumberToObject(pj, "min", prm->min);
            cJSON_AddNumberToObject(pj, "max", prm->max);
        }

        cJSON_AddItemToArray(params, pj);
    }

    /* A PID the table only names (every parameter a placeholder: 13 "O2
       sensors present", 7A, 85 ...) decodes nothing. As a row it was polled
       all the same and each poll counted as failed (bench 2026-10-03: 5 of
       a van's 20 scanned rows). Not a row. */
    if (cJSON_GetArraySize(params) == 0)
    {
        cJSON_Delete(o);
        return false;
    }

    cJSON_AddItemToArray(arr, o);
    return true;
}

cJSON *ap_std_table_json(void)
{
    cJSON *arr = cJSON_CreateArray();

    if (arr == NULL)
    {
        return NULL;
    }

    /* the table in the current car's dialect (OBD-II when there is no
       car); a row added from here has no owner: it is asked functionally.
       A J1939 vehicle gets the built-in SPN rows as well (an EU truck
       answers both; a J1939-only one has no chip rows at all). */
    ap_dialect_t dialect = autopid_vehicle_dialect();

    if (ap_dialect_has_requests(dialect) || !autopid_vehicle_j1939())
    {
        if (!ap_dialect_has_requests(dialect))
        {
            dialect = AP_DIALECT_OBD2;
        }

        for (int pid = 1; pid < 256; pid++)
        {
            (void)ap_std_entry_to_json(arr, (uint8_t)pid, dialect,
                                       UINT32_MAX);
        }
    }

    if (autopid_vehicle_j1939())
    {
        (void)ap_j1939_std_table_json(arr);
    }

    return arr;
}

#endif /* !AUTOPID_HOST_TEST */
