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
 * @file config_backup_doc.c
 * @brief The plain document: the builders of the six parts (the settings
 *        export, redacted when asked; the files of /data read by path, JSON
 *        files appended verbatim after a validity scan, binaries as base64,
 *        PEM as JSON strings), the whole document, and the counts the
 *        status route reports. The buffer and the file helpers are in
 *        config_backup_files.c.
 *
 * Every function here reads the filesystem: callers run on an internal
 * stack (the httpd task, the copies worker, the console).
 */
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "filesystem.h"
#include "settings_manager.h"

#include "config_backup_private.h"

#define AP_DIR        "/data/autopid"
#define AP_INDEX      AP_DIR "/vehicles.json"
#define AP_TABLES_DIR AP_DIR "/vehicles"
#define AP_LIVE       AP_DIR "/config.json"
#define AP_STD_SCAN   AP_DIR "/std_scan.json"
#define DTC_DB_DIR    AP_DIR "/dtc_db"
#define DBC_DIR       AP_DIR "/dbc"
#define SCRIPTS_DIR   "/data/scripts"
#define DASHBOARD     "/data/dashboard.json"
#define CERTS_DIR     "/data/certs"

/* ---- the parts ----------------------------------------------------------------------- */

static esp_err_t part_settings(cb_buf_t *b, bool redact)
{
    cJSON    *root = NULL;
    esp_err_t err = settings_manager_export(&root);

    if (err != ESP_OK)
    {
        return err;
    }

    if (redact)
    {
        cb_redact_settings(root);
    }

    char *s = cJSON_PrintUnformatted(root);

    cJSON_Delete(root);

    if (s == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    err = cb_buf_puts(b, s);
    cJSON_free(s);
    return err;
}

/* why a JSON file could not be appended, as the left-out list says it */
static cb_why_t why_of(esp_err_t err)
{
    switch (err)
    {
        case ESP_ERR_INVALID_SIZE:     return CB_WHY_SIZE;
        case ESP_ERR_INVALID_RESPONSE: return CB_WHY_JSON;
        default:                       return CB_WHY_READ;
    }
}

/* a JSON file as one member; a torn, oversize or unreadable file is rolled
   back and listed under @p name (issue #989), the buffer's own failure
   stops the part */
static esp_err_t json_member(cb_buf_t *b, const char *key, size_t key_len,
                             const char *path, const char *name, bool *first,
                             cb_left_out_t *left)
{
    size_t    mark = b->len;
    esp_err_t err = cb_buf_printf(b, "%s\"%.*s\":", *first ? "" : ",",
                                  (int)key_len, key);

    if (err == ESP_OK)
    {
        err = cb_append_json_file(b, path);
    }

    if (err != ESP_OK && err != ESP_ERR_NO_MEM && !b->failed)
    {
        b->len = mark;
        b->data[mark] = '\0';
        cb_left_out_add(left, name, strlen(name), why_of(err));
        return ESP_OK;
    }

    *first = false;
    return err;
}

static esp_err_t part_vehicles(cb_buf_t *b, cb_left_out_t *left)
{
    esp_err_t err = cb_buf_puts(b, "{");
    bool      first = true;
    static const struct
    {
        const char *key;
        const char *path;
        const char *name;
    } FILES[] =
    {
        { "index",    AP_INDEX,    "vehicles.json" },
        { "live",     AP_LIVE,     "config.json"   },
        { "std_scan", AP_STD_SCAN, "std_scan.json" },
    };

    for (size_t i = 0; i < sizeof(FILES) / sizeof(FILES[0]) && err == ESP_OK;
         i++)
    {
        if (!filesystem_exists(FILES[i].path))
        {
            continue;
        }

        err = json_member(b, FILES[i].key, strlen(FILES[i].key), FILES[i].path,
                          FILES[i].name, &first, left);
    }

    /* the per-car tables */
    cb_dir_t *d = (err == ESP_OK) ? cb_dir_read(AP_TABLES_DIR) : NULL;

    if (d != NULL && d->n > 0)
    {
        err = cb_buf_printf(b, "%s\"tables\":{", first ? "" : ",");

        bool tfirst = true;

        for (size_t i = 0; i < d->n && err == ESP_OK; i++)
        {
            size_t nlen = strlen(d->names[i]);

            if (d->dirs[i])
            {
                continue;
            }

            if (nlen <= 5 || strcmp(d->names[i] + nlen - 5, ".json") != 0 ||
                !cb_vehicle_key_ok(d->names[i], nlen - 5))
            {
                cb_left_out_add(left, d->names[i], nlen, CB_WHY_NAME);
                continue;
            }

            char path[128];

            snprintf(path, sizeof(path), AP_TABLES_DIR "/%s", d->names[i]);
            err = json_member(b, d->names[i], nlen - 5, path, d->names[i],
                              &tfirst, left);
        }

        if (err == ESP_OK)
        {
            err = cb_buf_puts(b, "}");
        }
    }

    if (d != NULL)
    {
        cb_left_out_merge(left, &d->left);
    }

    free(d);

    if (err == ESP_OK)
    {
        err = cb_buf_puts(b, "}");
    }

    return err;
}

static esp_err_t part_databases(cb_buf_t *b, cb_left_out_t *left)
{
    esp_err_t err = cb_buf_puts(b, "{\"dtc_db\":");

    if (err == ESP_OK)
    {
        err = cb_append_dir_b64(b, DTC_DB_DIR, left);
    }

    if (err == ESP_OK)
    {
        err = cb_buf_puts(b, ",\"dbc\":");
    }

    if (err == ESP_OK)
    {
        err = cb_append_dir_b64(b, DBC_DIR, left);
    }

    if (err == ESP_OK)
    {
        err = cb_buf_puts(b, "}");
    }

    return err;
}

static esp_err_t part_dashboard(cb_buf_t *b, cb_left_out_t *left)
{
    if (!filesystem_exists(DASHBOARD))
    {
        return cb_buf_puts(b, "null");
    }

    size_t    mark = b->len;
    esp_err_t err = cb_append_json_file(b, DASHBOARD);

    if (err != ESP_OK && err != ESP_ERR_NO_MEM && !b->failed)
    {
        b->len = mark;
        b->data[mark] = '\0';
        cb_left_out_add(left, "dashboard.json", 14, why_of(err));
        return cb_buf_puts(b, "null");
    }

    return err;
}

static esp_err_t append_pem(cb_buf_t *b, const char *set, const char *file,
                            const char *key, bool *first)
{
    char   path[128];
    char  *pem = NULL;
    size_t len = 0;

    snprintf(path, sizeof(path), CERTS_DIR "/%s/%s", set, file);

    if (cb_read_file(path, &pem, &len, CB_PEM_MAX) != ESP_OK)
    {
        return ESP_OK; /* absent part */
    }

    esp_err_t err = cb_buf_printf(b, "%s\"%s\":", *first ? "" : ",", key);

    if (err == ESP_OK)
    {
        err = cb_json_string_append(b, pem, len);
    }

    free(pem);
    *first = false;
    return err;
}

static esp_err_t part_certs(cb_buf_t *b, cb_left_out_t *left)
{
    cb_dir_t *d = cb_dir_read(CERTS_DIR);

    if (d == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = cb_buf_puts(b, "[");
    bool      first = true;

    for (size_t i = 0; i < d->n && err == ESP_OK; i++)
    {
        if (!d->dirs[i])
        {
            continue;
        }

        if (!cb_set_name_ok(d->names[i], strlen(d->names[i])))
        {
            cb_left_out_add(left, d->names[i], strlen(d->names[i]),
                            CB_WHY_NAME);
            continue;
        }

        bool pfirst = false;

        err = cb_buf_printf(b, "%s{\"set\":\"%s\"", first ? "" : ",",
                            d->names[i]);

        if (err == ESP_OK)
        {
            err = append_pem(b, d->names[i], "ca.pem", "ca", &pfirst);
        }

        if (err == ESP_OK)
        {
            err = append_pem(b, d->names[i], "client.crt", "cert", &pfirst);
        }

        if (err == ESP_OK)
        {
            err = append_pem(b, d->names[i], "client.key", "key", &pfirst);
        }

        if (err == ESP_OK)
        {
            err = cb_buf_puts(b, "}");
        }

        first = false;
    }

    cb_left_out_merge(left, &d->left);
    free(d);

    if (err == ESP_OK)
    {
        err = cb_buf_puts(b, "]");
    }

    return err;
}

esp_err_t cb_doc_part(cb_part_t part, bool redact, cb_buf_t *out,
                      cb_left_out_t *left)
{
    switch (part)
    {
        case CB_PART_SETTINGS:  return part_settings(out, redact);
        case CB_PART_VEHICLES:  return part_vehicles(out, left);
        case CB_PART_DATABASES: return part_databases(out, left);
        case CB_PART_SCRIPTS:   return cb_append_dir_b64(out, SCRIPTS_DIR, left);
        case CB_PART_DASHBOARD: return part_dashboard(out, left);
        case CB_PART_CERTS:     return part_certs(out, left);
        default:                return ESP_ERR_INVALID_ARG;
    }
}

/* ,"left_out":{"<part>":[{"name","why"},...]} for the parts that left
   something out (issue #989); nothing when every file went in. An older
   firmware restoring the file never looks at the member. */
static esp_err_t append_left_out(cb_buf_t *out, const cb_left_out_t *left)
{
    esp_err_t err = ESP_OK;
    bool      first = true;

    for (int p = 0; p < CB_PART_COUNT && err == ESP_OK; p++)
    {
        cJSON *arr = cb_left_out_json(&left[p]);

        if (arr == NULL)
        {
            continue;
        }

        char *s = cJSON_PrintUnformatted(arr);

        cJSON_Delete(arr);

        if (s == NULL)
        {
            return ESP_ERR_NO_MEM;
        }

        err = cb_buf_printf(out, "%s\"%s\":%s",
                            first ? ",\"left_out\":{" : ",", CB_PART_NAMES[p],
                            s);
        cJSON_free(s);
        first = false;
    }

    if (err == ESP_OK && !first)
    {
        err = cb_buf_puts(out, "}");
    }

    return err;
}

esp_err_t cb_doc_build(uint32_t mask, bool redact, cb_buf_t *out)
{
    char      created[CB_ISO_LEN];
    esp_err_t err;

    /* one left-out list per part, PSRAM (about 2.4 KB), NULL = unlisted */
    cb_left_out_t *left = heap_caps_calloc(CB_PART_COUNT, sizeof(*left),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    cb_now_iso(created);
    err = cb_buf_printf(out,
                        "{\"wican_backup\":%u,\"device_id\":\"%s\","
                        "\"fw_version\":\"%s\",\"created\":\"%s\","
                        "\"redacted\":%s,\"parts\":{",
                        CB_DOC_VERSION, cb_device_id(), cb_fw_version(),
                        created, redact ? "true" : "false");

    bool first = true;

    for (int p = 0; p < CB_PART_COUNT && err == ESP_OK; p++)
    {
        if ((mask & CB_MASK(p)) == 0)
        {
            continue;
        }

        err = cb_buf_printf(out, "%s\"%s\":", first ? "" : ",",
                            CB_PART_NAMES[p]);

        if (err == ESP_OK)
        {
            err = cb_doc_part((cb_part_t)p, redact, out,
                              left != NULL ? &left[p] : NULL);
        }

        first = false;
    }

    if (err == ESP_OK)
    {
        err = cb_buf_puts(out, "}");
    }

    if (err == ESP_OK && left != NULL)
    {
        err = append_left_out(out, left);
    }

    if (err == ESP_OK)
    {
        err = cb_buf_puts(out, "}");
    }

    free(left);
    return err;
}

/* ---- the counts ------------------------------------------------------------------------- */

static unsigned json_array_len(const char *path, const char *key)
{
    char  *buf = NULL;
    size_t len = 0;

    if (cb_read_file(path, &buf, &len, CB_FILE_MAX) != ESP_OK)
    {
        return 0;
    }

    cJSON   *root = cJSON_ParseWithLength(buf, len);
    unsigned n = 0;

    if (root != NULL)
    {
        const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, key);

        n = cJSON_IsArray(arr) ? (unsigned)cJSON_GetArraySize(arr) : 0;
    }

    cJSON_Delete(root);
    free(buf);
    return n;
}

/* the files (or the folders) of a dir: count and bytes; what a backup would
   leave out of it goes into @p left (the listing's refusals, the oversize
   files), so the status route says it before any download */
static size_t dir_bytes(const char *dir, unsigned *count, bool dirs,
                        cb_left_out_t *left)
{
    cb_dir_t *d = cb_dir_read(dir);
    size_t    sum = 0;

    *count = 0;

    if (d == NULL)
    {
        return 0;
    }

    for (size_t i = 0; i < d->n; i++)
    {
        if (d->dirs[i] != dirs)
        {
            continue;
        }

        if (!dirs && d->sizes[i] > CB_FILE_MAX)
        {
            cb_left_out_add(left, d->names[i], strlen(d->names[i]),
                            CB_WHY_SIZE);
            continue;
        }

        (*count)++;
        sum += d->sizes[i];
    }

    if (!dirs)
    {
        cb_left_out_merge(left, &d->left);
    }

    free(d);
    return sum;
}

static size_t file_bytes(const char *path)
{
    size_t n = 0;

    return (filesystem_size(path, &n) == ESP_OK) ? n : 0;
}

esp_err_t cb_doc_part_status(cb_part_t part, cJSON *into)
{
    unsigned n1 = 0;
    unsigned n2 = 0;
    size_t   bytes = 0;
    /* PSRAM, not the caller's stack (the httpd task, the console) */
    cb_left_out_t *left = heap_caps_calloc(1, sizeof(*left),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    switch (part)
    {
        case CB_PART_SETTINGS:
        {
            cJSON *list = NULL;

            if (settings_manager_list(&list) == ESP_OK)
            {
                n1 = (unsigned)cJSON_GetArraySize(list);
            }

            cJSON_Delete(list);

            cb_buf_t b;

            if (cb_buf_init(&b, 8192) == ESP_OK)
            {
                if (part_settings(&b, false) == ESP_OK)
                {
                    bytes = b.len;
                }

                cb_buf_free(&b);
            }

            cJSON_AddNumberToObject(into, "groups", n1);
            break;
        }

        case CB_PART_VEHICLES:
        {
            unsigned tables = 0;

            bytes = file_bytes(AP_INDEX) + file_bytes(AP_LIVE) +
                    file_bytes(AP_STD_SCAN) + dir_bytes(AP_TABLES_DIR, &tables,
                                                        false, left);
            cJSON_AddNumberToObject(into, "cars",
                                    json_array_len(AP_INDEX, "vehicles"));
            cJSON_AddNumberToObject(into, "pids",
                                    json_array_len(AP_LIVE, "pids"));
            cJSON_AddNumberToObject(into, "filters",
                                    json_array_len(AP_LIVE, "filters"));
            break;
        }

        case CB_PART_DATABASES:
            bytes = dir_bytes(DTC_DB_DIR, &n1, false, left) +
                    dir_bytes(DBC_DIR, &n2, false, left);
            cJSON_AddNumberToObject(into, "dtc_db", n1);
            cJSON_AddNumberToObject(into, "dbc", n2);
            break;

        case CB_PART_SCRIPTS:
            bytes = dir_bytes(SCRIPTS_DIR, &n1, false, left);
            cJSON_AddNumberToObject(into, "count", n1);
            break;

        case CB_PART_DASHBOARD:
            bytes = file_bytes(DASHBOARD);
            cJSON_AddBoolToObject(into, "present", bytes > 0);
            break;

        case CB_PART_CERTS:
        {
            cb_dir_t *d = cb_dir_read(CERTS_DIR);

            if (d != NULL)
            {
                for (size_t i = 0; i < d->n; i++)
                {
                    if (!d->dirs[i])
                    {
                        continue;
                    }

                    if (!cb_set_name_ok(d->names[i], strlen(d->names[i])))
                    {
                        cb_left_out_add(left, d->names[i],
                                        strlen(d->names[i]), CB_WHY_NAME);
                        continue;
                    }

                    char     sub[128];
                    unsigned files = 0;

                    snprintf(sub, sizeof(sub), CERTS_DIR "/%s", d->names[i]);
                    bytes += dir_bytes(sub, &files, false, NULL);
                    n1++;
                }

                free(d);
            }

            cJSON_AddNumberToObject(into, "sets", n1);
            break;
        }

        default:
            free(left);
            return ESP_ERR_INVALID_ARG;
    }

    cJSON_AddNumberToObject(into, "bytes", (double)bytes);

    cJSON *lo = cb_left_out_json(left);

    if (lo != NULL)
    {
        cJSON_AddItemToObject(into, "left_out", lo);
    }

    free(left);
    return ESP_OK;
}
