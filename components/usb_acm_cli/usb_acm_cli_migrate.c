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
 * @file usb_acm_cli_migrate.c
 * @brief Settings migration for usb_acm_cli (pure: cJSON only, no IDF glue).
 *
 * Version 1 (2026-07) had one switch, `enabled`, written for the ESPNetLink
 * dongle's console and off by default. From 2026-10-07 a plain USB GNSS
 * receiver was parsed by the same reader behind the same switch, which is
 * why a receiver plugged in after setup was never read (issue #978).
 * Version 2 (2026-10-11) names the switches for what they do: `gps` (read a
 * USB GPS receiver, default on, new) and `console` (talk to the dongle's
 * console, default off, the old `enabled`). The rename matters beyond
 * words: the web UI's generic settings form shows only a component's
 * `enabled` switch while that key is false, which would have hidden the GPS
 * switch exactly where a user looks.
 */
#include "usb_acm_cli_migrate.h"

#include <stdbool.h>

esp_err_t usb_acm_cli_settings_migrate(uint32_t from_version, cJSON *settings)
{
    if (settings == NULL || !cJSON_IsObject(settings))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (from_version < 2)
    {
        const cJSON *old = cJSON_GetObjectItemCaseSensitive(settings, "enabled");
        bool console = cJSON_IsTrue(old);

        cJSON_DeleteItemFromObject(settings, "console");
        cJSON_AddBoolToObject(settings, "console", console);
        cJSON_DeleteItemFromObject(settings, "enabled");
        /* `gps` is not written here on purpose: the schema fills its
         * default (on) at validation, like any added key */
    }

    return ESP_OK;
}
