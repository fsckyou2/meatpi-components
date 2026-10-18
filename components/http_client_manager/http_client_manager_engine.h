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
 * @file http_client_manager_engine.h
 * @brief The request engine's internals, lent to the transfer files
 *        (download, upload): the per-request client, the open with
 *        redirects, the limiter and the counters. Target-only (it names
 *        esp_http_client): the host test compiles the pure helpers of
 *        http_client_manager_private.h alone.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_client.h"

#include "http_client_manager.h"

#define HC_TAG "http_client_manager"

/** The response headers a caller may want back (the stream's info). */
typedef struct
{
    char *etag;
    size_t etag_cap;
} hdr_capture_t;

/** A client for @p req: method, timeout, TLS per the cert_set > ca_pem >
 *  bundle priority, 2 KB buffers, redirects left to hc_open_and_fetch(),
 *  the Content-Type and auth headers set. NULL when IDF refuses. */
esp_http_client_handle_t hc_client_for(const http_client_request_t *req,
                                       hdr_capture_t *cap);

/** Open, send the request's body, read the headers; a GET follows up to
 *  three redirects by hand. The final status, the body length (-1 when
 *  unknown) and the hops taken come back. */
esp_err_t hc_open_and_fetch(esp_http_client_handle_t client,
                            const http_client_request_t *req,
                            int *status, int64_t *total, int *hops);

/** The concurrency limiter (4 requests device-wide). */
void hc_limiter_take(void);
void hc_limiter_give(void);

/** Count a finished transfer. */
void hc_stats_note(bool ok, size_t rx);

bool hc_inited(void);

/** The download path's own objects (http_client_manager_download.c). */
esp_err_t hc_xfer_init(void);
