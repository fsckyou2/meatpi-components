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
 * @file config_backup.c
 * @brief Lifecycle (init: descriptors; start: the key, the copies worker),
 *        the touch, the clock helpers, the identity, the planned restart
 *        after a restore.
 */
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dev_status_manager.h"
#include "log_manager.h"
#include "restart_tracker.h"

#include "config_backup.h"
#include "config_backup_private.h"

static const char *TAG = "config_backup";

static bool s_started;

/* ---- the clock and the identity ------------------------------------------------------ */

int64_t cb_now_epoch(void)
{
    if (!dev_status_manager_is_set(DEV_STATUS_BIT_TIME_SYNCED))
    {
        return 0;
    }

    return (int64_t)time(NULL);
}

void cb_now_iso(char out[CB_ISO_LEN])
{
    cb_iso_from_epoch(cb_now_epoch(), out);
}

const char *cb_device_id(void)
{
    const char *id = dev_status_manager_device_id();

    return (id != NULL) ? id : "unknown";
}

const char *cb_fw_version(void)
{
    const char *v = dev_status_manager_app_version();

    return (v != NULL) ? v : "";
}

/* ---- the restart after a restore ---------------------------------------------------- */

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1000)); /* let the response flush (§4.2) */
    restart_tracker_restart(RESTART_TRACKER_PLANNED_REASON_CONFIG_APPLY,
                            RESTART_TRACKER_SOURCE_WEB_UI, 0);
}

void cb_schedule_restart(void)
{
    static bool s_scheduled;

    if (s_scheduled)
    {
        return;
    }

    s_scheduled = true;

    if (xTaskCreate(restart_task, "cb_reboot", 3072, NULL, 5, NULL) != pdPASS)
    {
        restart_task(NULL); /* inline: the response is lost, the restore is not */
    }
}

/* ---- lifecycle --------------------------------------------------------------------------- */

esp_err_t config_backup_init(void)
{
    static const log_descriptor_t LOG_DESC = { "config_backup", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);
    return cb_settings_register();
}

esp_err_t config_backup_start(void)
{
    if (s_started)
    {
        return ESP_OK;
    }

    esp_err_t err = cb_crypto_init();

    if (err != ESP_OK)
    {
        return err; /* no key: no sealing, no copies; the routes say so */
    }

    err = cb_copies_start();

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "copies worker not started: %s", esp_err_to_name(err));
        return err;
    }

    s_started = true;
    ESP_LOGI(TAG, "started (copies %s)",
             cb_settings_auto_copy() ? "kept up to date" : "manual");
    return ESP_OK;
}

esp_err_t config_backup_stop(void)
{
    cb_copies_stop();
    cb_restore_drop();
    s_started = false;
    return ESP_OK;
}

void config_backup_touch(void)
{
    if (s_started)
    {
        cb_copies_touch();
    }
}
