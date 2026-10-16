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
 * @file config_backup_http.c
 * @brief The /api/backup routes (HTTP_API.md of this component). The
 *        api_http helpers are private to api_http, so the small send and
 *        read helpers live here. Every handler runs on the server's
 *        internal stack, so the filesystem may be read and written.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "http_server_manager.h"

#include "config_backup_private.h"

static const char *TAG = "config_backup";

#define CHUNK       4096
#define B64_BLOCK   3072 /* a multiple of 3 */

/* ---- helpers --------------------------------------------------------------------------- */

static esp_err_t send_json(httpd_req_t *req, cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);

    cJSON_Delete(obj);

    if (s == NULL)
    {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"out of memory\"}");
    }

    httpd_resp_set_type(req, "application/json");

    esp_err_t err = httpd_resp_sendstr(req, s);

    cJSON_free(s);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *status,
                            const char *msg)
{
    cJSON *o = cJSON_CreateObject();

    cJSON_AddStringToObject(o, "error", msg);
    httpd_resp_set_status(req, status);
    return send_json(req, o);
}

/* the whole body into PSRAM, NUL-terminated; NULL when empty or too big */
static char *read_body(httpd_req_t *req, size_t max, size_t *out_len)
{
    size_t total = req->content_len;

    if (total == 0 || total > max)
    {
        return NULL;
    }

    char *buf = heap_caps_malloc(total + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (buf == NULL)
    {
        return NULL;
    }

    size_t got = 0;
    int    retries = 0;

    while (got < total)
    {
        size_t want = total - got;

        if (want > CHUNK)
        {
            want = CHUNK;
        }

        int r = httpd_req_recv(req, buf + got, want);

        if (r == HTTPD_SOCK_ERR_TIMEOUT && retries++ < 5)
        {
            continue;
        }

        if (r <= 0)
        {
            free(buf);
            return NULL;
        }

        got += (size_t)r;
    }

    buf[got] = '\0';
    *out_len = got;
    return buf;
}

static bool query_get(httpd_req_t *req, const char *key, char *out,
                      size_t out_len)
{
    char query[256];

    out[0] = '\0';

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
    {
        return false;
    }

    return httpd_query_key_value(query, key, out, out_len) == ESP_OK;
}

static uint32_t parse_mask(const char *list, uint32_t allowed, bool *bad)
{
    uint32_t mask = 0;

    *bad = false;

    while (*list != '\0')
    {
        const char *end = strchr(list, ',');
        size_t      len = end ? (size_t)(end - list) : strlen(list);
        int         p = cb_part_from_name(list, len);

        if (len == 0)
        {
            /* an empty item: ignore */
        }
        else if (p < 0 || (allowed & CB_MASK(p)) == 0)
        {
            *bad = true;
        }
        else
        {
            mask |= CB_MASK(p);
        }

        list += len + (end ? 1 : 0);
    }

    return mask;
}

/* ---- GET /api/backup/status ---------------------------------------------------------------- */

static esp_err_t status_handler(httpd_req_t *req)
{
    cJSON *j = cJSON_CreateObject();
    char   iso[CB_ISO_LEN];

    cJSON_AddStringToObject(j, "device_id", cb_device_id());
    cJSON_AddStringToObject(j, "fw_version", cb_fw_version());
    cJSON_AddBoolToObject(j, "auto_copy", cb_settings_auto_copy());

    const cb_key_info_t *k = cb_key_info();
    cJSON               *key = cJSON_AddObjectToObject(j, "key");

    cb_iso_from_epoch(k->created, iso);
    cJSON_AddStringToObject(key, "created", iso);
    cJSON_AddBoolToObject(key, "downloaded", k->downloaded);
    cb_iso_from_epoch(k->downloaded_at, iso);
    cJSON_AddStringToObject(key, "downloaded_at", iso);

    cJSON *parts = cJSON_AddObjectToObject(j, "parts");

    for (int p = 0; p < CB_PART_COUNT; p++)
    {
        cJSON *o = cJSON_AddObjectToObject(parts, CB_PART_NAMES[p]);

        (void)cb_doc_part_status((cb_part_t)p, o);
    }

    cJSON *ex = cJSON_AddArrayToObject(j, "excluded");

    cJSON_AddItemToArray(ex, cJSON_CreateString("logs on the SD card"));
    cJSON_AddItemToArray(ex, cJSON_CreateString(
                                 "the restart history and the fault log"));
    cJSON_AddItemToArray(ex, cJSON_CreateString(
                                 "the web UI extras cache (downloaded again "
                                 "when a page needs it)"));
    cJSON_AddItemToArray(ex, cJSON_CreateString("the firmware itself"));

    cJSON *copies = cJSON_AddObjectToObject(j, "copies");

    cb_copies_status(copies);
    cJSON_AddBoolToObject(j, "staged", cb_restore_staged());
    return send_json(req, j);
}

/* ---- GET /api/backup?include=&redact=&encrypt= -------------------------------------------- */

static esp_err_t send_b64(httpd_req_t *req, const uint8_t *data, size_t len,
                          char *enc)
{
    for (size_t off = 0; off < len; off += B64_BLOCK)
    {
        size_t n = len - off;

        if (n > B64_BLOCK)
        {
            n = B64_BLOCK;
        }

        size_t olen = 0;

        if (cb_b64_encode(data + off, n, enc, cb_b64_size(B64_BLOCK), &olen) !=
            ESP_OK)
        {
            return ESP_FAIL;
        }

        esp_err_t err = httpd_resp_send_chunk(req, enc, olen);

        if (err != ESP_OK)
        {
            return err;
        }
    }

    return ESP_OK;
}

static esp_err_t download_handler(httpd_req_t *req)
{
    char     q[160];
    bool     bad = false;
    uint32_t mask = CB_MASK_FILE;

    if (query_get(req, "include", q, sizeof(q)) && q[0] != '\0')
    {
        mask = parse_mask(q, CB_MASK_FILE, &bad);
    }

    if (bad || mask == 0)
    {
        return send_error(req, "400 Bad Request",
                          bad ? "unknown part (certificates never leave the "
                                "device)"
                              : "nothing to include");
    }

    bool redact = query_get(req, "redact", q, sizeof(q)) && q[0] == '1';
    bool encrypt = query_get(req, "encrypt", q, sizeof(q)) && q[0] == '1';

    if (encrypt && !cb_key_info()->downloaded)
    {
        return send_error(req, "409 Conflict", "download the key first");
    }

    cb_buf_t doc;

    if (cb_buf_init(&doc, 32768) != ESP_OK)
    {
        return send_error(req, "500 Internal Server Error", "out of memory");
    }

    esp_err_t err = cb_doc_build(mask, redact, &doc);

    if (err != ESP_OK)
    {
        cb_buf_free(&doc);
        ESP_LOGW(TAG, "download: the document could not be built (%s)",
                 esp_err_to_name(err));
        return send_error(req, "500 Internal Server Error",
                          err == ESP_ERR_INVALID_SIZE ? "backup too large"
                                                      : "could not build");
    }

    char created[CB_ISO_LEN];
    char disp[96];

    cb_now_iso(created);

    if (created[0] == '\0')
    {
        snprintf(created, sizeof(created), "undated");
    }

    snprintf(disp, sizeof(disp), "attachment; filename=\"wican-%s-%.10s%s.json\"",
             cb_device_id(), created, encrypt ? "-encrypted" : "");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    if (!encrypt)
    {
        for (size_t off = 0; off < doc.len && err == ESP_OK; off += CHUNK)
        {
            size_t n = doc.len - off;

            err = httpd_resp_send_chunk(req, doc.data + off,
                                        n > CHUNK ? CHUNK : n);
        }

        cb_buf_free(&doc);
        return httpd_resp_send_chunk(req, NULL, 0);
    }

    /* seal the whole document, then the envelope with the ciphertext as base64 */
    cb_member_t m;
    char        doc_created[CB_ISO_LEN] = "";

    if (cb_json_find(doc.data, doc.len, "created", &m))
    {
        (void)cb_json_string(&m, doc_created, sizeof(doc_created));
    }

    char     aad[96];
    size_t   csize = doc.len + CB_TAG_LEN;
    uint8_t *ct = heap_caps_malloc(csize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char    *enc = heap_caps_malloc(cb_b64_size(B64_BLOCK),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t  nonce[CB_NONCE_LEN];
    size_t   clen = 0;

    snprintf(aad, sizeof(aad), "wican_backup:%u:%s:%s", CB_DOC_VERSION,
             cb_device_id(), doc_created);

    if (ct == NULL || enc == NULL ||
        cb_seal(cb_device_key(), aad, (const uint8_t *)doc.data, doc.len, nonce,
                ct, csize, &clen) != ESP_OK)
    {
        free(ct);
        free(enc);
        cb_buf_free(&doc);
        return send_error(req, "500 Internal Server Error", "could not seal");
    }

    cb_buf_free(&doc);

    char nonce_b64[32];
    char head[256];

    (void)cb_b64_encode(nonce, CB_NONCE_LEN, nonce_b64, sizeof(nonce_b64), NULL);
    /* `redacted` rides the wrapper too (issue #986) so the preview can say
       so before the unlock; the sealed document's own flag rules */
    snprintf(head, sizeof(head),
             "{\"wican_backup\":%u,\"encrypted\":true,\"redacted\":%s,"
             "\"device_id\":\"%s\",\"fw_version\":\"%s\",\"created\":\"%s\","
             "\"cipher\":\"aes-256-gcm\",\"nonce\":\"%s\",\"data\":\"",
             CB_DOC_VERSION, redact ? "true" : "false", cb_device_id(),
             cb_fw_version(), doc_created, nonce_b64);
    err = httpd_resp_send_chunk(req, head, strlen(head));

    if (err == ESP_OK)
    {
        err = send_b64(req, ct, clen, enc);
    }

    free(ct);
    free(enc);

    if (err == ESP_OK)
    {
        err = httpd_resp_send_chunk(req, "\"}", 2);
    }

    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ---- GET /api/backup/key ------------------------------------------------------------------ */

static esp_err_t key_handler(httpd_req_t *req)
{
    const uint8_t *key = cb_device_key();

    if (key == NULL)
    {
        return send_error(req, "503 Service Unavailable", "no key yet");
    }

    char hex[2 * CB_KEY_LEN + 1];
    char iso[CB_ISO_LEN];
    char body[256];
    char disp[96];

    cb_hex_encode(key, CB_KEY_LEN, hex);
    cb_iso_from_epoch(cb_key_info()->created, iso);
    snprintf(body, sizeof(body),
             "# WiCAN backup key, device %s, made %.10s.\n"
             "# Keep this file apart from the backups.\n%s\n",
             cb_device_id(), iso[0] ? iso : "undated", hex);
    snprintf(disp, sizeof(disp), "attachment; filename=\"wican-%s-backup-key.txt\"",
             cb_device_id());
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    esp_err_t err = httpd_resp_sendstr(req, body);

    if (err == ESP_OK && cb_key_mark_downloaded() != ESP_OK)
    {
        ESP_LOGW(TAG, "the key's downloaded flag could not be stored");
    }

    return err;
}

/* ---- the copies --------------------------------------------------------------------------- */

static esp_err_t copies_refresh_handler(httpd_req_t *req)
{
    cJSON    *j = cJSON_CreateObject();
    cJSON    *written = cJSON_AddArrayToObject(j, "written");
    esp_err_t err = cb_copies_refresh(CB_REFRESH_WAIT_MS, written);

    if (err != ESP_OK)
    {
        cJSON_Delete(j);
        return send_error(req, err == ESP_ERR_TIMEOUT ? "504 Gateway Timeout"
                                                      : "503 Service Unavailable",
                          err == ESP_ERR_TIMEOUT ? "the refresh is still running"
                                                 : "the copies are not running");
    }

    cJSON_AddBoolToObject(j, "ok", true);
    return send_json(req, j);
}

static esp_err_t copies_get_handler(httpd_req_t *req)
{
    const char *where = strrchr(req->uri, '/');
    char        name[12];

    if (where == NULL)
    {
        return send_error(req, "404 Not Found", "not found");
    }

    snprintf(name, sizeof(name), "%s", where + 1);

    char *qm = strchr(name, '?');

    if (qm != NULL)
    {
        *qm = '\0';
    }

    esp_err_t err = cb_restore_stage_copy(name);

    if (err == ESP_ERR_INVALID_ARG)
    {
        return send_error(req, "404 Not Found", "not found");
    }

    if (err == ESP_ERR_INVALID_STATE)
    {
        return send_error(req, "409 Conflict", "no SD card");
    }

    if (err == ESP_ERR_NOT_FOUND)
    {
        return send_error(req, "404 Not Found", "no copy there");
    }

    cJSON *m = cb_restore_manifest();

    if (m == NULL)
    {
        return send_error(req, "500 Internal Server Error", "could not stage");
    }

    return send_json(req, m);
}

/* ---- the staged restore ----------------------------------------------------------------- */

static esp_err_t inspect_handler(httpd_req_t *req)
{
    size_t len = 0;
    char  *raw = read_body(req, CB_DOC_MAX, &len);

    if (raw == NULL)
    {
        return send_error(req, "400 Bad Request",
                          req->content_len > CB_DOC_MAX ? "backup too large"
                                                        : "missing body");
    }

    char name[64];

    if (!query_get(req, "name", name, sizeof(name)))
    {
        name[0] = '\0';
    }

    esp_err_t err = cb_restore_stage_file(raw, len, name[0] ? name : NULL);

    if (err == ESP_ERR_INVALID_ARG)
    {
        return send_error(req, "400 Bad Request", "not a WiCAN backup");
    }

    cJSON *m = cb_restore_manifest();

    if (m == NULL)
    {
        return send_error(req, "500 Internal Server Error", "could not stage");
    }

    return send_json(req, m);
}

static esp_err_t unlock_handler(httpd_req_t *req)
{
    size_t len = 0;
    char  *body = read_body(req, 2048, &len);

    if (body == NULL)
    {
        return send_error(req, "400 Bad Request", "missing body");
    }

    cJSON       *in = cJSON_ParseWithLength(body, len);
    const cJSON *k = cJSON_GetObjectItemCaseSensitive(in, "key");
    uint8_t      key[CB_KEY_LEN];
    bool         ok = cJSON_IsString(k) &&
                      cb_key_text_decode(k->valuestring, strlen(k->valuestring),
                                         key);

    cJSON_Delete(in);
    free(body);

    if (!ok)
    {
        return send_error(req, "400 Bad Request", "not a key file");
    }

    esp_err_t err = cb_restore_unlock(key);

    memset(key, 0, sizeof(key));

    if (err == ESP_ERR_INVALID_STATE)
    {
        return send_error(req, "409 Conflict", "nothing staged");
    }

    if (err == ESP_ERR_INVALID_CRC)
    {
        return send_error(req, "400 Bad Request", "wrong key for this backup");
    }

    cJSON *m = cb_restore_manifest();

    if (m == NULL)
    {
        return send_error(req, "500 Internal Server Error", "could not stage");
    }

    return send_json(req, m);
}

static esp_err_t restore_handler(httpd_req_t *req)
{
    size_t len = 0;
    char  *body = read_body(req, 2048, &len);
    cJSON *in = body ? cJSON_ParseWithLength(body, len) : NULL;

    free(body);

    if (!cJSON_IsObject(in))
    {
        cJSON_Delete(in);
        return send_error(req, "400 Bad Request", "missing body");
    }

    const cJSON *inc = cJSON_GetObjectItemCaseSensitive(in, "include");
    uint32_t     mask = 0;

    if (cJSON_IsArray(inc))
    {
        const cJSON *s = NULL;

        cJSON_ArrayForEach(s, inc)
        {
            int p = cJSON_IsString(s) ? cb_part_from_name(s->valuestring,
                                                         strlen(s->valuestring))
                                      : -1;

            if (p >= 0)
            {
                mask |= CB_MASK(p);
            }
        }
    }
    else
    {
        mask = CB_MASK_ALL;
    }

    const cJSON *ki = cJSON_GetObjectItemCaseSensitive(in, "keep_identity");
    const cJSON *kv = cJSON_GetObjectItemCaseSensitive(in, "keep_vpn_keys");
    bool         keep_identity = !cJSON_IsFalse(ki);
    bool         keep_vpn = cJSON_IsTrue(kv);

    cJSON_Delete(in);

    cJSON    *resp = cJSON_CreateObject();
    char      err[CB_ERR_LEN] = "";
    esp_err_t r = cb_restore_apply(mask, keep_identity, keep_vpn, resp, err,
                                   sizeof(err));

    if (r != ESP_OK)
    {
        cJSON_Delete(resp);
        return send_error(req, r == ESP_ERR_INVALID_STATE ? "409 Conflict"
                                                          : "400 Bad Request",
                          err[0] ? err : esp_err_to_name(r));
    }

    return send_json(req, resp);
}

static esp_err_t staged_delete_handler(httpd_req_t *req)
{
    cb_restore_drop();

    cJSON *j = cJSON_CreateObject();

    cJSON_AddBoolToObject(j, "ok", true);
    return send_json(req, j);
}

/* ---- registration --------------------------------------------------------------------- */

esp_err_t config_backup_register_http(void)
{
    /* exact URIs first: the wildcard matcher takes the first hit */
    static const httpd_uri_t URIS[] =
    {
        { .uri = "/api/backup/status", .method = HTTP_GET,
          .handler = status_handler },
        { .uri = "/api/backup/key", .method = HTTP_GET,
          .handler = key_handler },
        { .uri = "/api/backup/copies/refresh", .method = HTTP_POST,
          .handler = copies_refresh_handler },
        { .uri = "/api/backup/copies/*", .method = HTTP_GET,
          .handler = copies_get_handler },
        { .uri = "/api/backup/inspect", .method = HTTP_POST,
          .handler = inspect_handler },
        { .uri = "/api/backup/unlock", .method = HTTP_POST,
          .handler = unlock_handler },
        { .uri = "/api/backup/restore", .method = HTTP_POST,
          .handler = restore_handler },
        { .uri = "/api/backup/staged", .method = HTTP_DELETE,
          .handler = staged_delete_handler },
        { .uri = "/api/backup", .method = HTTP_GET,
          .handler = download_handler },
    };

    esp_err_t err = http_server_manager_register_handlers(
        URIS, sizeof(URIS) / sizeof(URIS[0]));

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "/api/backup registered");
    }

    return err;
}
