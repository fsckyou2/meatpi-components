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
 * @file api_http_private.h
 * @brief Internal API between api_http translation units: shared HTTP
 *        helpers, the pure (host-testable) utility layer, and the per-area
 *        route registration entry points.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- pure utilities (api_http_util.c: host-tested, no HTTP deps) ---------- */

/** Redact every string value whose key ends in "_password" to "". */
void api_util_redact(cJSON *obj);

/**
 * PUT semantics for redacted fields: any "*_password" key in @p in whose
 * value is "" is replaced by the stored value from @p stored (when present).
 */
void api_util_unredact(cJSON *in, const cJSON *stored);

/**
 * Parse "/api/settings/<name>" or "/api/settings/<name>/schema".
 * @return ESP_OK with @p name filled and @p is_schema set;
 *         ESP_ERR_INVALID_ARG on empty name / extra segments / overflow.
 */
esp_err_t api_util_settings_path(const char *uri, char *name,
                                 size_t name_len, bool *is_schema);

/** Map "none|error|warn|info|debug|verbose" to esp_log_level_t values
 *  (returned as int to keep this unit esp_log-free). */
esp_err_t api_util_level_from_str(const char *s, int *out_level);

/**
 * The ENABLED bridge (bridge_manager "bridges" array) whose side a or b
 * is @p jack (a server, channel or fixed jack name): its name, borrowed
 * from @p bridges, or NULL when nothing uses the jack. A server or
 * channel with no bridge accepts clients and discards their bytes, so
 * /api/sockets and /api/ws report this as "bridge" (2026-10-16).
 */
const char *api_util_bridge_for(const cJSON *bridges, const char *jack);

/* ---- HTTP helpers (api_http.c): httpd_req_t kept as void* here so the
 * pure unit above compiles without esp_http_server ------------------------- */

struct httpd_req;

/** Send @p obj as application/json and cJSON_Delete() it. */
esp_err_t api_send_json(struct httpd_req *req, cJSON *obj);

/** Send {"error":msg} with @p status (e.g. "400 Bad Request"). */
esp_err_t api_send_error(struct httpd_req *req, const char *status,
                         const char *msg);

/** Read the full request body (PSRAM heap, NUL-terminated). Caller frees.
 *  NULL when missing/too large/receive error. */
char *api_read_body(struct httpd_req *req, size_t max_len);

/** Schedule restart_tracker_restart(reason, source, flags) after ~1 s so the
 *  HTTP response flushes first. Idempotent per boot (first wins). */
void api_schedule_reboot(int planned_reason, int source, uint32_t flags);

/* ---- per-area registration (called by api_http_init) ----------------------- */

esp_err_t api_http_register_settings(void);
esp_err_t api_http_register_system(void); /* status + restart routes */
esp_err_t api_http_register_diag(void);   /* logs routes             */
esp_err_t api_http_register_fs(void);
esp_err_t api_http_register_datapath(void);     /* the UI file manager     */
esp_err_t api_http_register_ota(void);    /* firmware upload routes  */

/** Batch-changed flag shared between the settings PUT and submit handlers. */
void api_settings_note_changed(void);
bool api_settings_take_changed(void);

#ifdef __cplusplus
}
#endif
