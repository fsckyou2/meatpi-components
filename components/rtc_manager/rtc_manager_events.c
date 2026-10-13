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
 * @file rtc_manager_events.c
 * @brief event_manager glue: wall-clock pull values for rule templates
 *        (`${time.iso}` in MQTT payloads, main_events.c parity), the
 *        `${time.valid}` value and the `time.step {delta_ms, source}`
 *        source (2026-10-13: a rule can react to the clock being set or
 *        stepped; "log only while the clock is valid" is possible, not
 *        the default).
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "event_manager.h"

#include "rtc_manager.h"

#include "rtc_manager_private.h"

static esp_err_t read_time(const char *name, char *out, size_t len)
{
    if (strcmp(name, "time.iso") == 0)
    {
        return rtc_manager_now_iso8601(out, len);
    }

    if (strcmp(name, "time.epoch") == 0)
    {
        snprintf(out, len, "%lld", (long long)time(NULL));
        return ESP_OK;
    }

    if (strcmp(name, "time.valid") == 0)
    {
        snprintf(out, len, "%s", rtc_manager_time_valid() ? "true" : "false");
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

void rtcm_events_register(void)
{
    static const em_key_decl_t STEP_KEYS[] =
    {
        { "delta_ms", EM_VAL_F64 },
        { "source", EM_VAL_STR },
    };
    static const em_source_decl_t STEP =
    {
        .source = "time", .name = "step",
        .description = "the system clock was set: {delta_ms} is how far "
                       "it moved (the first set of a boot moves it from "
                       "1970), {source} rtc, sntp or manual",
        .keys = STEP_KEYS, .n_keys = 2,
    };

    (void)event_manager_declare_source(&STEP);
    (void)event_manager_register_value("time.iso", read_time);
    (void)event_manager_register_value("time.epoch", read_time);
    (void)event_manager_register_value("time.valid", read_time);
}

void rtcm_events_step(const rtc_clock_step_t *step)
{
    em_event_t ev = { 0 };

    snprintf(ev.source, sizeof(ev.source), "time");
    snprintf(ev.name, sizeof(ev.name), "step");
    ev.kv[0] = em_kv_f64("delta_ms", (double)step->delta_ms);
    ev.kv[1] = em_kv_str("source", rtc_manager_source_name(step->source));
    ev.n = 2;
    (void)event_manager_publish(&ev); /* before event_manager starts: dropped */
}
