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
 * @file http_client_manager_download.c
 * @brief The atomic download to the filesystem (moved out of the engine
 *        file on 2026-10-19, the upload beside it): the caller reads the
 *        network into one PSRAM buffer and feeds filesystem's async pipe,
 *        whose internal-stack writer task does the media writes, so the
 *        receive and the write overlap.
 */
#include <string.h>

#include "esp_attr.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "filesystem.h"

#include "http_client_manager_engine.h"
#include "http_client_manager_private.h"

static const char *TAG = HC_TAG;

/* The CALLER reads the network into one PSRAM buffer and feeds
 * filesystem's ASYNC PIPE (its internal-stack writer task does the
 * media writes): receive and write overlap, so the slower of (network,
 * media) sets the pace instead of their sum. */

#define HC_DL_BUF_SIZE (32 * 1024)

static uint8_t s_dl_buf[HC_DL_BUF_SIZE] EXT_RAM_BSS_ATTR;
static SemaphoreHandle_t s_dl_mutex;      /* one download at a time     */
static StaticSemaphore_t s_dl_mutex_buf;  /* internal: FreeRTOS object  */

esp_err_t hc_xfer_init(void)
{
    if (s_dl_mutex == NULL)
    {
        s_dl_mutex = xSemaphoreCreateMutexStatic(&s_dl_mutex_buf);
    }

    return (s_dl_mutex != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t http_client_manager_download(const char *url,
                                       const char *save_path,
                                       void (*progress_cb)(size_t done,
                                                           size_t total))
{
    if (!hc_url_valid(url) || save_path == NULL || !hc_inited())
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_dl_mutex, portMAX_DELAY);   /* one download       */
    hc_limiter_take();                           /* one request slot   */

    filesystem_wstream_t *ws = NULL;
    esp_err_t err = filesystem_write_open(save_path, &ws);
    bool pipe_open = false;
    esp_http_client_handle_t client = NULL;
    int status = -1;
    size_t total_rx = 0;
    int64_t total = -1;
    int hops = 0;

    if (err == ESP_OK)
    {
        err = filesystem_write_async_begin(ws);
        pipe_open = (err == ESP_OK);
    }

    if (err == ESP_OK)
    {
        http_client_request_t get = { .url = url };

        client = hc_client_for(&get, NULL);
        err = (client != NULL)
                  ? hc_open_and_fetch(client, &get, &status, &total, &hops)
                  : ESP_FAIL;
    }

    if (err == ESP_OK && (status < 200 || status >= 300))
    {
        err = ESP_ERR_INVALID_RESPONSE;
    }

    while (err == ESP_OK)
    {
        /* read into our buffer while filesystem's writer task flushes
         * the previous chunk: receive and media write overlap */
        int r = esp_http_client_read(client, (char *)s_dl_buf,
                                     sizeof(s_dl_buf));

        if (r < 0)
        {
            err = ESP_FAIL;
        }
        else if (r == 0)
        {
            break; /* body complete */
        }
        else
        {
            err = filesystem_write_async_chunk(ws, s_dl_buf, (size_t)r);
            total_rx += (size_t)r;

            if (progress_cb != NULL)
            {
                progress_cb(total_rx, (total > 0) ? (size_t)total : 0);
            }
        }
    }

    if (client != NULL)
    {
        esp_http_client_cleanup(client);
    }

    if (pipe_open)
    {
        esp_err_t end_err = filesystem_write_async_end(ws,
                                                       err == ESP_OK);

        if (err == ESP_OK)
        {
            err = end_err;
        }
    }
    else if (ws != NULL)
    {
        filesystem_write_abort(ws);
    }

    hc_stats_note(err == ESP_OK, total_rx);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "downloaded %s -> %s (%u bytes)", url, save_path,
                 (unsigned)total_rx);
    }
    else
    {
        ESP_LOGW(TAG, "download %s failed: %s (status %d)", url,
                 esp_err_to_name(err), status);
    }

    hc_limiter_give();
    xSemaphoreGive(s_dl_mutex);
    return err;
}
