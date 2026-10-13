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
 * @file rtc_manager_http.c
 * @brief The optional /api/rtc routes (§9.1: feature components register
 *        their own domain routes), main calls rtc_manager_register_http()
 *        only in HTTP compositions, so the chip code carries no HTTP
 *        dependency. Documented in components/HTTP_API.md section 6d.
 */
#include <stdio.h>
#include <time.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "cJSON.h"
#include "http_server_manager.h"

#include "rtc_manager.h"

static const char *TAG = "rtc_manager";

static esp_err_t rtc_get_handler(httpd_req_t *req)
{
    char now[24] = "";
    char chip[24] = "";
    char last[24] = "";
    char last_step[112] = "null";
    struct tm t;
    rtc_clock_state_t cs;
    char body[512];

    rtc_manager_now_iso8601(now, sizeof(now));

    if (rtc_manager_get_time(&t) == ESP_OK)
    {
        strftime(chip, sizeof(chip), "%Y-%m-%dT%H:%M:%SZ", &t);
    }

    time_t last_sync = rtc_manager_last_sync();

    if (last_sync > 0)
    {
        struct tm lt;

        gmtime_r(&last_sync, &lt);
        strftime(last, sizeof(last), "%Y-%m-%dT%H:%M:%SZ", &lt);
    }

    /* the clock's history (2026-10-13): every set is a measured step */
    rtc_manager_clock_state(&cs);

    if (cs.steps_this_boot > 0)
    {
        time_t at = (time_t)(cs.last.wall_ms / 1000);
        struct tm lt;
        char at_iso[24];

        gmtime_r(&at, &lt);
        strftime(at_iso, sizeof(at_iso), "%Y-%m-%dT%H:%M:%SZ", &lt);
        snprintf(last_step, sizeof(last_step),
                 "{\"delta_ms\":%lld,\"source\":\"%s\",\"at\":\"%s\"}",
                 (long long)cs.last.delta_ms,
                 rtc_manager_source_name(cs.last.source), at_iso);
    }

    snprintf(body, sizeof(body),
             "{\"time\":\"%s\",\"valid\":%s,\"rtc\":%s%s%s,"
             "\"sntp\":{\"enabled\":%s,\"server\":\"%s\","
             "\"last_sync\":%s%s%s},"
             "\"clock\":{\"source\":\"%s\",\"generation\":%lu,"
             "\"steps_this_boot\":%lu,\"last_step\":%s}}",
             now, rtc_manager_time_valid() ? "true" : "false",
             (chip[0] != '\0') ? "\"" : "",
             (chip[0] != '\0') ? chip : "null",
             (chip[0] != '\0') ? "\"" : "",
             rtc_manager_sntp_enabled() ? "true" : "false",
             rtc_manager_ntp_server(),
             (last[0] != '\0') ? "\"" : "",
             (last[0] != '\0') ? last : "null",
             (last[0] != '\0') ? "\"" : "",
             rtc_manager_source_name(cs.source),
             (unsigned long)cs.generation,
             (unsigned long)cs.steps_this_boot, last_step);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t rtc_sync_handler(httpd_req_t *req)
{
    /* blocking on-demand SNTP sync: ~16 s/server worst case; the UI
     * shows a spinner (same pattern as /api/wifi/scan) */
    esp_err_t err = rtc_manager_sync_now();

    if (err == ESP_ERR_INVALID_STATE)
    {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "no internet connection");
    }

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "on-demand SNTP sync failed");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "no NTP server answered");
    }

    return rtc_get_handler(req); /* respond with the fresh state */
}

static esp_err_t rtc_post_handler(httpd_req_t *req)
{
    char buf[96];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);

    if (len <= 0)
    {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "empty body");
    }

    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    const cJSON *epoch =
        cJSON_GetObjectItemCaseSensitive(root, "epoch");
    esp_err_t err = ESP_ERR_INVALID_ARG;

    if (cJSON_IsNumber(epoch))
    {
        err = rtc_manager_set_time((time_t)epoch->valuedouble);
    }

    cJSON_Delete(root);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "manual time set rejected");
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "epoch missing or outside 2020..2099");
    }

    return rtc_get_handler(req); /* respond with the new state */
}

esp_err_t rtc_manager_register_http(void)
{
    static const httpd_uri_t URIS[] =
    {
        { .uri = "/api/rtc", .method = HTTP_GET,
          .handler = rtc_get_handler },
        { .uri = "/api/rtc", .method = HTTP_POST,
          .handler = rtc_post_handler },
        { .uri = "/api/rtc/sync", .method = HTTP_POST,
          .handler = rtc_sync_handler },
    };

    esp_err_t err = http_server_manager_register_handlers(
        URIS, sizeof(URIS) / sizeof(URIS[0]));

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "/api/rtc registered");
    }

    return err;
}
