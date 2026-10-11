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
 * @file usb_acm_cli_settings.c
 * @brief settings_manager descriptor for usb_acm_cli: field-table schema
 *        (source of truth for shape/defaults), the v1 -> v2 migration
 *        (usb_acm_cli_migrate.c, pure) and on_apply (stores the two
 *        boot-applied switches; registers the CLI, settings-gated per
 *        standard section 6b).
 *
 * Version 2 (2026-10-11, issue #978): `gps` (read a USB GPS receiver on the
 * connector, default on) and `console` (talk to the ESPNetLink's console
 * over USB, default off; v1's `enabled`). A receiver is read whether the
 * console is on or off.
 */
#include "settings_manager.h"

#include "usb_acm_cli_migrate.h"
#include "usb_acm_cli_private.h"

static const settings_field_t FIELDS[] =
{
    SETTINGS_BOOL("gps", true),      /* read a USB GPS receiver (NMEA)         */
    SETTINGS_BOOL("console", false), /* the ESPNetLink's console over the cable */
    SETTINGS_BOOL("cli", true),
};

/* boot-applied settings */
static bool s_configured;
static bool s_gps;
static bool s_console;

bool usb_acm_cli_settings_gps(void)
{
    return s_gps;
}

bool usb_acm_cli_settings_console(void)
{
    return s_console;
}

bool usb_acm_cli_settings_is_configured(void)
{
    return s_configured;
}

static esp_err_t on_apply(const cJSON *settings)
{
    const cJSON *gps = cJSON_GetObjectItemCaseSensitive(settings, "gps");

    /* the schema fills a missing key with its default; a document without
     * it (never, after validation) still reads as the default: on */
    s_gps = (gps == NULL) ? true : cJSON_IsTrue(gps);
    s_console = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(settings, "console"));

    if (!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(settings, "cli")))
    {
        static bool reg;

        if (!reg && usb_acm_cli_register_cli() == ESP_OK)
        {
            reg = true;
        }
    }

    s_configured = true;
    return ESP_OK;
}

esp_err_t usb_acm_cli_settings_register(void)
{
    static const settings_descriptor_t DESC =
    {
        .name        = "usb_acm_cli",
        .version     = USB_ACM_CLI_SETTINGS_VERSION,
        .fields      = FIELDS,
        .field_count = sizeof(FIELDS) / sizeof(FIELDS[0]),
        .on_apply    = on_apply,
        .on_migrate  = usb_acm_cli_settings_migrate,
    };

    return settings_manager_register(&DESC);
}
