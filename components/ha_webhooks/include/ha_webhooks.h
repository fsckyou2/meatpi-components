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
 * @file ha_webhooks.h
 * @brief Home Assistant integration (v6 port of the legacy ha_webhooks +
 *        autopid webhook poster). Owns the HA telemetry link:
 *
 *   - inbound  `/api/webhook` GET/POST/DELETE: the HA HACS integration
 *              auto-registers its webhook URL here (discovery push);
 *   - outbound a PSRAM-stack poster task that, every `interval_s`, builds
 *              `{status, autopid_data, config, gps}` (ha_webhooks_payload.c;
 *              `status` carries the identity keys, the station address
 *              `sta_ip` since 2026-10-11 for issue #977, battery, VPN) and
 *              POSTs it to the webhook URL(s) with failover, while enabled
 *              and the network is up (autopid may be off: a status-only
 *              push is valid).
 *
 * Feature component: consumes http_server_manager, http_client_manager,
 * cert_manager, autopid (snapshot + cached config), dev_status_manager.
 * Config via settings_manager (`"ha_webhooks"`); the URL push applies
 * LIVE (documented reboot-to-apply exception, see TASK_ha_webhooks.md §7).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Register the settings + log descriptors. No hardware, no network. */
esp_err_t ha_webhooks_init(void);

/** Start the poster task if enabled (else log + ESP_OK). */
esp_err_t ha_webhooks_start(void);

/** Stop the poster task and park. */
esp_err_t ha_webhooks_stop(void);

/** Register the `/api/webhook` routes (HTTP compositions only, §9.1). */
esp_err_t ha_webhooks_register_http(void);

/** Register the `webhook` console command (gated by the `cli` setting;
 *  called internally from on_apply: main wires nothing, §6b). */
esp_err_t ha_webhooks_register_cli(void);

/**
 * The firmware update keys of the push's status section (2026-10-18):
 * `fw_update_available` (always sent in diff mode) and `fw_latest_version`
 * ("" when unknown or the updater is off). The composition root provides
 * them from fw_update; without a provider the keys are absent. Runs on
 * the poster task (a PSRAM stack): no flash, RAM only.
 */
typedef bool (*ha_webhooks_fw_provider_t)(bool *available, char *latest,
                                          size_t cap);
esp_err_t ha_webhooks_set_fw_provider(ha_webhooks_fw_provider_t fn);

#ifdef __cplusplus
}
#endif
