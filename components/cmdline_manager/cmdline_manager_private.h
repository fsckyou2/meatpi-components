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
 * @file cmdline_manager_private.h
 * @brief Internal contracts: the PURE pieces (host-tested: the line
 *        assembler and the dispatcher's verdict line), the console hook
 *        and the settings accessors.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* ---- pure line assembler (cmdline_manager_line.c) --------------------------
 * Bytes in (any chunking) -> complete lines out. \r and \n both
 * terminate; empty lines are skipped; CRLF never yields a bogus empty
 * line; an overlong line is discarded (flagged) up to its terminator. */

typedef struct
{
    char   buf[256];
    size_t len;
    bool   overflow;
} cm_line_asm_t;

void cm_line_reset(cm_line_asm_t *a);

/**
 * Consume input until a complete line or the end of input.
 * @return bytes consumed from @p data. *out_line is set to the
 * NUL-terminated line when one completed (else NULL). Call repeatedly
 * while it consumes less than @p len.
 */
size_t cm_line_feed(cm_line_asm_t *a, const uint8_t *data, size_t len,
                    const char **out_line);

/* ---- pure dispatcher verdict (cmdline_manager_line.c) -----------------------
 * What the dispatcher owes the client after esp_console_run(), printed
 * before the prompt on every transport. Silence = the command ran and
 * returned 0 (its own output is the answer), or the line was empty. */

/** Holds every verdict: "Unknown command: " + a 255-char line + the
 *  "Try: help" line + NUL is 284 bytes. */
#define CM_VERDICT_BUF 304

/**
 * Compose the verdict line for one command outcome.
 * @param err     esp_console_run()'s return code
 * @param ret     the handler's return value (meaningful with ESP_OK only)
 * @param line    the command line (echoed by the unknown-command verdict)
 * @param out     receives the NUL-terminated verdict; "" when none is owed
 * @param out_len size of @p out (a smaller buffer truncates safely)
 * @return bytes written, 0 when nothing is owed
 */
size_t cm_exec_verdict(esp_err_t err, int ret, const char *line,
                       char *out, size_t out_len);

/* ---- shared internals -------------------------------------------------------- */

#define CM_PROMPT "wican> " /* the legacy prompt */

/** Start the UART0 linenoise console task (cmdline_manager_console.c).
 *  Called from cmdline_manager_start() when the `uart` setting is on. */
void cm_console_start(void);

/* ---- settings (cmdline_manager_settings.c) ----------------------------------- */

/** Register the "cmdline_manager" descriptor with settings_manager. */
esp_err_t cm_settings_register(void);

bool cm_settings_enabled(void);       /* the dispatcher/console run at all  */
bool cm_settings_uart(void);          /* the UART0 linenoise console        */
bool cm_settings_is_configured(void); /* boot apply ran (standard §4.3)     */
