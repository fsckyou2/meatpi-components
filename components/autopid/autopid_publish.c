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
 * @file autopid_publish.c
 * @brief The publish half of a poll: every enabled parameter of a row
 *        evaluated over ONE payload (mux precondition, expression,
 *        the bounds) into the cache and the events. Shared by the chip
 *        runner and the J1939 runner. Moved out of autopid_runner.c
 *        2026-10-05 (700-line rule). The bounds rule is
 *        autopid_bounds.c's (issue #1004, 2026-10-20).
 */
#include <math.h>

#include "esp_log.h"

#include "battery_monitor.h"
#include "expression_parser.h"

#include "autopid_private.h"

static const char *TAG = "autopid";

bool ap_runner_publish(const ap_pid_t *pid, const ap_param_t *params,
                       const uint8_t *payload, size_t payload_len,
                       int64_t ts_us)
{
    float volts = 0;

    (void)battery_monitor_voltage(&volts);

    int64_t now = ts_us;
    bool any = false;

    for (uint16_t i = 0; i < pid->param_count; i++)
    {
        const ap_param_t *prm = &params[i];

        if (!prm->enabled)
        {
            continue;
        }

        /* mux precondition (DBC m<N> signals): the parameter only
           applies when the switch slice equals its mux value */
        if (prm->mux_expr[0] != '\0')
        {
            double mv = 0;

            if (expression_parser_eval(prm->mux_expr, payload,
                                       payload_len, (double)volts,
                                       &mv) != ESP_OK ||
                fabs(mv - (double)prm->mux_val) > 0.5)
            {
                continue;
            }
        }

        double value = 0;

        if (expression_parser_eval(prm->expression, payload, payload_len,
                                   (double)volts, &value) != ESP_OK)
        {
            ESP_LOGD(TAG, "%s/%s: expression failed", pid->name,
                     prm->name);
            continue;
        }

        /* the bounds (autopid_bounds.c, issue #1004): float noise is
           clamped away for every row; beyond the tolerance a standard
           OBD row clamps to the table's range (legacy parity), a custom,
           specific or J1939 row drops the reading (its bounds filter:
           a profile's SOC max 100 against a raw 0xFF, J1939's FF.. not
           available) */
        bool clamps = (pid->type == AP_PID_STD) && !pid->j1939;

        if (ap_bounds_apply(&value, prm->min, prm->max, clamps) ==
            AP_BOUNDS_DROPPED)
        {
            ESP_LOGD(TAG, "%s/%s: %f out of range", pid->name, prm->name,
                     value);
            continue;
        }

        ap_cache_put(pid->param_start + i, value, now);
        ap_events_param(prm, pid->param_start + i, pid->group, value);
        any = true;
    }

    return any;
}
