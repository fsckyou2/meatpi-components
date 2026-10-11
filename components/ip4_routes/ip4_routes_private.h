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
 * @file ip4_routes_private.h
 * @brief The route table's pure operations (ip4_routes_core.c): the lwIP
 *        glue keeps the table and the lock, the host tests drive these
 *        directly.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ip4_routes.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    void              *owner; /* the netif the route points into */
    ip4_routes_entry_t e;
} ip4_routes_slot_t;

/** Replace @p owner's routes in @p table (@p count used of @p cap) with
 *  @p entries. Returns the new count, or -1 when the other owners' routes
 *  plus @p n would not fit (the table is then untouched). */
int ip4_routes_core_set(ip4_routes_slot_t *table, size_t count, size_t cap,
                        void *owner, const ip4_routes_entry_t *entries,
                        size_t n);

/** Drop @p owner's routes; returns the new count. */
size_t ip4_routes_core_clear(ip4_routes_slot_t *table, size_t count,
                             void *owner);

/** The owner of the longest prefix that matches @p dest (host order), or
 *  NULL. Among equal prefixes the first entry wins. */
void *ip4_routes_core_match(const ip4_routes_slot_t *table, size_t count,
                            uint32_t dest);

#ifdef __cplusplus
}
#endif
