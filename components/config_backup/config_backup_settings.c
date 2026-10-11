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
 * @file config_backup_settings.c
 * @brief The `config_backup` settings component: {auto_copy, cli}, v1.
 *        auto_copy (default true) keeps the copies on the device up to date
 *        after every change; off, they are refreshed by hand only.
 */
#include "esp_log.h"

#include "settings_manager.h"

#include "config_backup_private.h"

static const char *TAG = "config_backup";

static const settings_field_t FIELDS[] =
{
    SETTINGS_BOOL("auto_copy", true),
    SETTINGS_BOOL("cli", true),
};

static bool s_auto_copy = true;

bool cb_settings_auto_copy(void)
{
    return s_auto_copy;
}

static esp_err_t on_apply(const cJSON *settings)
{
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(settings, "auto_copy");

    s_auto_copy = !cJSON_IsFalse(a);

    if (!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(settings, "cli")))
    {
        static bool s_cli_registered;

        if (!s_cli_registered && config_backup_register_cli() == ESP_OK)
        {
            s_cli_registered = true;
        }
    }

    ESP_LOGD(TAG, "settings applied: auto_copy=%d", (int)s_auto_copy);
    return ESP_OK;
}

esp_err_t cb_settings_register(void)
{
    static const settings_descriptor_t DESC =
    {
        .name = "config_backup",
        .version = 1,
        .fields = FIELDS,
        .field_count = sizeof(FIELDS) / sizeof(FIELDS[0]),
        .on_apply = on_apply,
    };

    return settings_manager_register(&DESC);
}
