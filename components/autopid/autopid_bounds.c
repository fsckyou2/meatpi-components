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
 * @file autopid_bounds.c
 * @brief PURE: a parameter's `min` / `max` applied to one decoded value
 *        (host-tested, host_test/main/test_bounds.c).
 *
 * Issue #1004 (2026-10-20, a VW T6.1): with the tank full the ECU answers
 * `41 2F FF`, the standard FuelTankLevel row decodes it as
 * `B2*0.3921568692` = 100.000001646, a hair above the row's max of 100,
 * and the poller dropped it: the dashboard went stale, MQTT kept the old
 * value. The SAE table's scale is a float printed with ten digits, so every
 * percent row overshoots its max at full scale by 1.6e-6, and the table's
 * ranges are approximate for a quarter of its rows (a fuel trim reaches
 * 99.22 against a max of 99). The legacy firmware clamped a standard PID
 * to the table's bounds and only dropped a custom PID outside bounds the
 * user set; v6 had turned both into a drop.
 *
 * The rule now: float noise never drops a reading (a value beyond a bound
 * by at most the tolerance is clamped to it, whatever the row); beyond
 * that a STANDARD OBD row clamps (the table's range is its display range,
 * legacy parity) and every other row drops (the bounds of a custom,
 * specific or J1939 row are a filter: a profile's SOC max 100 against a raw
 * 0xFF, a J1939 `FF..` not-available value).
 */
#include "autopid_private.h"

#include <math.h>

/** A bound's tolerance: relative to its magnitude, with a floor for the
 *  bounds around zero. 100 -> 1e-4, 1 -> 1e-6, 0 -> 1e-6. */
static double tolerance(double bound)
{
    double rel = fabs(bound) * AP_BOUNDS_TOL_REL;

    return (rel > AP_BOUNDS_TOL_ABS) ? rel : AP_BOUNDS_TOL_ABS;
}

ap_bounds_verdict_t ap_bounds_apply(double *value, float min, float max,
                                    bool clamp_beyond)
{
    if (value == NULL || isnan(*value))
    {
        return AP_BOUNDS_DROPPED;
    }

    if (!isnan(min) && *value < (double)min)
    {
        double lo = (double)min;

        if (clamp_beyond || *value >= lo - tolerance(lo))
        {
            *value = lo;
            return AP_BOUNDS_CLAMPED;
        }

        return AP_BOUNDS_DROPPED;
    }

    if (!isnan(max) && *value > (double)max)
    {
        double hi = (double)max;

        if (clamp_beyond || *value <= hi + tolerance(hi))
        {
            *value = hi;
            return AP_BOUNDS_CLAMPED;
        }

        return AP_BOUNDS_DROPPED;
    }

    return AP_BOUNDS_KEPT;
}
