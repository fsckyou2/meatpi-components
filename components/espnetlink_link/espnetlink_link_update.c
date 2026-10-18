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
 * @file espnetlink_link_update.c
 * @brief What a firmware update of the dongle through this WiCAN needs
 *        from the link (2026-10-19, internal/TASK_fw_update.md section 6):
 *        where the dongle answers right now and whether the pairing
 *        machine is at rest (the endpoint), a hold on the link's own
 *        traffic to the dongle while an image is pushed (its httpd is one
 *        worker: a GPS poll in the middle of the upload would stall it,
 *        three stalled polls would kick the association), the expected
 *        restart after the push, and an identify on demand so the new
 *        firmware's version is read without waiting for the next pass.
 *        The relay itself lives in fw_update, wired by main: this
 *        component never depends on it.
 */
#include "espnetlink_link_private.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "http_client_manager.h"

static const char *TAG = "espnetlink";

#define UPD_INFO_RESP_MAX 1024

/** The pairing machine states in which the dongle is not being talked to
 *  by the link and will not be in the next second. */
static bool state_is_steady(const char *state)
{
    static const char *const BUSY[] =
    {
        "identify", "read_key", "cut", "wait_drop", "ncm_share",
    };

    if (state == NULL)
    {
        return true;
    }

    for (size_t i = 0; i < sizeof(BUSY) / sizeof(BUSY[0]); i++)
    {
        if (strcmp(state, BUSY[i]) == 0)
        {
            return false;
        }
    }

    return true;
}

static void fill_endpoint(const espnetlink_link_status_t *st,
                          espnetlink_endpoint_t *out)
{
    bool usb = st->uplink == ESPNETLINK_UPLINK_ESPNETLINK_USB;
    bool ap = st->uplink == ESPNETLINK_UPLINK_ESPNETLINK_AP;

    memset(out, 0, sizeof(*out));
    out->present = st->enabled &&
                   (st->paired || st->usb_attached || st->dongle_console ||
                    st->dongle_fw[0] != '\0');
    out->reachable = st->host[0] != '\0' && (usb || ap);
    strlcpy(out->host, st->host, sizeof(out->host));
    strlcpy(out->via, usb ? "usb" : ap ? "wifi" : "", sizeof(out->via));
    out->steady = out->reachable && state_is_steady(st->pair_state);
    out->usb_attached = st->usb_attached;
    strlcpy(out->device_id, st->device_id, sizeof(out->device_id));
    strlcpy(out->fw_version, st->dongle_fw, sizeof(out->fw_version));
    out->api_level = st->dongle_api;
}

esp_err_t espnetlink_link_endpoint(espnetlink_endpoint_t *out)
{
    static espnetlink_link_status_t st EXT_RAM_BSS_ATTR; /* ~700 B: not on the caller's stack */
    static SemaphoreHandle_t s_gate;
    static StaticSemaphore_t s_gate_buf;                 /* internal: FreeRTOS object */

    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_gate == NULL)
    {
        s_gate = xSemaphoreCreateMutexStatic(&s_gate_buf);
    }

    xSemaphoreTake(s_gate, portMAX_DELAY);

    esp_err_t err = espnetlink_link_status(&st);

    if (err == ESP_OK)
    {
        fill_endpoint(&st, out);
    }
    else
    {
        memset(out, 0, sizeof(*out));
    }

    xSemaphoreGive(s_gate);
    return err;
}

esp_err_t espnetlink_link_hold(bool hold)
{
    espnl_engine_set_update_hold(hold);
    ESP_LOGI(TAG, "update hold %s", hold ? "on: the link's polls pause"
                                         : "off");
    return ESP_OK;
}

void espnetlink_link_expect_dongle_reboot(void)
{
    espnl_engine_dongle_rebooting();
}

esp_err_t espnetlink_link_identify_now(int timeout_ms,
                                       espnetlink_endpoint_t *out)
{
    espnetlink_endpoint_t ep;
    char url[80];
    http_client_response_t resp;
    espnl_info_t info;

    if (espnetlink_link_endpoint(&ep) != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!ep.reachable ||
        !espnl_core_url(url, sizeof(url), ep.host, "/api/info"))
    {
        return ESP_ERR_NOT_FOUND;
    }

    http_client_request_t req =
    {
        .url          = url,
        .method       = HTTP_CLIENT_GET,
        .timeout_ms   = (timeout_ms > 0) ? timeout_ms : 5000,
        .max_response = UPD_INFO_RESP_MAX,
    };

    memset(&resp, 0, sizeof(resp));

    esp_err_t err = http_client_manager_request(&req, &resp);
    bool parsed = err == ESP_OK && resp.status_code == 200 &&
                  resp.data != NULL && espnl_core_parse_info(resp.data, &info) &&
                  info.valid && info.is_espnetlink;

    http_client_manager_free(&resp);

    if (!parsed)
    {
        ESP_LOGD(TAG, "identify now: %s status %d", esp_err_to_name(err),
                 resp.status_code);
        return (err == ESP_OK) ? ESP_ERR_INVALID_RESPONSE : err;
    }

    espnl_engine_note_info(info.device_id, info.fw_version, info.api_level);

    if (out != NULL)
    {
        (void)espnetlink_link_endpoint(out);
    }

    return ESP_OK;
}
