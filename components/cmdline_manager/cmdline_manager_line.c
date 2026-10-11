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
 * @file cmdline_manager_line.c
 * @brief The dispatcher's PURE pieces, no RTOS, host-tested: the
 *        byte-stream -> line assembler, and the verdict line a client
 *        is owed after a command.
 */
#include <stdio.h>
#include <string.h>

#include "cmdline_manager_private.h"

void cm_line_reset(cm_line_asm_t *a)
{
    a->len = 0;
    a->overflow = false;
}

size_t cm_line_feed(cm_line_asm_t *a, const uint8_t *data, size_t len,
                    const char **out_line)
{
    *out_line = NULL;

    for (size_t i = 0; i < len; i++)
    {
        char c = (char)data[i];

        if (c == '\n' || c == '\r')
        {
            bool had_overflow = a->overflow;
            size_t line_len = a->len;

            a->buf[line_len] = '\0';
            a->len = 0;
            a->overflow = false;

            if (line_len == 0 || had_overflow)
            {
                continue; /* empty (or the CRLF tail) / oversized: skip */
            }

            *out_line = a->buf;
            return i + 1;
        }

        if (a->len < sizeof(a->buf) - 1)
        {
            a->buf[a->len++] = c;
        }
        else
        {
            a->overflow = true; /* discard up to the terminator */
        }
    }

    return len;
}

/* ---- the dispatcher's verdict ------------------------------------------------- */

size_t cm_exec_verdict(esp_err_t err, int ret, const char *line,
                       char *out, size_t out_len)
{
    int n = 0;

    if (out == NULL || out_len == 0)
    {
        return 0;
    }

    out[0] = '\0';

    if (err == ESP_ERR_NOT_FOUND)
    {
        n = snprintf(out, out_len, "Unknown command: %s\nTry: help\n",
                     line != NULL ? line : "");
    }
    else if (err == ESP_ERR_INVALID_ARG)
    {
        /* an empty or whitespace-only line: nothing ran, nothing owed */
    }
    else if (err == ESP_OK && ret != 0)
    {
        n = snprintf(out, out_len, "command failed (%d)\n", ret);
    }
    else if (err != ESP_OK)
    {
        /* the console itself failed (no memory for the argument vector,
         * not initialised): the command never ran. Until 2026-10-11 this
         * printed nothing and looked like success (issue #980). */
        n = snprintf(out, out_len,
                     "Internal error: %s (0x%x), the command did not run\n",
                     esp_err_to_name(err), (unsigned)err);
    }

    if (n <= 0)
    {
        out[0] = '\0';
        return 0;
    }

    return (size_t)n < out_len ? (size_t)n : out_len - 1;
}
