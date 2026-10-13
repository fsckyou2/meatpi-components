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
 * @file espnetlink_link_settings.c
 * @brief settings_manager descriptor "espnetlink" (standard §4.1), v2.
 *
 * v1 (2026-08-22, dev only): enabled(false) ssid host gps_poll_s
 * health_poll_s cli, manual pairing only.
 * v2: enabled defaults TRUE (the dongle is plug-and-play), + mode,
 * auto_pair, device_id, cut_retries. The migration (espnetlink_link_
 * migrate.c, host-tested) flips a v1 `enabled` that was merely the old
 * default (no ssid ever paired) to the new one.
 */
#include <string.h>

#include "esp_err.h"

#include "cJSON.h"
#include "settings_manager.h"

#include "espnetlink_link.h"
#include "espnetlink_link_core.h"
#include "espnetlink_link_migrate.h"
#include "espnetlink_link_private.h"

static const settings_field_t FIELDS[] =
{
    /* master switch: the link task only runs when true */
    SETTINGS_BOOL("enabled", true),
    /* usb_rndis (the default since 2026-10-12, Ali: the dongle is set up
       as a USB Ethernet modem by default; a fresh dongle ships rndis +
       LTE sharing, so the first contact needs no dongle restart) /
       usb_ncm: the dongle stays a USB-Ethernet adapter presenting that
       class (the pairing pass sets the dongle's usb_dev_ethernet.class
       to match); wifi_modem: the dongle's USB data is cut after every
       pairing pass (nothing persists on the dongle: it boots with USB on
       and the WiCAN re-pairs at every dongle boot), the dongle is reached
       over its AP, its GPS is undisturbed. A stored value persists across
       updates: only a fresh device takes the new default. */
    SETTINGS_STR_ENUM("mode", "wifi_modem,usb_ncm,usb_rndis", "usb_rndis"),
    /* zero-touch: read the AP key over USB on every enumeration and
       (wifi_modem) cut the data lines; false = manual pair only */
    SETTINGS_BOOL("auto_pair", true),
    /* the ESPNetLink's AP SSID (what "we are on the dongle" means);
       joining it is wifi_manager's job, see espnetlink_link_pair() */
    SETTINGS_STR ("ssid", 32, ""),
    /* the paired dongle's device_id (12 hex); informational + "changed?" */
    SETTINGS_STR ("device_id", 12, ""),
    /* dongle address override; "" = the STA lease's gateway on the AP
       (192.168.80.1) or 192.168.7.1 on the USB link */
    SETTINGS_STR ("host", 15, ""),
    SETTINGS_INT ("gps_poll_s", 1, 60, 2),
    SETTINGS_INT ("health_poll_s", 5, 300, 10),
    /* POST usb_data attempts before a VBUS recovery cycle */
    SETTINGS_INT ("cut_retries", 1, 5, 2),
    /* the carrier settings the pairing pass writes to the dongle
       (its lte_upstream_pppos apn / apn_user / apn_password; the dongle
       restarts once when they changed). "" = leave the dongle's own
       (2026-10-07, Quick Setup's USB step; additive, no version bump) */
    SETTINGS_STR ("apn", 63, ""),
    SETTINGS_STR ("apn_user", 32, ""),
    SETTINGS_STR ("apn_password", 32, ""),
    /* the SIM's PIN (4 to 8 digits), relayed to the dongle's `apn_pin` once
       per boot while the dongle reports its SIM locked; a rejected PIN is
       never retried by itself (2026-10-13; additive, no version bump) */
    SETTINGS_STR ("sim_pin", 8, ""),
    SETTINGS_BOOL("cli", true),
};

static espnl_config_t s_cfg;
static bool s_configured;

const espnl_config_t *espnl_config(void)
{
    return &s_cfg;
}

bool espnl_config_is_configured(void)
{
    return s_configured;
}

static bool get_bool(const cJSON *o, const char *k, bool def)
{
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);

    return cJSON_IsBool(i) ? cJSON_IsTrue(i) : def;
}

static int get_int(const cJSON *o, const char *k, int def)
{
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);

    return cJSON_IsNumber(i) ? (int)i->valuedouble : def;
}

static void get_str(const cJSON *o, const char *k, char *dst, size_t len)
{
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);

    if (cJSON_IsString(i) && i->valuestring != NULL)
    {
        strncpy(dst, i->valuestring, len - 1);
        dst[len - 1] = '\0';
    }
    else
    {
        dst[0] = '\0';
    }
}

static esp_err_t on_apply(const cJSON *settings)
{
    char mode[16];

    s_cfg.enabled   = get_bool(settings, "enabled", true);
    get_str(settings, "mode", mode, sizeof(mode));
    s_cfg.mode      = (espnetlink_mode_t)espnl_core_mode_from_str(mode);
    s_cfg.auto_pair = get_bool(settings, "auto_pair", true);
    get_str(settings, "ssid", s_cfg.ssid, sizeof(s_cfg.ssid));
    get_str(settings, "device_id", s_cfg.device_id, sizeof(s_cfg.device_id));
    get_str(settings, "host", s_cfg.host, sizeof(s_cfg.host));
    s_cfg.gps_poll_s    = get_int(settings, "gps_poll_s", 2);
    s_cfg.health_poll_s = get_int(settings, "health_poll_s", 10);
    s_cfg.cut_retries   = get_int(settings, "cut_retries", 2);
    get_str(settings, "apn", s_cfg.apn, sizeof(s_cfg.apn));
    get_str(settings, "apn_user", s_cfg.apn_user, sizeof(s_cfg.apn_user));
    get_str(settings, "apn_password", s_cfg.apn_password,
            sizeof(s_cfg.apn_password));
    get_str(settings, "sim_pin", s_cfg.sim_pin, sizeof(s_cfg.sim_pin));
    s_cfg.cli           = get_bool(settings, "cli", true);
    s_configured = true;

    if (s_cfg.cli)
    {
        (void)espnetlink_link_register_cli();
    }

    return ESP_OK;
}

esp_err_t espnl_settings_register(void)
{
    static const settings_descriptor_t DESC =
    {
        .name        = "espnetlink",
        .version     = 2,
        .fields      = FIELDS,
        .field_count = sizeof(FIELDS) / sizeof(FIELDS[0]),
        .on_apply    = on_apply,
        .on_migrate  = espnl_settings_migrate,
    };

    return settings_manager_register(&DESC);
}
