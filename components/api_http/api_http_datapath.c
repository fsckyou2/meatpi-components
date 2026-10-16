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
 * @file api_http_datapath.c
 * @brief Data-path observability: GET /api/bridges, /api/sockets, /api/ws
 *        (API-first §1b, a user dashboard shows data-path activity
 *        without firmware access).
 *
 * The configured entries come from each component's SETTINGS (the source
 * of truth for what exists); the live counters come from the component
 * stats calls. `up:false` + no stats = configured but not running (e.g. a
 * bridge whose endpoint isn't registered in this composition, or a
 * disabled entry).
 */
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "bridge_manager.h"
#include "http_server_manager.h"
#include "settings_manager.h"
#include "socket_manager.h"
#include "websocket_manager.h"

#include "api_http_private.h"

/* Pull "<component>".<array_key> from settings; caller owns the returned
   settings root (the array is borrowed from it). */
static cJSON *settings_array(const char *component, const char *key,
                             const cJSON **array_out)
{
    cJSON *root = NULL;

    *array_out = NULL;

    if (settings_manager_get(component, &root) != ESP_OK)
    {
        return NULL;
    }

    *array_out = cJSON_GetObjectItemCaseSensitive(root, key);
    return root;
}

static const char *entry_name(const cJSON *entry)
{
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(entry, "name");

    return cJSON_IsString(name) ? name->valuestring : NULL;
}

/* Copy the config keys a dashboard needs, verbatim from settings. */
static cJSON *copy_keys(const cJSON *entry, const char *const *keys,
                        size_t count)
{
    cJSON *out = cJSON_CreateObject();

    for (size_t i = 0; out != NULL && i < count; i++)
    {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(entry, keys[i]);

        if (v != NULL)
        {
            cJSON_AddItemToObject(out, keys[i], cJSON_Duplicate(v, true));
        }
    }

    return out;
}

/* "bridge": the enabled bridge that uses this server/channel, or null.
   A jack nothing uses accepts clients and discards their bytes: the
   field names the gap a user otherwise reads as "connects, no data"
   (2026-10-16). @p bridges is the bridge_manager settings array. */
static void add_bridge(cJSON *item, const cJSON *bridges, const char *name)
{
    const char *br = (name != NULL) ? api_util_bridge_for(bridges, name)
                                    : NULL;

    if (br != NULL)
    {
        cJSON_AddStringToObject(item, "bridge", br);
    }
    else
    {
        cJSON_AddNullToObject(item, "bridge");
    }
}

static esp_err_t bridges_handler(httpd_req_t *req)
{
    static const char *const KEYS[] =
        { "name", "a", "b", "translator", "enabled" };
    const cJSON *cfg = NULL;
    cJSON *root = settings_array("bridge_manager", "bridges", &cfg);
    cJSON *resp = cJSON_CreateObject();
    cJSON *arr  = cJSON_AddArrayToObject(resp, "bridges");
    const cJSON *entry = NULL;

    cJSON_ArrayForEach(entry, cfg)
    {
        cJSON *item = copy_keys(entry, KEYS,
                                sizeof(KEYS) / sizeof(KEYS[0]));
        const char *name = entry_name(entry);
        bridge_stats_t st;

        if (item == NULL)
        {
            continue;
        }

        if (name != NULL && bridge_manager_stats(name, &st) == ESP_OK)
        {
            cJSON *s = cJSON_AddObjectToObject(item, "stats");

            cJSON_AddBoolToObject(item, "up", true);
            cJSON_AddNumberToObject(s, "a2b_chunks", st.a2b_chunks);
            cJSON_AddNumberToObject(s, "b2a_chunks", st.b2a_chunks);
            cJSON_AddNumberToObject(s, "a2b_bytes", st.a2b_bytes);
            cJSON_AddNumberToObject(s, "b2a_bytes", st.b2a_bytes);
            cJSON_AddNumberToObject(s, "send_errors", st.send_errors);
            cJSON_AddNumberToObject(s, "codec_errors", st.codec_errors);
        }
        else
        {
            cJSON_AddBoolToObject(item, "up", false);
        }

        cJSON_AddItemToArray(arr, item);
    }

    cJSON_Delete(root);
    return api_send_json(req, resp);
}

static esp_err_t sockets_handler(httpd_req_t *req)
{
    static const char *const KEYS[] =
        { "name", "proto", "port", "enabled" };
    const cJSON *cfg = NULL;
    const cJSON *bridges = NULL;
    cJSON *root = settings_array("socket_manager", "servers", &cfg);
    cJSON *broot = settings_array("bridge_manager", "bridges", &bridges);
    cJSON *resp = cJSON_CreateObject();
    cJSON *arr  = cJSON_AddArrayToObject(resp, "servers");
    const cJSON *entry = NULL;

    cJSON_ArrayForEach(entry, cfg)
    {
        cJSON *item = copy_keys(entry, KEYS,
                                sizeof(KEYS) / sizeof(KEYS[0]));
        const char *name = entry_name(entry);
        socket_stats_t st;

        if (item == NULL)
        {
            continue;
        }

        if (name != NULL && socket_manager_stats(name, &st) == ESP_OK)
        {
            cJSON *s = cJSON_AddObjectToObject(item, "stats");

            cJSON_AddBoolToObject(item, "up", true);
            cJSON_AddNumberToObject(s, "clients", st.clients);
            cJSON_AddNumberToObject(s, "bytes_in", st.bytes_in);
            cJSON_AddNumberToObject(s, "bytes_out", st.bytes_out);
            cJSON_AddNumberToObject(s, "rx_drops", st.rx_drops);
            cJSON_AddNumberToObject(s, "tx_drops", st.tx_drops);
            cJSON_AddNumberToObject(s, "reconnects", st.reconnects);
            cJSON_AddNumberToObject(s, "refused", st.refused);
        }
        else
        {
            cJSON_AddBoolToObject(item, "up", false);
        }

        add_bridge(item, bridges, name);
        cJSON_AddItemToArray(arr, item);
    }

    cJSON_Delete(broot);
    cJSON_Delete(root);
    return api_send_json(req, resp);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    static const char *const KEYS[] =
        { "name", "path", "mode", "max_clients", "enabled" };
    const cJSON *cfg = NULL;
    const cJSON *bridges = NULL;
    cJSON *root = settings_array("websocket_manager", "channels", &cfg);
    cJSON *broot = settings_array("bridge_manager", "bridges", &bridges);
    cJSON *resp = cJSON_CreateObject();
    cJSON *arr  = cJSON_AddArrayToObject(resp, "channels");
    const cJSON *entry = NULL;

    cJSON_ArrayForEach(entry, cfg)
    {
        cJSON *item = copy_keys(entry, KEYS,
                                sizeof(KEYS) / sizeof(KEYS[0]));
        const char *name = entry_name(entry);
        websocket_stats_t st;

        if (item == NULL)
        {
            continue;
        }

        if (name != NULL && websocket_manager_stats(name, &st) == ESP_OK)
        {
            cJSON *s = cJSON_AddObjectToObject(item, "stats");

            cJSON_AddBoolToObject(item, "up", true);
            cJSON_AddNumberToObject(s, "clients", st.clients);
            cJSON_AddNumberToObject(s, "frames_in", st.frames_in);
            cJSON_AddNumberToObject(s, "frames_out", st.frames_out);
            cJSON_AddNumberToObject(s, "bytes_in", st.bytes_in);
            cJSON_AddNumberToObject(s, "bytes_out", st.bytes_out);
            cJSON_AddNumberToObject(s, "rx_drops", st.rx_drops);
            cJSON_AddNumberToObject(s, "tx_drops", st.tx_drops);
            cJSON_AddNumberToObject(s, "refused", st.refused);
        }
        else
        {
            cJSON_AddBoolToObject(item, "up", false);
        }

        add_bridge(item, bridges, name);
        cJSON_AddItemToArray(arr, item);
    }

    cJSON_Delete(broot);
    cJSON_Delete(root);
    return api_send_json(req, resp);
}

esp_err_t api_http_register_datapath(void)
{
    static const httpd_uri_t URIS[] =
    {
        { .uri = "/api/bridges", .method = HTTP_GET,
          .handler = bridges_handler },
        { .uri = "/api/sockets", .method = HTTP_GET,
          .handler = sockets_handler },
        { .uri = "/api/ws", .method = HTTP_GET,
          .handler = ws_handler },
    };

    return http_server_manager_register_handlers(
        URIS, sizeof(URIS) / sizeof(URIS[0]));
}
