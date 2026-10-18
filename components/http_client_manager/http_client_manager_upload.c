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
 * @file http_client_manager_upload.c
 * @brief A chunked POST of a memory body with progress (2026-10-19: the
 *        firmware image fw_update pushes into the ESPNetLink dongle). The
 *        Content-Length is announced at the open, the body goes out in
 *        pieces so the caller sees the progress and may stop between two
 *        of them, and the answer (a small JSON) is read capped. No
 *        redirect: a 3xx comes back as the status.
 */
#include <string.h>

#include "esp_log.h"

#include "http_client_manager_engine.h"
#include "http_client_manager_private.h"

static const char *TAG = HC_TAG;

#define HC_UPLOAD_CHUNK 8192

esp_err_t http_client_manager_upload(const http_client_request_t *req,
                                     size_t chunk,
                                     http_client_upload_progress_t progress,
                                     void *ctx,
                                     http_client_upload_info_t *info)
{
    if (req == NULL || info == NULL || req->body == NULL ||
        req->body_len == 0 || !hc_url_valid(req->url) || !hc_inited())
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(info, 0, sizeof(*info));
    info->status_code = -1;

    if (chunk == 0)
    {
        chunk = HC_UPLOAD_CHUNK;
    }

    http_client_request_t r = *req;

    if (r.method == HTTP_CLIENT_GET)
    {
        r.method = HTTP_CLIENT_POST;
    }

    hc_limiter_take();

    esp_http_client_handle_t client = hc_client_for(&r, NULL);
    esp_err_t err = (client != NULL) ? ESP_OK : ESP_FAIL;

    if (err == ESP_OK)
    {
        err = esp_http_client_open(client, (int)r.body_len);
    }

    const uint8_t *p = r.body;
    size_t left = r.body_len;

    while (err == ESP_OK && left > 0)
    {
        size_t n = (left < chunk) ? left : chunk;
        int w = esp_http_client_write(client, (const char *)p, (int)n);

        if (w != (int)n)
        {
            ESP_LOGW(TAG, "upload %s: write %d of %u after %u bytes", r.url,
                     w, (unsigned)n, (unsigned)info->sent);
            err = ESP_FAIL;
            break;
        }

        p += n;
        left -= n;
        info->sent += n;

        if (progress != NULL && !progress(ctx, info->sent, r.body_len))
        {
            info->aborted = true;
            err = ESP_ERR_INVALID_STATE;
            break;
        }
    }

    if (err == ESP_OK)
    {
        /* the peer validates what it received before it answers (an OTA
           digest pass takes seconds): the request's timeout bounds it */
        int64_t total = esp_http_client_fetch_headers(client);

        if (total < 0 && total != -ESP_ERR_HTTP_EAGAIN)
        {
            err = ESP_FAIL;
        }
        else
        {
            size_t got = 0;

            info->status_code = esp_http_client_get_status_code(client);

            while (got < sizeof(info->body) - 1)
            {
                int rd = esp_http_client_read(client, info->body + got,
                                              (int)(sizeof(info->body) - 1 - got));

                if (rd <= 0)
                {
                    break;
                }

                got += (size_t)rd;
            }

            info->body[got] = '\0';
            info->body_len = got;
        }
    }

    if (client != NULL)
    {
        esp_http_client_cleanup(client);
    }

    hc_limiter_give();
    hc_stats_note(err == ESP_OK, info->body_len);

    if (err != ESP_OK)
    {
        ESP_LOGD(TAG, "upload %s: %s after %u of %u bytes", r.url,
                 esp_err_to_name(err), (unsigned)info->sent,
                 (unsigned)r.body_len);
    }

    return err;
}
