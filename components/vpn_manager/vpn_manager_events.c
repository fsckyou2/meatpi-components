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
 * @file vpn_manager_events.c
 * @brief event_manager glue: the `vpn.state {connected}` source,
 *        rules can alert on tunnel up/down or nudge consumers that
 *        live behind the tunnel. Publishes come from the state task
 *        (queue-safe).
 */
#include <stdio.h>

#include "event_manager.h"

#include "vpn_manager_private.h"

void vpn_events_register(void)
{
    static const em_key_decl_t KEYS[] =
    {
        { "connected", EM_VAL_STR },
        { "paused", EM_VAL_STR },
        { "network", EM_VAL_STR },
    };
    static const em_source_decl_t STATE =
    {
        .source = "vpn", .name = "state",
        .description = "VPN tunnel state changed; {connected} is \"true\" "
                       "after a verified handshake, \"false\" on teardown; "
                       "{paused} is \"true\" while the station is on a "
                       "network marked Pause VPN; {network} names the "
                       "station's network",
        .keys = KEYS, .n_keys = 3,
    };

    (void)event_manager_declare_source(&STATE);
}

void vpn_events_state(bool connected, bool paused, const char *network)
{
    em_event_t ev = { 0 };

    snprintf(ev.source, sizeof(ev.source), "vpn");
    snprintf(ev.name, sizeof(ev.name), "state");
    ev.kv[0] = em_kv_str("connected", connected ? "true" : "false");
    ev.kv[1] = em_kv_str("paused", paused ? "true" : "false");
    ev.kv[2] = em_kv_str("network", network != NULL ? network : "");
    ev.n = 3;
    (void)event_manager_publish(&ev);
}
