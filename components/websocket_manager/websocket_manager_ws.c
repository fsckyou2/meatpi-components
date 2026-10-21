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
 * @file websocket_manager_ws.c
 * @brief The httpd WebSocket layer: route registration on the ONE server
 *        (http_server_manager), handshake gating at max_clients, frame RX
 *        into the subscriber queue, fan-out TX via async frame sends, and
 *        send-failure/fd-state client reaping.
 *
 * httpd handles PING/PONG/CLOSE control frames itself
 * (handle_ws_control_frames = false); a closed client's fd stops reporting
 * HTTPD_WS_CLIENT_WEBSOCKET and is reaped on the next send.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "lwip/sockets.h"

#include "http_server_manager.h"

#include "websocket_manager.h"
#include "websocket_manager_private.h"

static const char *TAG = "websocket_manager";

#define WSM_FRAME_MAX 4096 /* per-frame RX cap (request-scoped PSRAM) */

/* A client is registered by the pre-handshake hook, BEFORE its 101 goes
 * out, so for a moment it reads HTTPD_WS_CLIENT_HTTP: a fan-out send in
 * that moment used to reap it (2026-10-21, the release test: "client
 * reaped" and "client connected" in the same millisecond, the client then
 * got nothing and a third one took its slot). Within this grace an HTTP
 * state means "handshake in flight"; past it, a ghost (a silently closed
 * fd that an HTTP session reused). */
#define WSM_HANDSHAKE_GRACE_US (3LL * 1000 * 1000)

typedef struct
{
    int fds[WEBSOCKET_MANAGER_MAX_CLIENTS]; /* -1 = free                */
    int64_t since[WEBSOCKET_MANAGER_MAX_CLIENTS]; /* registered at (us) */
    QueueHandle_t sub;
    websocket_stats_t stats;
} wsm_channel_t;

static wsm_channel_t s_ch[WEBSOCKET_MANAGER_MAX_CHANNELS] EXT_RAM_BSS_ATTR;

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf; /* internal: FreeRTOS object */

/* route table handed to httpd: must outlive the server */
static httpd_uri_t s_uris[WEBSOCKET_MANAGER_MAX_CHANNELS] EXT_RAM_BSS_ATTR;

/* ---- client table helpers (under s_lock) -------------------------------------- */

static int client_count(const wsm_channel_t *ch)
{
    int n = 0;

    for (int i = 0; i < WEBSOCKET_MANAGER_MAX_CLIENTS; i++)
    {
        n += (ch->fds[i] >= 0) ? 1 : 0;
    }

    return n;
}

static void client_remove(wsm_channel_t *ch, int fd)
{
    for (int i = 0; i < WEBSOCKET_MANAGER_MAX_CLIENTS; i++)
    {
        if (ch->fds[i] == fd)
        {
            ch->fds[i] = -1;
            ch->stats.clients--;
        }
    }
}

static bool client_add(wsm_channel_t *ch, int fd)
{
    client_remove(ch, fd); /* fd reuse after a silent close */

    for (int i = 0; i < WEBSOCKET_MANAGER_MAX_CLIENTS; i++)
    {
        if (ch->fds[i] < 0)
        {
            ch->fds[i] = fd;
            ch->since[i] = esp_timer_get_time();
            ch->stats.clients++;
            return true;
        }
    }

    return false;
}

/** A registered fd that is no longer a client: closed (httpd knows no
 *  session, or a reused fd that stayed HTTP past the handshake grace).
 *  A handshake still in flight is NOT a ghost. */
static bool client_is_ghost(httpd_handle_t server, int fd, int64_t since)
{
    httpd_ws_client_info_t info = httpd_ws_get_fd_info(server, fd);

    if (info == HTTPD_WS_CLIENT_WEBSOCKET)
    {
        return false;
    }

    return info != HTTPD_WS_CLIENT_HTTP ||
           esp_timer_get_time() - since > WSM_HANDSHAKE_GRACE_US;
}

/* ---- handshake gate (IDF v6: the uri handler is NOT called at handshake,
 * the pre-handshake callback is the sanctioned hook, and it can refuse
 * BEFORE the 101 goes out) --------------------------------------------------------- */

static esp_err_t ws_pre_handshake(httpd_req_t *req)
{
    int idx = (int)(intptr_t)req->user_ctx;
    const wsm_channel_cfg_t *cfg = wsm_core_config(idx);
    wsm_channel_t *ch = &s_ch[idx];

    if (cfg == NULL)
    {
        return ESP_FAIL;
    }

    int fd = httpd_req_to_sockfd(req);
    httpd_handle_t server = http_server_manager_handle();

    xSemaphoreTake(s_lock, portMAX_DELAY);

    /* reap ghosts first: a client that closed while the channel was quiet
     * still occupies a slot (send-side reaping never ran) and would refuse
     * this legitimate upgrade */
    for (int i = 0; server != NULL && i < WEBSOCKET_MANAGER_MAX_CLIENTS; i++)
    {
        if (ch->fds[i] >= 0 &&
            client_is_ghost(server, ch->fds[i], ch->since[i]))
        {
            ESP_LOGD(TAG, "%s: stale client reaped (fd %d)", cfg->name,
                     ch->fds[i]);
            ch->fds[i] = -1;
            ch->stats.clients--;
        }
    }

    bool accept = client_count(ch) < cfg->max_clients &&
                  client_add(ch, fd);

    if (!accept)
    {
        ch->stats.refused++;
    }

    xSemaphoreGive(s_lock);

    if (!accept)
    {
        ESP_LOGD(TAG, "%s: upgrade refused (max %u)", cfg->name,
                 (unsigned)cfg->max_clients);
        return ESP_FAIL; /* handshake rejected, socket closed */
    }

    /* httpd sends a WS frame as TWO send() calls (header, payload);
     * with Nagle the payload stalls on the peer's delayed ACK of the
     * 2-byte header (~40 ms/frame measured). Interactive channels need
     * TCP_NODELAY. */
    int on = 1;

    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

    ESP_LOGI(TAG, "%s: client connected (fd %d)", cfg->name, fd);
    return ESP_OK;
}

/* ---- the data-frame handler -------------------------------------------------------- */

static esp_err_t ws_handler(httpd_req_t *req)
{
    int idx = (int)(intptr_t)req->user_ctx;
    const wsm_channel_cfg_t *cfg = wsm_core_config(idx);
    wsm_channel_t *ch = &s_ch[idx];

    if (cfg == NULL)
    {
        return ESP_FAIL;
    }

    /* a data frame: length probe, then payload (request-scoped PSRAM) */
    httpd_ws_frame_t frame = { 0 };
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);

    if (err != ESP_OK)
    {
        return err;
    }

    if (frame.len == 0 || frame.len > WSM_FRAME_MAX)
    {
        return (frame.len == 0) ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }

    uint8_t *payload = heap_caps_malloc(frame.len,
                                        MALLOC_CAP_SPIRAM |
                                            MALLOC_CAP_8BIT);

    if (payload == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    frame.payload = payload;
    err = httpd_ws_recv_frame(req, &frame, frame.len);

    if (err == ESP_OK)
    {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        ch->stats.frames_in++;
        ch->stats.bytes_in += frame.len;

        if (ch->sub != NULL)
        {
            websocket_chunk_t chunk;
            size_t off = 0;

            while (off < frame.len)
            {
                size_t n = frame.len - off;

                if (n > WEBSOCKET_MANAGER_CHUNK_SIZE)
                {
                    n = WEBSOCKET_MANAGER_CHUNK_SIZE;
                }

                chunk.len = (uint16_t)n;
                memcpy(chunk.data, payload + off, n);

                if (xQueueSend(ch->sub, &chunk, 0) != pdTRUE)
                {
                    ch->stats.rx_drops++; /* never block the httpd worker */
                }

                off += n;
            }
        }

        xSemaphoreGive(s_lock);
    }

    free(payload);
    return err;
}

/* ---- internal API ------------------------------------------------------------------ */

void wsm_ws_reset(void)
{
    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    for (int i = 0; i < WEBSOCKET_MANAGER_MAX_CHANNELS; i++)
    {
        for (int c = 0; c < WEBSOCKET_MANAGER_MAX_CLIENTS; c++)
        {
            s_ch[i].fds[c] = -1;
        }

        s_ch[i].sub = NULL;
    }

    xSemaphoreGive(s_lock);
}

esp_err_t wsm_ws_register_routes(void)
{
    for (int i = 0; wsm_core_config(i) != NULL; i++)
    {
        const wsm_channel_cfg_t *cfg = wsm_core_config(i);

        if (!cfg->enabled)
        {
            continue;
        }

        s_uris[i] = (httpd_uri_t)
        {
            .uri = cfg->path,
            .method = HTTP_GET,
            .handler = ws_handler,
            .user_ctx = (void *)(intptr_t)i,
            .is_websocket = true,
            .ws_pre_handshake_cb = ws_pre_handshake,
        };

        esp_err_t err = http_server_manager_register_uri(&s_uris[i]);

        if (err != ESP_OK)
        {
            return err;
        }

        ESP_LOGI(TAG, "%s: channel at %s (%s, max %u clients)", cfg->name,
                 cfg->path, cfg->text_mode ? "text" : "binary",
                 (unsigned)cfg->max_clients);
    }

    return ESP_OK;
}

esp_err_t wsm_ws_subscribe(int idx, void *queue)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);

    esp_err_t err = ESP_OK;

    if (s_ch[idx].sub != NULL)
    {
        err = ESP_ERR_INVALID_STATE; /* one subscriber = one bridge */
    }
    else
    {
        s_ch[idx].sub = (QueueHandle_t)queue;
    }

    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t wsm_ws_unsubscribe(int idx, void *queue)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);

    esp_err_t err = ESP_ERR_NOT_FOUND;

    if (s_ch[idx].sub == (QueueHandle_t)queue)
    {
        s_ch[idx].sub = NULL;
        err = ESP_OK;
    }

    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t wsm_ws_send(int idx, const uint8_t *data, size_t len)
{
    const wsm_channel_cfg_t *cfg = wsm_core_config(idx);
    wsm_channel_t *ch = &s_ch[idx];
    httpd_handle_t server = http_server_manager_handle();

    if (server == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    httpd_ws_frame_t frame =
    {
        .final = true,
        .type = cfg->text_mode ? HTTPD_WS_TYPE_TEXT : HTTPD_WS_TYPE_BINARY,
        .payload = (uint8_t *)data,
        .len = len,
    };

    /* snapshot the client list, then send OUTSIDE the lock: network
     * sends under s_lock starve the RX handler at high TX rates
     * (measured: bidirectional WS throughput collapsed) */
    int fds[WEBSOCKET_MANAGER_MAX_CLIENTS];
    int64_t since[WEBSOCKET_MANAGER_MAX_CLIENTS];

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(fds, ch->fds, sizeof(fds));
    memcpy(since, ch->since, sizeof(since));
    xSemaphoreGive(s_lock);

    bool failed[WEBSOCKET_MANAGER_MAX_CLIENTS] = { false };
    int delivered = 0;

    for (int c = 0; c < WEBSOCKET_MANAGER_MAX_CLIENTS; c++)
    {
        int fd = fds[c];

        if (fd < 0)
        {
            continue;
        }

        httpd_ws_client_info_t info = httpd_ws_get_fd_info(server, fd);

        if (info != HTTPD_WS_CLIENT_WEBSOCKET &&
            !client_is_ghost(server, fd, since[c]))
        {
            continue; /* its handshake is in flight: this frame is not its */
        }

        /* reap silently-closed clients before wasting a send */
        if (info != HTTPD_WS_CLIENT_WEBSOCKET ||
            httpd_ws_send_frame_async(server, fd, &frame) != ESP_OK)
        {
            failed[c] = true;
            ESP_LOGI(TAG, "%s: client reaped (fd %d)", cfg->name, fd);
        }
        else
        {
            delivered++;
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    for (int c = 0; c < WEBSOCKET_MANAGER_MAX_CLIENTS; c++)
    {
        /* still the same client? (a reconnect may have reused the slot) */
        if (failed[c] && ch->fds[c] == fds[c])
        {
            ch->fds[c] = -1;
            ch->stats.clients--;
            ch->stats.tx_drops++;
        }
    }

    if (delivered > 0)
    {
        ch->stats.frames_out++;
        ch->stats.bytes_out += len;
    }

    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t wsm_ws_stats(int idx, void *stats_out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *(websocket_stats_t *)stats_out = s_ch[idx].stats;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
