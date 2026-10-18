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
 * @file web_ui_v2.c
 * @brief Registers the embedded, gzipped v2 single-page app as an asset
 *        table. Hash-routed SPA, so the browser only ever requests "/" (and
 *        "/index.html"); both map to the one blob. API/WS routes always win
 *        because the catch-all is installed last (http_server_manager).
 */
#include "web_ui_v2.h"

#include "sdkconfig.h"

#if CONFIG_WICAN_WEBUI_V2

#include "esp_log.h"
#include "http_server_manager.h"

static const char *TAG = "web_ui_v2";

extern const uint8_t index_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_gz_end[]   asm("_binary_index_html_gz_end");
/* the Scripts page chunk (web/scripts.js), fetched on first use */
extern const uint8_t scripts_gz_start[] asm("_binary_scripts_js_gz_start");
extern const uint8_t scripts_gz_end[]   asm("_binary_scripts_js_gz_end");
/* the File Manager chunk (web/files.js), same pattern (2026-09-07) */
extern const uint8_t files_gz_start[] asm("_binary_files_js_gz_start");
extern const uint8_t files_gz_end[]   asm("_binary_files_js_gz_end");

extern const uint8_t monitor_gz_start[] asm("_binary_monitor_js_gz_start");
extern const uint8_t monitor_gz_end[]   asm("_binary_monitor_js_gz_end");
/* the Quick Setup wizard chunk (web/setup.js, 2026-10-01) */
extern const uint8_t setup_gz_start[] asm("_binary_setup_js_gz_start");
extern const uint8_t setup_gz_end[]   asm("_binary_setup_js_gz_end");
/* the configuration backup cards chunk (web/backup.js, 2026-10-11) */
extern const uint8_t backup_gz_start[] asm("_binary_backup_js_gz_start");
extern const uint8_t backup_gz_end[]   asm("_binary_backup_js_gz_end");
/* the firmware updates card chunk (web/fwupdate.js, 2026-10-18) */
extern const uint8_t fwu_gz_start[] asm("_binary_fwupdate_js_gz_start");
extern const uint8_t fwu_gz_end[]   asm("_binary_fwupdate_js_gz_end");

static const http_asset_t ASSETS[] =
{
    { .uri = "/", .content_type = "text/html",
      .data_start = index_gz_start, .data_end = index_gz_end,
      .content_encoding = "gzip" },
    { .uri = "/index.html", .content_type = "text/html",
      .data_start = index_gz_start, .data_end = index_gz_end,
      .content_encoding = "gzip" },
    /* On-demand page chunks (2026-09-07): heavier, rarely opened pages ship
     * as their own gzipped blobs the app loads the first time they open,
     * so the main page stays small. */
    { .uri = "/ui/scripts.js", .content_type = "application/javascript",
      .data_start = scripts_gz_start, .data_end = scripts_gz_end,
      .content_encoding = "gzip" },
    { .uri = "/ui/files.js", .content_type = "application/javascript",
      .data_start = files_gz_start, .data_end = files_gz_end,
      .content_encoding = "gzip" },
    { .uri = "/ui/monitor.js", .content_type = "application/javascript",
      .data_start = monitor_gz_start, .data_end = monitor_gz_end,
      .content_encoding = "gzip" },
    { .uri = "/ui/setup.js", .content_type = "application/javascript",
      .data_start = setup_gz_start, .data_end = setup_gz_end,
      .content_encoding = "gzip" },
    { .uri = "/ui/backup.js", .content_type = "application/javascript",
      .data_start = backup_gz_start, .data_end = backup_gz_end,
      .content_encoding = "gzip" },
    { .uri = "/ui/fwupdate.js", .content_type = "application/javascript",
      .data_start = fwu_gz_start, .data_end = fwu_gz_end,
      .content_encoding = "gzip" },
    /* Optional UI extras the page installs on demand (2026-09-06): the
     * dashboard's chart library (uPlot, ~50 KB) is uploaded through
     * /api/fs/upload into <store>/cache/www/ and served from here with the
     * catch-all's MIME inference + ETag caching. Internal flash first; the
     * SD card is the fallback when flash is short. Nothing is embedded in
     * the firmware for it, and anything under cache/ is re-creatable. */
    { .uri = "/cache/*",   .fs_path = "/data/cache" },
    { .uri = "/sdcache/*", .fs_path = "/sd/cache" },
    { 0 }
};

esp_err_t web_ui_v2_register(void)
{
    esp_err_t err = http_server_manager_register_assets(ASSETS);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "v2 web UI registered (%u bytes gzipped + %u + %u + %u + %u + %u chunks)",
                 (unsigned)(index_gz_end - index_gz_start),
                 (unsigned)(scripts_gz_end - scripts_gz_start),
                 (unsigned)(files_gz_end - files_gz_start),
                 (unsigned)(monitor_gz_end - monitor_gz_start),
                 (unsigned)(setup_gz_end - setup_gz_start),
                 (unsigned)(backup_gz_end - backup_gz_start));
    }
    return err;
}

#else /* !CONFIG_WICAN_WEBUI_V2 */

esp_err_t web_ui_v2_register(void)
{
    return ESP_OK; /* another UI (or none) is selected */
}

#endif
