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
 * @file rtc_manager_clock.c
 * @brief PURE clock-step bookkeeping (issue #984, 2026-10-13). The
 *        component samples wall - mono at init and at every set of the
 *        system clock; the change of that offset is the step. No RTOS, no
 *        clocks read here: host-testable.
 */
#include <string.h>

#include "rtc_manager_private.h"

void rtcm_clock_init(rtcm_clock_t *c, int64_t wall_ms, int64_t mono_ms)
{
    memset(c, 0, sizeof(*c));
    c->offset_ms = wall_ms - mono_ms;
    c->source = RTC_CLOCK_SRC_NONE;
}

void rtcm_clock_note(rtcm_clock_t *c, int64_t wall_ms, int64_t mono_ms,
                     uint8_t source, rtc_clock_step_t *out)
{
    int64_t offset = wall_ms - mono_ms;
    rtc_clock_step_t *s = &c->steps[c->steps_this_boot % RTCM_STEPS_KEEP];

    c->generation++;
    c->steps_this_boot++;
    s->generation = c->generation;
    s->mono_ms = (uint32_t)mono_ms;
    s->delta_ms = offset - c->offset_ms;
    s->wall_ms = wall_ms;
    s->source = source;
    c->offset_ms = offset;
    c->source = source;
    c->valid = true;

    if (out != NULL)
    {
        *out = *s;
    }
}

void rtcm_clock_state(const rtcm_clock_t *c, rtc_clock_state_t *out)
{
    memset(out, 0, sizeof(*out));
    out->valid = c->valid;
    out->source = c->source;
    out->generation = c->generation;
    out->steps_this_boot = c->steps_this_boot;

    if (c->steps_this_boot > 0)
    {
        out->last = c->steps[(c->steps_this_boot - 1) % RTCM_STEPS_KEEP];
    }
}

int rtcm_clock_steps_since(const rtcm_clock_t *c, uint32_t since,
                           rtc_clock_step_t *out, int cap)
{
    uint32_t kept = (c->steps_this_boot < RTCM_STEPS_KEEP)
                        ? c->steps_this_boot : RTCM_STEPS_KEEP;
    int n = 0;

    for (uint32_t i = 0; i < kept && n < cap; i++)
    {
        const rtc_clock_step_t *s =
            &c->steps[(c->steps_this_boot - kept + i) % RTCM_STEPS_KEEP];

        if (s->generation > since)
        {
            out[n++] = *s;
        }
    }

    return n;
}
