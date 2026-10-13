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
 * @file rtc_manager_private.h
 * @brief Internal contracts: the PURE time codec and the PURE clock-step
 *        bookkeeping (both host-testable) and the RX8130 chip layer.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#include "rtc_manager.h"

/* ---- pure time codec (rtc_manager_time.c) --------------------------------- */

/** Raw RX8130 time registers 0x10..0x16 in read order. */
#define RTC_REGS_LEN 7 /* sec min hour week day month year */

/** BCD-decode + plausibility gate (2020..2099, valid fields). The weekday
 *  register is ignored (the epoch conversion does not need it). */
bool rtc_time_decode(const uint8_t regs[RTC_REGS_LEN], struct tm *out);

/** Encode broken-down UTC into the register image (weekday one-hot). */
void rtc_time_encode(const struct tm *in, uint8_t regs[RTC_REGS_LEN]);

/** Broken-down UTC to epoch seconds: a pure days-from-civil conversion,
 *  no libc, no TZ dependence (mktime would follow whatever TZ is set:
 *  the legacy firmware's whole problem, issue #984). Valid for 1970 on. */
int64_t rtc_time_to_epoch(const struct tm *utc);

/* ---- pure clock-step bookkeeping (rtc_manager_clock.c) ---------------------
   The component keeps offset = wall - mono (both in ms). The system clock
   moves only when somebody calls settimeofday, so a change of the offset
   IS the step, exact to the millisecond, whoever wrote the clock (lwIP's
   SNTP included). The last RTCM_STEPS_KEEP steps are kept for consumers
   that catch up by generation (data_logger re-bases queued records). */

#define RTCM_STEPS_KEEP 8

typedef struct
{
    int64_t          offset_ms;        /* wall - mono as last seen         */
    uint32_t         generation;       /* bumps at every note              */
    uint32_t         steps_this_boot;
    uint8_t          source;           /* the latest set's source          */
    bool             valid;
    rtc_clock_step_t steps[RTCM_STEPS_KEEP]; /* ring by steps_this_boot    */
} rtcm_clock_t;

/** Start the bookkeeping from the clocks as they are (a warm reset keeps
 *  the system clock: that carried time is the baseline, not a step). */
void rtcm_clock_init(rtcm_clock_t *c, int64_t wall_ms, int64_t mono_ms);

/** The clock was set (or seen set): measure the step from the new wall
 *  and mono readings and record it. @p out (optional) gets the step. */
void rtcm_clock_note(rtcm_clock_t *c, int64_t wall_ms, int64_t mono_ms,
                     uint8_t source, rtc_clock_step_t *out);

void rtcm_clock_state(const rtcm_clock_t *c, rtc_clock_state_t *out);

/** Steps with generation > @p since, oldest first, at most @p cap. */
int rtcm_clock_steps_since(const rtcm_clock_t *c, uint32_t since,
                           rtc_clock_step_t *out, int cap);

/* ---- RX8130 chip layer (rtc_manager_rx8130.c) ------------------------------- */

esp_err_t rtc_rx8130_init(void);     /* attach + control-register bring-up */
esp_err_t rtc_rx8130_read(uint8_t regs[RTC_REGS_LEN]);  /* coherent read  */
esp_err_t rtc_rx8130_write(const uint8_t regs[RTC_REGS_LEN]);

/* ---- settings (rtc_manager_settings.c) -------------------------------------- */

/** Boot-applied config (written only by the settings on_apply). */
typedef struct
{
    bool     enabled;
    bool     sntp;
    char     ntp_server[64];
    char     ntp_server2[64];   /* fallback; "" = none                   */
    uint32_t sync_interval_h;
} rtcm_config_t;

/** Register the "rtc_manager" descriptor with settings_manager. */
esp_err_t rtcm_settings_register(void);

const rtcm_config_t *rtcm_settings_config(void);
bool rtcm_settings_is_configured(void); /* boot apply ran (standard 4.3) */

/* event_manager glue (rtc_manager_events.c) */
void rtcm_events_register(void);
void rtcm_events_step(const rtc_clock_step_t *step);  /* publish time.step */
