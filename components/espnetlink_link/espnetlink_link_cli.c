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
 * @file espnetlink_link_cli.c
 * @brief `espnetlink`: status; `espnetlink pair <ssid> <password>`;
 *        `espnetlink repair` (VBUS cycle -> key re-read). Registered on
 *        the settings boot apply when `cli` is true.
 */
#include <stdio.h>
#include <string.h>

#include "esp_console.h"

#include "cmdline_manager.h"

#include "espnetlink_link.h"
#include "espnetlink_link_core.h"

static const char *uplink_str(espnetlink_uplink_t u)
{
    switch (u)
    {
        case ESPNETLINK_UPLINK_WIFI:           return "wifi";
        case ESPNETLINK_UPLINK_ESPNETLINK_AP:  return "espnetlink";
        case ESPNETLINK_UPLINK_ESPNETLINK_USB: return "espnetlink_usb";
        default:                               return "none";
    }
}

static int cmd_espnetlink(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "pair") == 0)
    {
        int slot = -1;
        esp_err_t err = espnetlink_link_pair(argv[2],
                                             argc >= 4 ? argv[3] : "",
                                             &slot);

        if (err != ESP_OK)
        {
            cmdline_printf("pair failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        cmdline_printf("paired '%s' (wifi slot %d): reboot to apply\n",
                       argv[2], slot);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "repair") == 0)
    {
        esp_err_t err = espnetlink_link_repair();

        cmdline_printf("%s\n", err == ESP_OK
                                   ? "re-pair requested (VBUS cycle)"
                                   : "USB host is not up");
        return err == ESP_OK ? 0 : 1;
    }

    espnetlink_link_status_t st;

    (void)espnetlink_link_status(&st);

    cmdline_printf("ESPNETLINK: enabled=%d mode=%s auto_pair=%d paired=%d "
                   "ssid='%s' device_id=%s uplink=%s host=%s fw=%s api=%d"
                   "%s\n",
                   (int)st.enabled,
                   espnl_core_mode_str((espnl_core_mode_t)st.mode),
                   (int)st.auto_pair, (int)st.paired, st.ssid,
                   st.device_id[0] ? st.device_id : "-",
                   uplink_str(st.uplink), st.host[0] ? st.host : "-",
                   st.dongle_fw[0] ? st.dongle_fw : "-", st.dongle_api,
                   (st.dongle_api != 0 && st.dongle_api < ESPNL_MIN_API_LEVEL)
                       ? " (older than this WiCAN expects)" : "");
    cmdline_printf("  usb: attached=%d console_only=%d pair_state=%s cuts=%lu "
                   "vbus_cycles=%lu errors=%lu\n",
                   (int)st.usb_attached, (int)st.dongle_console,
                   st.pair_state, (unsigned long)st.cuts,
                   (unsigned long)st.vbus_cycles,
                   (unsigned long)st.pair_errors);
    if (st.pair_blocked_factory_pw || st.last_error[0] != '\0')
    {
        cmdline_printf("  pairing: %s%s%s\n",
                       st.pair_blocked_factory_pw
                           ? "ON HOLD (factory AP password)" : "",
                       (st.pair_blocked_factory_pw &&
                        st.last_error[0] != '\0') ? " | " : "",
                       st.last_error);
    }
    cmdline_printf("  gps: valid=%d age=%lu ms | dongle: valid=%d%s lte=%d "
                   "rssi=%d op='%s' net=%s fix=%d usb_data=%d sim=%s "
                   "stage=%s ip=%s age=%lu s | polls=%lu "
                   "fail=%lu link_ups=%lu\n",
                   (int)st.gps_valid, (unsigned long)st.gps_age_ms,
                   (int)st.health_valid,
                   st.health_unsupported ? " (no health API in this dongle "
                                           "firmware)" : "",
                   (int)st.lte_connected, st.rssi_dbm,
                   st.operator_name, st.network_type[0] ? st.network_type
                                                        : "-",
                   (int)st.dongle_gps_fix, (int)st.dongle_usb_data,
                   st.dongle_sim[0] ? st.dongle_sim : "-",
                   st.dongle_stage[0] ? st.dongle_stage : "-",
                   st.dongle_ip[0] ? st.dongle_ip : "-",
                   (unsigned long)(st.health_age_ms / 1000),
                   (unsigned long)st.polls, (unsigned long)st.failures,
                   (unsigned long)st.link_ups);
    if (st.apn[0] != '\0' || !st.carrier_synced)
    {
        cmdline_printf("  carrier: apn='%s' %s%s%s\n", st.apn,
                       st.carrier_synced ? "on the dongle" : "NOT on the dongle",
                       st.carrier_note[0] ? ": " : "", st.carrier_note);
    }

    if (st.gps_valid)
    {
        usb_acm_gps_t g;

        if (espnetlink_link_gps_get(&g) == ESP_OK)
        {
            cmdline_printf("  fix: %.6f,%.6f alt %.1f m %.1f km/h hdg %.0f "
                           "sats %d\n",
                           g.latitude, g.longitude, g.altitude_m,
                           g.speed_kmph, g.heading_deg, g.satellites);
        }
    }
    return 0;
}

esp_err_t espnetlink_link_register_cli(void)
{
    static bool s_registered;

    if (s_registered)
    {
        return ESP_OK;
    }

    const esp_console_cmd_t cmd =
    {
        .command = "espnetlink",
        .help = "ESPNetLink dongle: status; `espnetlink pair <ssid> "
                "<password>`; `espnetlink repair`",
        .hint = NULL,
        .func = &cmd_espnetlink,
    };

    esp_err_t err = cmdline_manager_register(&cmd);

    s_registered = (err == ESP_OK);
    return err;
}
