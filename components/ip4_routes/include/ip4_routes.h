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
 * @file ip4_routes.h
 * @brief Destination routes for lwIP (issue #975, the split tunnel,
 *        2026-10-11): a small table of {network, mask} -> netif that lwIP's
 *        IPv4 route lookup consults before its own netif walk, so a VPN
 *        netif can carry a LIST of remote networks instead of the one its
 *        own netmask allows.
 *
 * The hook: IDF's lwIP port defines `LWIP_HOOK_IP4_ROUTE_SRC` as its own
 * `ip4_route_src_hook` (source-address routing; a strong symbol with no
 * Kconfig choice). This component wraps that symbol at link time
 * (`-Wl,--wrap=ip4_route_src_hook`, the restart_tracker / can_manager
 * pattern): lwIP's calls land in `__wrap_ip4_route_src_hook`, which
 * matches the destination against the table (longest prefix wins, the
 * netif must be up with its link up) and otherwise falls through to IDF's
 * hook. lwIP calls the hook from `ip4_route()` and `ip4_route_src()`
 * alike, so every outbound packet passes here: the table is tiny and read
 * under a spinlock.
 *
 * Owners: a component that brings a netif up with remote networks behind
 * it (vpn_manager's WireGuard netif today) sets its routes once the netif
 * exists and clears them BEFORE the netif is removed. Entries are host
 * order with the network pre-masked. The table is bounded
 * (IP4_ROUTES_MAX; occupancy on the WICAN CAPS line, standard section 12).
 *
 * The parsing and matching are pure (ip4_routes_core.c, host-tested) and
 * shared with the settings validators that take route lists.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IP4_ROUTES_MAX 16 /* every owner's routes together */

typedef struct
{
    uint32_t network; /**< host order, host bits cleared  */
    uint32_t mask;    /**< host order, contiguous         */
} ip4_routes_entry_t;

/** Register the log descriptor. No side effects. */
esp_err_t ip4_routes_init(void);

/** Replace every route of @p netif (an lwIP `struct netif *`) with
 *  @p entries. ESP_ERR_NO_MEM (and the table untouched, one E line) when
 *  the other owners' routes plus @p n exceed IP4_ROUTES_MAX; n == 0 clears. */
esp_err_t ip4_routes_set(void *netif, const ip4_routes_entry_t *entries,
                         size_t n);

/** Drop every route of @p netif. Call before the netif is removed. */
void ip4_routes_clear(void *netif);

/** Occupancy for the WICAN CAPS line. */
void ip4_routes_capacity(size_t *used, size_t *cap);

/* ---- pure (ip4_routes_core.c, host-tested) -------------------------------- */

/** "a.b.c.d/n" (n 0..32; a bare address is /32; spaces around it are
 *  fine). Host bits beyond the prefix are cleared. */
bool ip4_routes_parse_cidr(const char *text, ip4_routes_entry_t *out);

/** A comma or space separated list of CIDRs into @p out (at most @p max).
 *  Returns the count, or -1 with @p err naming the offending token or
 *  "more than N routes". An empty list is 0. */
int ip4_routes_parse_list(const char *list, ip4_routes_entry_t *out,
                          size_t max, char *err, size_t err_len);

/** "a.b.c.d/n" of an entry. */
void ip4_routes_format(const ip4_routes_entry_t *e, char *out, size_t len);

#ifdef __cplusplus
}
#endif
