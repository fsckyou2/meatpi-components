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
 * @file usb_acm_cli_http.c
 * @brief POST /api/usb/acm/cmd: send a console line to the dongle, return
 *        the response. GET /api/usb/acm: the bound device and the two
 *        switches. GET /api/gps: the last fix.
 */
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "http_server_manager.h"

#include "usb_acm_cli.h"

static const char *TAG = "usb_acm_cli";

static esp_err_t send_json(httpd_req_t *req, cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);

    cJSON_Delete(o);

    if (s == NULL)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_send(req, s, HTTPD_RESP_USE_STRLEN);
    free(s);
    return r;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();

    cJSON_AddBoolToObject(o, "connected", usb_acm_cli_connected());
    /* 2026-10-07: whether anyone reads the device, and what it turned out
     * to be; 2026-10-11: the two switches as applied (issue #978) */
    cJSON_AddBoolToObject(o, "reading", usb_acm_cli_reading());
    cJSON_AddStringToObject(o, "mode", usb_acm_cli_mode());
    cJSON_AddNumberToObject(o, "nmea_sentences", usb_acm_cli_nmea_sentences());
    cJSON_AddBoolToObject(o, "gps", usb_acm_cli_gps_enabled());
    cJSON_AddBoolToObject(o, "console", usb_acm_cli_console_enabled());
    return send_json(req, o);
}

/* GET /api/gps: the last GPS fix (cache read). Device-contract field names;
 * `speed` in m/s per DEVICE_ENDPOINTS.md. Also flows to HA as autopid
 * gps_* parameters (main-wired). */
static esp_err_t gps_handler(httpd_req_t *req)
{
    usb_acm_gps_t g;
    cJSON *o = cJSON_CreateObject();

    if (o == NULL)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    (void)usb_acm_cli_gps_get(&g);
    cJSON_AddBoolToObject(o, "valid", g.valid);

    if (g.valid)
    {
        cJSON_AddNumberToObject(o, "latitude", g.latitude);
        cJSON_AddNumberToObject(o, "longitude", g.longitude);
        cJSON_AddNumberToObject(o, "accuracy", g.accuracy_m);
        cJSON_AddNumberToObject(o, "altitude", g.altitude_m);
        cJSON_AddNumberToObject(o, "speed", g.speed_kmph / 3.6); /* m/s */
        cJSON_AddNumberToObject(o, "heading", g.heading_deg);
        cJSON_AddNumberToObject(o, "satellites", g.satellites);
        cJSON_AddNumberToObject(o, "age_ms", g.age_ms);
    }

    return send_json(req, o);
}

static esp_err_t cmd_handler(httpd_req_t *req)
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

    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");

    if (!cJSON_IsString(cmd))
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need \"cmd\"");
        return ESP_FAIL;
    }

    /* cap, not latency: collection ends at the dongle's prompt, but
     * the modem legs of lte/gps commands legitimately take seconds */
    uint32_t timeout = 8000;
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "timeout_ms");
    if (cJSON_IsNumber(t) && t->valueint > 0)
    {
        timeout = (uint32_t)t->valueint;
    }

    char *resp = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (resp == NULL)
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    size_t rn = 0;
    esp_err_t err = usb_acm_cli_command(cmd->valuestring, resp, 2048, &rn,
                                        timeout);
    cJSON_Delete(root);

    cJSON *o = cJSON_CreateObject();

    if (err == ESP_ERR_NOT_SUPPORTED || err == ESP_ERR_INVALID_STATE)
    {
        free(resp);
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddStringToObject(o, "error", (err == ESP_ERR_NOT_SUPPORTED)
            ? "the ESPNetLink console is off (USB > Dongle console)"
            : "no ACM device / busy");
        httpd_resp_set_status(req, "409 Conflict");
        return send_json(req, o);
    }

    cJSON_AddBoolToObject(o, "ok", err == ESP_OK);
    cJSON_AddStringToObject(o, "response", (err == ESP_OK) ? resp : "");
    cJSON_AddBoolToObject(o, "connected", usb_acm_cli_connected());
    free(resp);
    return send_json(req, o);
}

esp_err_t usb_acm_cli_register_http(void)
{
    static const httpd_uri_t URIS[] =
    {
        { .uri = "/api/usb/acm", .method = HTTP_GET,
          .handler = status_handler },
        { .uri = "/api/usb/acm/cmd", .method = HTTP_POST,
          .handler = cmd_handler },
        { .uri = "/api/gps", .method = HTTP_GET,
          .handler = gps_handler },
    };

    esp_err_t err = http_server_manager_register_handlers(
        URIS, sizeof(URIS) / sizeof(URIS[0]));

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "/api/usb/acm routes registered");
    }

    return err;
}
