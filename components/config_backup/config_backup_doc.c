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

static esp_err_t part_vehicles(cb_buf_t *b)
{
    esp_err_t err = cb_buf_puts(b, "{");
    bool      first = true;
    static const struct
    {
        const char *key;
        const char *path;
    } FILES[] =
    {
        { "index",    AP_INDEX    },
        { "live",     AP_LIVE     },
        { "std_scan", AP_STD_SCAN },
    };

    for (size_t i = 0; i < sizeof(FILES) / sizeof(FILES[0]) && err == ESP_OK;
         i++)
    {
        if (!filesystem_exists(FILES[i].path))
        {
            continue;
        }

        size_t mark = b->len;

        err = cb_buf_printf(b, "%s\"%s\":", first ? "" : ",", FILES[i].key);

        if (err == ESP_OK && !cb_append_json_file(b, FILES[i].path))
        {
            b->len = mark; /* a torn file: its member is left out */
            b->data[mark] = '\0';
            continue;
        }

        first = false;
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

            if (d->dirs[i] || nlen <= 5 ||
                strcmp(d->names[i] + nlen - 5, ".json") != 0 ||
                !cb_vehicle_key_ok(d->names[i], nlen - 5))
            {
                continue;
            }

            char   path[128];
            size_t mark = b->len;

            snprintf(path, sizeof(path), AP_TABLES_DIR "/%s", d->names[i]);
            err = cb_buf_printf(b, "%s\"%.*s\":", tfirst ? "" : ",",
                                (int)(nlen - 5), d->names[i]);

            if (err == ESP_OK && !cb_append_json_file(b, path))
            {
                b->len = mark;
                b->data[mark] = '\0';
                continue;
            }

            tfirst = false;
        }

        if (err == ESP_OK)
        {
            err = cb_buf_puts(b, "}");
        }
    }

    free(d);

    if (err == ESP_OK)
    {
        err = cb_buf_puts(b, "}");
    }

    return err;
}

static esp_err_t part_databases(cb_buf_t *b)
{
    esp_err_t err = cb_buf_puts(b, "{\"dtc_db\":");

    if (err == ESP_OK)
    {
        err = cb_append_dir_b64(b, DTC_DB_DIR);
    }

    if (err == ESP_OK)
    {
        err = cb_buf_puts(b, ",\"dbc\":");
    }

    if (err == ESP_OK)
    {
        err = cb_append_dir_b64(b, DBC_DIR);
    }

    if (err == ESP_OK)
    {
        err = cb_buf_puts(b, "}");
    }

    return err;
}

static esp_err_t part_dashboard(cb_buf_t *b)
{
    if (!filesystem_exists(DASHBOARD) || !cb_append_json_file(b, DASHBOARD))
    {
        return cb_buf_puts(b, "null");
    }

    return ESP_OK;
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

static esp_err_t part_certs(cb_buf_t *b)
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
        if (!d->dirs[i] || !cb_set_name_ok(d->names[i], strlen(d->names[i])))
        {
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

    free(d);

    if (err == ESP_OK)
    {
        err = cb_buf_puts(b, "]");
    }

    return err;
}

esp_err_t cb_doc_part(cb_part_t part, bool redact, cb_buf_t *out)
{
    switch (part)
    {
        case CB_PART_SETTINGS:  return part_settings(out, redact);
        case CB_PART_VEHICLES:  return part_vehicles(out);
        case CB_PART_DATABASES: return part_databases(out);
        case CB_PART_SCRIPTS:   return cb_append_dir_b64(out, SCRIPTS_DIR);
        case CB_PART_DASHBOARD: return part_dashboard(out);
        case CB_PART_CERTS:     return part_certs(out);
        default:                return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t cb_doc_build(uint32_t mask, bool redact, cb_buf_t *out)
{
    char      created[CB_ISO_LEN];
    esp_err_t err;

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
            err = cb_doc_part((cb_part_t)p, redact, out);
        }

        first = false;
    }

    if (err == ESP_OK)
    {
        err = cb_buf_puts(out, "}}");
    }

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

static size_t dir_bytes(const char *dir, unsigned *count, bool dirs)
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
        if (d->dirs[i] == dirs)
        {
            (*count)++;
            sum += d->sizes[i];
        }
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
                                                        false);
            cJSON_AddNumberToObject(into, "cars",
                                    json_array_len(AP_INDEX, "vehicles"));
            cJSON_AddNumberToObject(into, "pids",
                                    json_array_len(AP_LIVE, "pids"));
            cJSON_AddNumberToObject(into, "filters",
                                    json_array_len(AP_LIVE, "filters"));
            break;
        }

        case CB_PART_DATABASES:
            bytes = dir_bytes(DTC_DB_DIR, &n1, false) +
                    dir_bytes(DBC_DIR, &n2, false);
            cJSON_AddNumberToObject(into, "dtc_db", n1);
            cJSON_AddNumberToObject(into, "dbc", n2);
            break;

        case CB_PART_SCRIPTS:
            bytes = dir_bytes(SCRIPTS_DIR, &n1, false);
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

                    char     sub[128];
                    unsigned files = 0;

                    snprintf(sub, sizeof(sub), CERTS_DIR "/%s", d->names[i]);
                    bytes += dir_bytes(sub, &files, false);
                    n1++;
                }

                free(d);
            }

            cJSON_AddNumberToObject(into, "sets", n1);
            break;
        }

        default:
            return ESP_ERR_INVALID_ARG;
    }

    cJSON_AddNumberToObject(into, "bytes", (double)bytes);
    return ESP_OK;
}
