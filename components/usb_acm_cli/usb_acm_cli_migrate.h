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
 * @file usb_acm_cli_migrate.h
 * @brief The stored usb_acm_cli document brought up to the current schema
 *        version. Pure (cJSON only), host-tested.
 */
#pragma once

#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The current schema version of the "usb_acm_cli" settings. */
#define USB_ACM_CLI_SETTINGS_VERSION 2

/**
 * Migrate @p settings from @p from_version to USB_ACM_CLI_SETTINGS_VERSION in
 * place. Version 1 had one switch, `enabled`, which was the ESPNetLink's
 * console: it becomes `console` (the stored value carried over, false when
 * the key was missing) and the old key is dropped. The `gps` switch is new
 * and takes its schema default (on). ESP_ERR_INVALID_ARG unless @p settings
 * is an object.
 */
esp_err_t usb_acm_cli_settings_migrate(uint32_t from_version, cJSON *settings);

#ifdef __cplusplus
}
#endif
