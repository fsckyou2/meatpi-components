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
 * @file fw_update_fetch_ram.c
 * @brief A release's image downloaded into a PSRAM buffer of exactly its
 *        size (the dongle relay stages the image before it pushes it):
 *        the same stream, size rule, abort and three attempts as the pipe
 *        path, into memory instead of a flash slot.
 */
#include "fw_update_private.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "http_client_manager.h"

static const char *TAG = FWU_TAG;

#define FWU_IMAGE_TIMEOUT_MS 20000   /* per read                           */
#define FWU_IMAGE_ATTEMPTS   3

static const uint32_t RETRY_WAIT_S[FWU_IMAGE_ATTEMPTS] = { 5, 20, 60 };

/* ---- the image into RAM (the dongle relay, 2026-10-19) ------------------------------- */

typedef struct
{
    uint8_t *buf;
    size_t   cap;
    size_t   received;
    bool     size_mismatch;
    int64_t  content_length;
    volatile bool *abort;
    fwu_progress_fn progress;
    int64_t  last_progress_us;
} ram_sink_t;

static bool ram_headers(int status, int64_t content_length, void *ctx)
{
    ram_sink_t *s = ctx;

    s->content_length = content_length;

    if (status != 200)
    {
        return false;
    }

    if (content_length >= 0 && (size_t)content_length != s->cap)
    {
        s->size_mismatch = true;
        return false;
    }

    return true;
}

static esp_err_t ram_data(const uint8_t *data, size_t len, void *ctx)
{
    ram_sink_t *s = ctx;

    if (s->abort != NULL && *s->abort)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (s->received + len > s->cap)
    {
        s->size_mismatch = true;
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(s->buf + s->received, data, len);
    s->received += len;

    int64_t now = esp_timer_get_time();

    if (s->progress != NULL && now - s->last_progress_us > 250000)
    {
        s->last_progress_us = now;
        s->progress(s->received, s->cap);
    }

    return ESP_OK;
}

static esp_err_t ram_attempt(const fwu_release_t *r, const fwu_config_t *cfg,
                             uint8_t *buf, fwu_progress_fn progress,
                             volatile bool *abort, char *code, char *detail,
                             bool *retry)
{
    ram_sink_t sink =
    {
        .buf = buf, .cap = r->size, .abort = abort, .progress = progress,
    };
    http_client_request_t req =
    {
        .url = r->url,
        .cert_set = (cfg->cert_set[0] != '\0') ? cfg->cert_set : NULL,
        .timeout_ms = FWU_IMAGE_TIMEOUT_MS,
    };
    http_client_stream_cb_t cb = { .on_headers = ram_headers, .on_data = ram_data };
    http_client_stream_info_t info;

    *retry = false;
    code[0] = '\0';
    detail[0] = '\0';

    esp_err_t err = http_client_manager_stream(&req, &cb, &sink, &info);

    if (abort != NULL && *abort)
    {
        snprintf(code, FW_UPDATE_CODE_LEN, "aborted");
        return ESP_ERR_INVALID_STATE;
    }

    if (sink.size_mismatch)
    {
        snprintf(code, FW_UPDATE_CODE_LEN, "size_mismatch");
        snprintf(detail, FW_UPDATE_DETAIL_LEN,
                 "the server offers %lld bytes, the manifest says %lu",
                 (long long)info.content_length, (unsigned long)r->size);
        return ESP_ERR_INVALID_SIZE;
    }

    if (err == ESP_ERR_INVALID_RESPONSE || (err == ESP_OK && info.status_code != 200))
    {
        snprintf(code, FW_UPDATE_CODE_LEN,
                 (info.status_code == 404) ? "image_not_found" : "image_fetch_failed");
        snprintf(detail, FW_UPDATE_DETAIL_LEN, "HTTP %d", info.status_code);
        *retry = info.status_code >= 500;
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (err != ESP_OK || !info.complete || sink.received != r->size)
    {
        snprintf(code, FW_UPDATE_CODE_LEN, "image_fetch_failed");
        snprintf(detail, FW_UPDATE_DETAIL_LEN, "%lu of %lu bytes (%s)",
                 (unsigned long)sink.received, (unsigned long)r->size,
                 (err != ESP_OK) ? esp_err_to_name(err) : "incomplete");
        *retry = true;
        return (err != ESP_OK) ? err : ESP_ERR_INVALID_SIZE;
    }

    if (progress != NULL)
    {
        progress(sink.received, r->size);
    }

    return ESP_OK;
}

esp_err_t fwu_fetch_image_to_ram(const fwu_release_t *r, const fwu_config_t *cfg,
                                 uint8_t *buf, fwu_progress_fn progress,
                                 volatile bool *abort, uint8_t *attempt,
                                 char *code, char *detail)
{
    esp_err_t err = ESP_FAIL;

    code[0] = '\0';
    detail[0] = '\0';

    for (int i = 0; i < FWU_IMAGE_ATTEMPTS; i++)
    {
        bool retry = false;

        *attempt = (uint8_t)(i + 1);
        err = ram_attempt(r, cfg, buf, progress, abort, code, detail, &retry);

        if (err == ESP_OK)
        {
            return ESP_OK;
        }

        ESP_LOGW(TAG, "image %s (to RAM) attempt %d: %s (%s)", r->version_str,
                 i + 1, code, detail);

        if (!retry || i + 1 >= FWU_IMAGE_ATTEMPTS)
        {
            break;
        }

        for (uint32_t s = 0; s < RETRY_WAIT_S[i]; s++)
        {
            if (abort != NULL && *abort)
            {
                snprintf(code, FW_UPDATE_CODE_LEN, "aborted");
                return ESP_ERR_INVALID_STATE;
            }

            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    return err;
}
