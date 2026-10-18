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
 * @file http_client_manager_private.h
 * @brief Internal contract: the PURE auth/url helpers (host-testable,
 *        no esp_http_client, no RTOS).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** "http://host..." or "https://host..." with a non-empty host. */
bool hc_url_valid(const char *url);
bool hc_url_is_tls(const char *url);

/** Standard base64 (no wrapping). Returns chars written (excl. NUL), or
 *  0 when @p out can't hold ceil(len/3)*4 + 1. */
size_t hc_base64(const uint8_t *in, size_t len, char *out, size_t cap);

/** "Bearer <token>" -> out. False when it doesn't fit. */
bool hc_auth_bearer(const char *token, char *out, size_t cap);

/** "Basic <base64(user:pass)>" -> out. False when it doesn't fit. */
bool hc_auth_basic(const char *user, const char *pass, char *out,
                   size_t cap);

/** Split a pre-formatted "Key: Value" header string in place-safe way:
 *  copies the key into @p key (cap bytes) and returns a pointer to the
 *  value inside @p header. NULL for malformed input. */
const char *hc_header_split(const char *header, char *key, size_t cap);

/** A redirect the client follows by hand (2026-10-18: the manager never
 *  calls esp_http_client_perform, so IDF's own redirect handling is
 *  inert): 301, 302, 303, 307 or 308 on a GET, while @p hops < @p max.
 *  A POST / PUT / DELETE is never redirected (the body would have to be
 *  sent again). */
bool hc_redirect_follow(int status, bool is_get, int hops, int max_hops);

#define HC_MAX_REDIRECTS 3

/* event_manager glue (http_client_manager_events.c) */
void hcm_events_register(void);
