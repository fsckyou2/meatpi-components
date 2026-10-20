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
 * @file autopid_runner_private.h
 * @brief What the two halves of the chip-facing runner share (2026-10-20,
 *        700-line rule): autopid_runner.c (one poll, one test) and
 *        autopid_baseline.c (the baseline prelude, the chain sender, what
 *        the chains moved and its restore). Poller-task context, plus the
 *        httpd task for test-a-PID while the poller is paused: no lock,
 *        the callers are serialized by the chip job.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#include "autopid_private.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The chip-state memory: what the chip is set up for right now. */
typedef struct
{
    bool     baseline_sent;  /* the boot prelude went out                  */
    bool     hdr_borrowed;   /* a standard row's init set the request
                                header: the functional one is owed back    */
    bool     rxheader_set;   /* a row's ATCRA<rx> is in effect             */
    char     init_pin;       /* the CAN protocol a chain of the TABLES left
                                the chip on ('\0' = the baseline's): the bus
                                guard is asked for it again before every
                                request                                    */
    unsigned moved;          /* what the chains of the tables set on the
                                chip beyond the baseline since it last went
                                out (AP_SETS_* bits, ap_chain_sets): put
                                back before the next row type (issue #964) */
    bool     chain_cut;      /* a chain was stopped behind a reset: no
                                verdict for the protocol in effect         */
} ap_base_mem_t;

ap_base_mem_t *ap_base_mem(void);

/** One chip exchange, logged into the transcript when one is wanted. */
esp_err_t ap_base_request_tr(const char *cmd, char *resp, size_t resp_len,
                             TickType_t timeout, ap_tr_t *tr);

/** Send a ';'-separated chain, one command at a time; a reset inside it
 *  (`ATZ`, `ATD`, `ATWS`) gets the baseline prelude behind it, or cuts the
 *  chain (mem->chain_cut) when the bus guard has no verdict. */
void ap_base_send_chain(const char *chain, ap_tr_t *tr);

/** ap_base_send_chain for a chain OF THE TABLES (a type's init, a row's
 *  init, an AT command as a cmd): what it sets beyond the baseline is
 *  remembered (mem->moved) and put back before the next row type. */
void ap_base_send_init(const char *chain, ap_tr_t *tr);

/** The protocol the baseline prelude pins right now. */
char ap_base_proto(void);

/** The boot/restore prelude for that protocol, when the bus guard's verdict
 *  covers it. @return false = not sent, the baseline is due. */
bool ap_base_send_baseline(ap_tr_t *tr);

/** A standard row that sets its own request header borrows it. */
bool ap_base_row_borrows_header(int type, const char *init);

/** Put the functional header of the protocol in effect back. */
void ap_base_header_give_back(ap_tr_t *tr);

/** The chains of the tables moved the chip (mem->moved): the baseline back
 *  before the next row type. @return false = the bus guard has no verdict
 *  for the baseline's protocol right now: nothing sent. */
bool ap_base_restore(ap_tr_t *tr);

#ifdef __cplusplus
}
#endif
