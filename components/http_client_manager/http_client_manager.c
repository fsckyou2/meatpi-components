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
 * @file http_client_manager.c
 * @brief The request engine: per-request esp_http_client instances (the
 *        thread-safety model, no shared client), the concurrency
 *        limiter, capped PSRAM responses, cert_set TLS, redirects followed
 *        by hand (2026-10-18: the manager never calls perform, so IDF's
 *        own handling is inert), the streamed GET with a sink, and the
 *        atomic download path (see include/http_client_manager.h).
 */
#include "http_client_manager.h"

#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "cert_manager.h"
#include "log_manager.h"

#include "http_client_manager_engine.h"
#include "http_client_manager_private.h"

static const char *TAG = "http_client_manager";

#define HC_DEFAULT_TIMEOUT_MS 10000
#define HC_READ_CHUNK         2048
#define HC_STREAM_CHUNK       4096
/* a GitHub release asset answers 302 with a 1.2 KB signed Location: the
   request line of the hop and the header must fit the client's buffers */
#define HC_BUF_RX             2048
#define HC_BUF_TX             2048

static SemaphoreHandle_t s_limiter;
static StaticSemaphore_t s_limiter_buf; /* internal: FreeRTOS object */
static http_client_stats_t s_stats;
static bool s_inited;

/* ---- lifecycle -------------------------------------------------------------- */

esp_err_t http_client_manager_init(void)
{
    static const log_descriptor_t LOG_DESC =
        { "http_client_manager", ESP_LOG_INFO };

    if (s_inited)
    {
        return ESP_OK;
    }

    log_manager_register(&LOG_DESC);
    s_limiter = xSemaphoreCreateCountingStatic(
        HTTP_CLIENT_MANAGER_MAX_CONCURRENT,
        HTTP_CLIENT_MANAGER_MAX_CONCURRENT, &s_limiter_buf);
    (void)hc_xfer_init();
    s_inited = true;
    hcm_events_register();
    return ESP_OK;
}

esp_err_t http_client_manager_start(void)
{
    return s_inited ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t http_client_manager_stop(void)
{
    return ESP_OK;
}

/* ---- lent to the transfer files (http_client_manager_engine.h) ---------------- */

void hc_limiter_take(void)
{
    xSemaphoreTake(s_limiter, portMAX_DELAY);
}

void hc_limiter_give(void)
{
    xSemaphoreGive(s_limiter);
}

void hc_stats_note(bool ok, size_t rx)
{
    s_stats.bytes_rx += rx;

    if (ok)
    {
        s_stats.requests++;
    }
    else
    {
        s_stats.failures++;
    }
}

bool hc_inited(void)
{
    return s_inited;
}

/* ---- request core ------------------------------------------------------------ */

static esp_http_client_method_t to_esp_method(http_client_method_t m)
{
    switch (m)
    {
        case HTTP_CLIENT_POST:   return HTTP_METHOD_POST;
        case HTTP_CLIENT_PUT:    return HTTP_METHOD_PUT;
        case HTTP_CLIENT_DELETE: return HTTP_METHOD_DELETE;
        default:                 return HTTP_METHOD_GET;
    }
}

/** TLS verification per the cert_set > ca_pem > bundle priority. */
static void configure_tls(esp_http_client_config_t *cfg,
                          const http_client_request_t *req)
{
    if (req->cert_set != NULL && req->cert_set[0] != '\0')
    {
        const char *ca;
        size_t ca_len;

        if (cert_manager_get(req->cert_set, CERT_MANAGER_CA, &ca,
                             &ca_len) == ESP_OK)
        {
            cfg->cert_pem = ca;
            cfg->cert_len = ca_len;

            const char *ccert;
            const char *ckey;
            size_t ccert_len;
            size_t ckey_len;

            if (cert_manager_get(req->cert_set, CERT_MANAGER_CLIENT_CERT,
                                 &ccert, &ccert_len) == ESP_OK &&
                cert_manager_get(req->cert_set, CERT_MANAGER_CLIENT_KEY,
                                 &ckey, &ckey_len) == ESP_OK)
            {
                cfg->client_cert_pem = ccert;
                cfg->client_cert_len = ccert_len;
                cfg->client_key_pem = ckey;
                cfg->client_key_len = ckey_len;
            }

            return;
        }

        ESP_LOGW(TAG, "cert_set '%s' unusable; using the bundle",
                 req->cert_set);
    }

    if (req->ca_pem != NULL)
    {
        cfg->cert_pem = req->ca_pem;
        cfg->cert_len = strlen(req->ca_pem) + 1;
        return;
    }

    cfg->crt_bundle_attach = esp_crt_bundle_attach;
}

static esp_err_t apply_auth_headers(esp_http_client_handle_t client,
                                    const http_client_request_t *req)
{
    char value[300];

    if (req->auth != NULL)
    {
        const http_client_auth_t *a = req->auth;

        if (a->bearer_token != NULL &&
            hc_auth_bearer(a->bearer_token, value, sizeof(value)))
        {
            esp_http_client_set_header(client, "Authorization", value);
        }

        if (a->basic_username != NULL &&
            hc_auth_basic(a->basic_username,
                          (a->basic_password != NULL) ? a->basic_password
                                                      : "",
                          value, sizeof(value)))
        {
            esp_http_client_set_header(client, "Authorization", value);
        }

        if (a->api_key != NULL)
        {
            esp_http_client_set_header(
                client,
                (a->api_key_header != NULL) ? a->api_key_header
                                            : "x-api-key",
                a->api_key);
        }
    }

    for (size_t i = 0; i < req->extra_header_count; i++)
    {
        char key[64];
        const char *val = hc_header_split(req->extra_headers[i], key,
                                          sizeof(key));

        if (val != NULL)
        {
            esp_http_client_set_header(client, key, val);
        }
    }

    return ESP_OK;
}

static esp_err_t on_http_event(esp_http_client_event_t *ev)
{
    hdr_capture_t *cap = ev->user_data;

    if (ev->event_id == HTTP_EVENT_ON_HEADER && cap != NULL &&
        cap->etag != NULL && ev->header_key != NULL &&
        ev->header_value != NULL && strcasecmp(ev->header_key, "ETag") == 0)
    {
        strlcpy(cap->etag, ev->header_value, cap->etag_cap);
    }

    return ESP_OK;
}

/**
 * Open the connection, send the body and read the headers, following
 * redirects by hand for a GET: IDF resolves a 3xx only inside
 * esp_http_client_perform(), which this engine never calls. Returns the
 * final status in @p status, the body length (-1 unknown, chunked) in
 * @p total and the hops taken in @p hops.
 */
esp_err_t hc_open_and_fetch(esp_http_client_handle_t client,
                                const http_client_request_t *req,
                                int *status, int64_t *total, int *hops)
{
    bool is_get = req->method == HTTP_CLIENT_GET;

    *hops = 0;

    for (;;)
    {
        esp_err_t err = esp_http_client_open(client,
                                             (*hops == 0) ? (int)req->body_len
                                                          : 0);

        if (err != ESP_OK)
        {
            return err;
        }

        if (*hops == 0 && req->body_len > 0)
        {
            int w = esp_http_client_write(client, req->body,
                                          (int)req->body_len);

            if (w != (int)req->body_len)
            {
                return ESP_FAIL;
            }
        }

        *total = esp_http_client_fetch_headers(client);

        if (*total < 0 && *total != -ESP_ERR_HTTP_EAGAIN)
        {
            return ESP_FAIL;
        }

        *status = esp_http_client_get_status_code(client);

        if (!hc_redirect_follow(*status, is_get, *hops, HC_MAX_REDIRECTS))
        {
            return ESP_OK;
        }

        /* the Location header was captured by the parser; it becomes the
           URL (scheme, host and port included), the connection is reopened */
        if (esp_http_client_set_redirection(client) != ESP_OK)
        {
            ESP_LOGW(TAG, "%d without a Location: not followed", *status);
            return ESP_OK;
        }

        esp_http_client_close(client);
        (*hops)++;
        ESP_LOGD(TAG, "redirect %d, hop %d", *status, *hops);
    }
}

esp_http_client_handle_t hc_client_for(const http_client_request_t *req,
                                       hdr_capture_t *cap)
{
    esp_http_client_config_t cfg =
    {
        .url = req->url,
        .method = to_esp_method(req->method),
        .timeout_ms = (req->timeout_ms > 0) ? req->timeout_ms
                                            : HC_DEFAULT_TIMEOUT_MS,
        .user_agent = "WiCAN/6",
        .skip_cert_common_name_check = req->skip_common_name,
        .disable_auto_redirect = true, /* followed by hand (open_and_fetch) */
        .buffer_size = HC_BUF_RX,
        .buffer_size_tx = HC_BUF_TX,
        .event_handler = on_http_event,
        .user_data = cap,
    };

    if (hc_url_is_tls(req->url))
    {
        configure_tls(&cfg, req);
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);

    if (client == NULL)
    {
        return NULL;
    }

    if (req->content_type != NULL)
    {
        esp_http_client_set_header(client, "Content-Type",
                                   req->content_type);
    }

    apply_auth_headers(client, req);
    return client;
}

/** Open the connection, send the body, read up to @p cap bytes into
 *  @p buf. Runs entirely on the caller's task under the limiter. */
static esp_err_t do_request(const http_client_request_t *req,
                            uint8_t *buf, size_t cap, size_t *out_len,
                            int *out_status, bool *out_truncated)
{
    esp_http_client_handle_t client = hc_client_for(req, NULL);

    if (client == NULL)
    {
        return ESP_FAIL;
    }

    int64_t total = -1;
    int hops = 0;
    esp_err_t err = hc_open_and_fetch(client, req, out_status, &total, &hops);
    size_t got = 0;

    *out_truncated = false;

    while (err == ESP_OK && got < cap)
    {
        size_t want = cap - got;

        if (want > HC_READ_CHUNK)
        {
            want = HC_READ_CHUNK;
        }

        int r = esp_http_client_read(client, (char *)buf + got, (int)want);

        if (r < 0)
        {
            err = ESP_FAIL;
            break;
        }

        if (r == 0)
        {
            break; /* body complete */
        }

        got += (size_t)r;
    }

    if (err == ESP_OK && got >= cap)
    {
        *out_truncated = !esp_http_client_is_complete_data_received(client);
    }

    esp_http_client_cleanup(client);
    *out_len = got;
    s_stats.bytes_rx += got;

    if (err == ESP_OK)
    {
        s_stats.requests++;
    }
    else
    {
        s_stats.failures++;
    }

    return err;
}

/* ---- public API --------------------------------------------------------------- */

esp_err_t http_client_manager_request(const http_client_request_t *req,
                                      http_client_response_t *out)
{
    if (req == NULL || out == NULL || !hc_url_valid(req->url))
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->status_code = -1;

    size_t cap = (req->max_response > 0) ? req->max_response
                                         : HTTP_CLIENT_MANAGER_DEFAULT_RESP;

    if (cap > HTTP_CLIENT_MANAGER_MAX_RESP || !s_inited)
    {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *buf = heap_caps_malloc(cap + 1,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (buf == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_limiter, portMAX_DELAY);

    size_t len = 0;
    int status = -1;
    bool truncated = false;
    esp_err_t err = do_request(req, buf, cap, &len, &status, &truncated);

    xSemaphoreGive(s_limiter);

    if (err != ESP_OK)
    {
        heap_caps_free(buf);
        ESP_LOGD(TAG, "%s failed: %s", req->url, esp_err_to_name(err));
        return err;
    }

    buf[len] = '\0';
    out->data = (char *)buf;
    out->len = len;
    out->status_code = status;
    out->truncated = truncated;
    return ESP_OK;
}

esp_err_t http_client_manager_get(const char *url,
                                  http_client_response_t *out)
{
    http_client_request_t req = { .url = url };

    return http_client_manager_request(&req, out);
}

esp_err_t http_client_manager_post(const char *url, const void *body,
                                   size_t body_len,
                                   const char *content_type,
                                   http_client_response_t *out)
{
    http_client_request_t req =
    {
        .url = url,
        .method = HTTP_CLIENT_POST,
        .body = body,
        .body_len = body_len,
        .content_type = content_type,
    };

    return http_client_manager_request(&req, out);
}

esp_err_t http_client_manager_stream(const http_client_request_t *req,
                                     const http_client_stream_cb_t *cb,
                                     void *ctx,
                                     http_client_stream_info_t *info)
{
    if (req == NULL || cb == NULL || cb->on_data == NULL || info == NULL ||
        !hc_url_valid(req->url) || !s_inited)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(info, 0, sizeof(*info));
    info->status_code = -1;
    info->content_length = -1;

    uint8_t *buf = heap_caps_malloc(HC_STREAM_CHUNK,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (buf == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    http_client_request_t get = *req;

    get.method = HTTP_CLIENT_GET;
    get.body = NULL;
    get.body_len = 0;

    hdr_capture_t cap = { .etag = info->etag, .etag_cap = sizeof(info->etag) };

    xSemaphoreTake(s_limiter, portMAX_DELAY);

    esp_http_client_handle_t client = hc_client_for(&get, &cap);
    esp_err_t err = (client != NULL) ? ESP_OK : ESP_FAIL;
    int64_t total = -1;

    if (err == ESP_OK)
    {
        err = hc_open_and_fetch(client, &get, &info->status_code, &total,
                             &info->hops);
    }

    if (err == ESP_OK)
    {
        info->content_length = (total >= 0) ? total : -1;

        bool want_body = info->status_code >= 200 && info->status_code < 300;

        if (cb->on_headers != NULL &&
            !cb->on_headers(info->status_code, info->content_length, ctx))
        {
            want_body = false;
            err = ESP_ERR_INVALID_RESPONSE;
        }

        while (err == ESP_OK && want_body)
        {
            int r = esp_http_client_read(client, (char *)buf, HC_STREAM_CHUNK);

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
                info->received += (size_t)r;
                err = cb->on_data(buf, (size_t)r, ctx);
            }
        }

        if (want_body)
        {
            info->complete = err == ESP_OK &&
                             esp_http_client_is_complete_data_received(client);
        }
        else
        {
            info->complete = err == ESP_OK; /* no body wanted */
        }
    }

    if (client != NULL)
    {
        esp_http_client_cleanup(client);
    }

    xSemaphoreGive(s_limiter);
    heap_caps_free(buf);
    s_stats.bytes_rx += info->received;

    if (err == ESP_OK)
    {
        s_stats.requests++;
    }
    else
    {
        s_stats.failures++;
        ESP_LOGD(TAG, "stream %s failed: %s (status %d, %u bytes)", req->url,
                 esp_err_to_name(err), info->status_code,
                 (unsigned)info->received);
    }

    return err;
}

void http_client_manager_free(http_client_response_t *resp)
{
    if (resp != NULL)
    {
        heap_caps_free(resp->data);
        resp->data = NULL;
        resp->len = 0;
    }
}

esp_err_t http_client_manager_stats(http_client_stats_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out = s_stats;
    return ESP_OK;
}
