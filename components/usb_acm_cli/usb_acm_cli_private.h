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

/** Internals shared across the usb_acm_cli .c files. */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "usb_acm_gps.h"

esp_err_t usb_acm_cli_register_cli(void);

/* ---- settings (usb_acm_cli_settings.c) --------------------------------------- */

/** Register the "usb_acm_cli" descriptor with settings_manager. */
esp_err_t usb_acm_cli_settings_register(void);

bool usb_acm_cli_settings_gps(void);           /* read a USB GPS receiver (default on)  */
bool usb_acm_cli_settings_console(void);       /* talk to the ESPNetLink's console       */
bool usb_acm_cli_settings_is_configured(void); /* boot apply ran (std section 4.3)       */

/* ---- the fix cache and the dongle's poll task (usb_acm_cli_poll.c) ------------ */

/** Create the cache lock (init-time, before any task runs). */
esp_err_t usb_acm_cli_poll_init(void);

/** Start the ESPNetLink console poll task (`gps -p -j` every 5 s). Idempotent. */
esp_err_t usb_acm_cli_poll_start(void);

/** Idle the poll task (it never exits: a stop/start cycle cannot race it). */
void usb_acm_cli_poll_stop(void);

/** The one writer of the fix cache: NULL = no live fix. Any task context. */
void usb_acm_cli_gps_cache_set(const usb_acm_gps_t *fix);

/** Hand a fresh fix to the sink main wired (the autopid publisher), if any. */
void usb_acm_cli_gps_publish(const usb_acm_gps_t *fix);
