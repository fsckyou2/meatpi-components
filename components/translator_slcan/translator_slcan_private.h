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
 * @file translator_slcan_private.h
 * @brief The one impure edge of the slcan codec (2026-10-17): an slcan
 *        client states the bus bitrate with `S<n>` on open (python-can
 *        writes S6 for 500 kbit/s before O). The codec stays pure and
 *        host-tested; translator_slcan.c points the hook at
 *        can_manager_adopt_baud() on the target, the host test at a stub.
 *        NULL = the command is absorbed as before.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern void (*slcan_bitrate_hook)(uint16_t kbps);

/** Lawicel `S` code to kbit/s: S0 10 ... S6 500, S8 1000 (python-can's
 *  S9 = 83.3). 0 for a code this codec does not know. */
uint16_t slcan_bitrate_kbps(char code);

#ifdef __cplusplus
}
#endif
