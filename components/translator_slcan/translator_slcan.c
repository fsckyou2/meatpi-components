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
 * @file translator_slcan.c
 * @brief Registration (the one target-only edge; the codec lives in
 *        translator_slcan_codec.c and is pure/host-tested).
 */
#include "translator_slcan.h"

#include "esp_log.h"

#include "bridge_manager.h"
#include "can_manager.h"

#include "translator_slcan_private.h"

static const char *TAG = "translator_slcan";

extern const bridge_translator_t translator_slcan_desc;

/* an slcan client stated the bitrate (S<n> on open): taken by the node
   only when it has no verdict of its own, `auto` on a silent bus
   (2026-10-17); otherwise the command is absorbed as it always was */
static void slcan_state_bitrate(uint16_t kbps)
{
    if (can_manager_adopt_baud(kbps) == ESP_OK)
    {
        ESP_LOGI(TAG, "the client stated %u kbit/s: the bus was silent, "
                      "the node now runs there", (unsigned)kbps);
    }
}

esp_err_t translator_slcan_init(void)
{
    esp_err_t err = bridge_manager_register_translator(&translator_slcan_desc);

    if (err == ESP_OK)
    {
        slcan_bitrate_hook = slcan_state_bitrate;
        ESP_LOGI(TAG, "registered 'slcan' codec");
    }
    return err;
}
