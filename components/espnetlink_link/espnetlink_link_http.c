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
 * @file espnetlink_link_http.c
 * @brief `GET /api/espnetlink` (status), `POST /api/espnetlink/pair`
 *        ({"ssid","password"} -> wifi_manager fallback slot + our ssid;
 *        reply {"ok":true,"slot":N,"reboot_required":bool}) and
 *        `POST /api/espnetlink/repair` (VBUS cycle -> key re-read).
 *        Reference: components/HTTP_API.md route map + this README.
 */
#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"

#include "api_http.h"
#include "cJSON.h"
#include "http_server_manager.h"

#include "espnetlink_link.h"
#include "espnetlink_link_core.h"

static esp_err_t send_json(httpd_req_t *req, cJSON *o)
{
    char *js = cJSON_PrintUnformatted(o);

    cJSON_Delete(o);
    if (js == NULL)
    {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "serialize");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, js, strlen(js));
    free(js);
    return ESP_OK;
}

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

static esp_err_t status_handler(httpd_req_t *req)
{
    espnetlink_link_status_t st;

    (void)espnetlink_link_status(&st);

    cJSON *o = cJSON_CreateObject();

    if (o == NULL)
    {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "no memory");
    }

    cJSON_AddBoolToObject(o, "enabled", st.enabled);
    cJSON_AddStringToObject(o, "mode",
                            espnl_core_mode_str((espnl_core_mode_t)st.mode));
    cJSON_AddBoolToObject(o, "auto_pair", st.auto_pair);
    cJSON_AddBoolToObject(o, "paired", st.paired);
    cJSON_AddStringToObject(o, "ssid", st.ssid);
    cJSON_AddStringToObject(o, "device_id", st.device_id);
    cJSON_AddStringToObject(o, "uplink", uplink_str(st.uplink));
    cJSON_AddBoolToObject(o, "on_link",
                          st.uplink == ESPNETLINK_UPLINK_ESPNETLINK_AP ||
                          st.uplink == ESPNETLINK_UPLINK_ESPNETLINK_USB);
    cJSON_AddStringToObject(o, "host", st.host);
    cJSON_AddBoolToObject(o, "pair_blocked_factory_pw",
                          st.pair_blocked_factory_pw);
    cJSON_AddStringToObject(o, "last_error", st.last_error);
    cJSON_AddStringToObject(o, "dongle_fw", st.dongle_fw);
    cJSON_AddNumberToObject(o, "dongle_api", st.dongle_api);
    cJSON_AddNumberToObject(o, "dongle_api_min", ESPNL_MIN_API_LEVEL);
    cJSON_AddBoolToObject(o, "health_unsupported", st.health_unsupported);
    /* the carrier settings relayed to the dongle (2026-10-07) */
    cJSON_AddStringToObject(o, "apn", st.apn);
    cJSON_AddBoolToObject(o, "carrier_synced", st.carrier_synced);
    cJSON_AddStringToObject(o, "carrier_note", st.carrier_note);

    /* the dongle preferences the pass enforces (2026-10-19) */
    cJSON *prefs = cJSON_AddObjectToObject(o, "prefs");

    cJSON_AddStringToObject(prefs, "agnss", st.prefs_agnss);
    cJSON_AddStringToObject(prefs, "wifi_ap", st.prefs_ap);

    cJSON *u = cJSON_AddObjectToObject(o, "usb");

    if (u != NULL)
    {
        cJSON_AddBoolToObject(u, "attached", st.usb_attached);
        /* 2026-10-12: a dongle in usb_mode=console on the port (CDC
         * console alone, no USB Ethernet, nothing to pair over USB) */
        cJSON_AddBoolToObject(u, "dongle_console", st.dongle_console);
        cJSON_AddStringToObject(u, "pair_state", st.pair_state);
        cJSON_AddNumberToObject(u, "cuts", st.cuts);
        cJSON_AddNumberToObject(u, "vbus_cycles", st.vbus_cycles);
        cJSON_AddNumberToObject(u, "errors", st.pair_errors);
    }

    cJSON *gps = cJSON_AddObjectToObject(o, "gps");

    if (gps != NULL)
    {
        usb_acm_gps_t g;
        bool have = espnetlink_link_gps_get(&g) == ESP_OK;

        cJSON_AddBoolToObject(gps, "valid", have);
        if (have)
        {
            cJSON_AddNumberToObject(gps, "latitude", g.latitude);
            cJSON_AddNumberToObject(gps, "longitude", g.longitude);
            cJSON_AddNumberToObject(gps, "altitude_m", g.altitude_m);
            cJSON_AddNumberToObject(gps, "speed_kmph", g.speed_kmph);
            cJSON_AddNumberToObject(gps, "heading_deg", g.heading_deg);
            cJSON_AddNumberToObject(gps, "satellites", g.satellites);
            cJSON_AddNumberToObject(gps, "accuracy_m", g.accuracy_m);
            cJSON_AddNumberToObject(gps, "age_ms", g.age_ms);
        }
    }

    cJSON *d = cJSON_AddObjectToObject(o, "dongle");

    if (d != NULL)
    {
        cJSON_AddBoolToObject(d, "valid", st.health_valid);
        if (st.health_valid)
        {
            cJSON_AddBoolToObject(d, "lte_connected", st.lte_connected);
            cJSON_AddNumberToObject(d, "rssi_dbm", st.rssi_dbm);
            cJSON_AddStringToObject(d, "operator", st.operator_name);
            cJSON_AddStringToObject(d, "network_type", st.network_type);
            cJSON_AddBoolToObject(d, "gps_fix", st.dongle_gps_fix);
            cJSON_AddBoolToObject(d, "usb_data", st.dongle_usb_data);
            /* dongle api 8 (2026-10-07): "" from an older one */
            cJSON_AddStringToObject(d, "sim", st.dongle_sim);
            cJSON_AddStringToObject(d, "stage", st.dongle_stage);
            cJSON_AddStringToObject(d, "ip", st.dongle_ip);
            cJSON_AddNumberToObject(d, "age_s", st.health_age_ms / 1000);
        }
    }

    /* the GPS hint (2026-10-13) */
    cJSON *hint = cJSON_AddObjectToObject(o, "gps_hint");

    if (hint != NULL)
    {
        cJSON_AddNumberToObject(hint, "no_fix_s", st.no_fix_s);
        cJSON_AddBoolToObject(hint, "no_fix", st.no_fix_hint);
    }

    cJSON_AddNumberToObject(o, "polls", st.polls);
    cJSON_AddNumberToObject(o, "failures", st.failures);
    cJSON_AddNumberToObject(o, "link_ups", st.link_ups);

    return send_json(req, o);
}

static esp_err_t pair_handler(httpd_req_t *req)
{
    size_t len = req->content_len;

    if (len == 0 || len > 512)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing/big body");
        return ESP_FAIL;
    }

    char body[513];
    size_t got = 0;

    while (got < len)
    {
        int r = httpd_req_recv(req, body + got, len - got);

        if (r <= 0)
        {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv");
            return ESP_FAIL;
        }
        got += (size_t)r;
    }
    body[len] = '\0';

    cJSON *root = cJSON_Parse(body);

    if (root == NULL)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_FAIL;
    }

    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(root, "password");

    if (!cJSON_IsString(ssid) || ssid->valuestring[0] == '\0')
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_FAIL;
    }

    int slot = -1;
    esp_err_t err = espnetlink_link_pair(
        ssid->valuestring,
        cJSON_IsString(pass) ? pass->valuestring : "", &slot);

    cJSON_Delete(root);

    if (err != ESP_OK)
    {
        httpd_resp_send_err(req, err == ESP_ERR_NO_MEM
                                     ? HTTPD_400_BAD_REQUEST
                                     : HTTPD_500_INTERNAL_SERVER_ERROR,
                            err == ESP_ERR_NO_MEM ? "no free fallback slot"
                                                  : esp_err_to_name(err));
        return ESP_FAIL;
    }

    /* two descriptors may have been written outside the PUT transport:
     * make the next /api/settings/submit reboot like a staged batch */
    api_http_note_settings_changed();

    cJSON *o = cJSON_CreateObject();

    if (o == NULL)
    {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "no memory");
    }
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddNumberToObject(o, "slot", slot);
    cJSON_AddBoolToObject(o, "reboot_required", true);
    return send_json(req, o);
}

static esp_err_t repair_handler(httpd_req_t *req)
{
    esp_err_t err = espnetlink_link_repair();

    if (err != ESP_OK)
    {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"error\":\"USB host is not up\"}");
    }

    cJSON *o = cJSON_CreateObject();

    if (o == NULL)
    {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "no memory");
    }
    cJSON_AddBoolToObject(o, "ok", true);
    return send_json(req, o);
}

esp_err_t espnetlink_link_register_http(void)
{
    static const httpd_uri_t URIS[] =
    {
        { .uri = "/api/espnetlink", .method = HTTP_GET,
          .handler = status_handler },
        { .uri = "/api/espnetlink/pair", .method = HTTP_POST,
          .handler = pair_handler },
        { .uri = "/api/espnetlink/repair", .method = HTTP_POST,
          .handler = repair_handler },
    };

    return http_server_manager_register_handlers(
        URIS, sizeof(URIS) / sizeof(URIS[0]));
}
