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
 * @file config_backup.h
 * @brief The configuration backup that carries everything (issue #979):
 *        a downloadable file with every part of the device that may leave
 *        it (settings, vehicles and parameters, code databases and DBC
 *        files, scripts, the dashboard layout), the copies the device keeps
 *        for itself on the internal flash and the SD card (certificates
 *        included, sealed with a key the device made once), and the restore
 *        that shows what a backup holds before anything changes.
 *
 * Key material never leaves the device through the API: the file has no
 * certificates part, and the generic /api/fs routes refuse /data/certs,
 * /data/backups and /sd/backups (filesystem_api_hidden). Routes live under
 * /api/backup (HTTP_API.md of this component). Settings: {auto_copy, cli}.
 *
 * Dependencies (init order): log_manager, settings_manager, filesystem,
 * dev_status_manager before this; the parts are read by path, so autopid,
 * cert_manager and script_engine are not dependencies.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Log descriptor + settings descriptor. No side effects. */
esp_err_t config_backup_init(void);

/** The key (made once, NVS), the copies worker, the first sweep 20 s later. */
esp_err_t config_backup_start(void);

/** Stops the worker. */
esp_err_t config_backup_stop(void);

/** The /api/backup routes (main wires it, before the HTTP server starts). */
esp_err_t config_backup_register_http(void);

/**
 * Something the copies mirror may have changed: a settings submit, a file
 * upload or delete through /api/fs. The worker sweeps about 5 s later
 * (touches within that window collapse into one sweep). Never blocks.
 */
void config_backup_touch(void);

#ifdef __cplusplus
}
#endif
