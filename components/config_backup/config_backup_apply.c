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
 * @file config_backup_apply.c
 * @brief One part at a time: the dry run (counts and the first error) and
 *        the apply (settings through settings_manager_restore with the
 *        keep rules, the files written back atomically, each part replaced
 *        as a whole). The device restarts after an apply, so nothing is
 *        reloaded live.
 */
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "filesystem.h"
#include "settings_manager.h"

#include "config_backup_private.h"

static const char *TAG = "config_backup";

#define AP_DIR        "/data/autopid"
#define AP_TABLES_DIR AP_DIR "/vehicles"
#define DTC_DB_DIR    AP_DIR "/dtc_db"
#define DBC_DIR       AP_DIR "/dbc"
#define SCRIPTS_DIR   "/data/scripts"
#define DASHBOARD     "/data/dashboard.json"
#define CERTS_DIR     "/data/certs"

/* ---- settings ------------------------------------------------------------------------- */

static void copy_key(cJSON *data, const cJSON *stored, const char *key)
{
    const cJSON *was = cJSON_GetObjectItemCaseSensitive(stored, key);

    if (was == NULL)
    {
        return;
    }

    cJSON_DeleteItemFromObjectCaseSensitive(data, key);
    cJSON_AddItemToObject(data, key, cJSON_Duplicate(was, true));
}

/* the keep rules and the blank-secret rule, against what the device has */
static void merge_stored(cJSON *data, const char *name,
                         const cb_apply_opts_t *o)
{
    cJSON *stored = NULL;

    if (settings_manager_get(name, &stored) != ESP_OK)
    {
        return;
    }

    if (o->redacted)
    {
        cb_unredact(data, stored, name);
    }

    if (o->keep_identity && strcmp(name, "wifi_manager") == 0)
    {
        copy_key(data, stored, "ap_ssid");
        copy_key(data, stored, "hostname");
    }

    cJSON_Delete(stored);
}

static esp_err_t settings_walk(const cb_member_t *m, const cb_apply_opts_t *o,
                               bool dry, cJSON *info, char *err, size_t n)
{
    cJSON *root = cJSON_ParseWithLength(m->val, m->val_len);

    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        snprintf(err, n, "the settings part is not an object");
        return ESP_ERR_INVALID_ARG;
    }

    unsigned  known = 0;
    unsigned  skipped = 0;
    esp_err_t result = ESP_OK;
    cJSON    *comp = NULL;

    cJSON_ArrayForEach(comp, root)
    {
        const cJSON *ver = cJSON_GetObjectItemCaseSensitive(comp, "version");
        cJSON       *data = cJSON_GetObjectItemCaseSensitive(comp, "data");

        if (!cJSON_IsNumber(ver) || !cJSON_IsObject(data))
        {
            snprintf(err, n, "%s: needs {version, data}", comp->string);
            result = ESP_ERR_INVALID_ARG;
            break;
        }

        if (!dry && o->keep_vpn && strcmp(comp->string, "vpn_manager") == 0)
        {
            skipped++;
            continue; /* this device keeps its own tunnel identity */
        }

        merge_stored(data, comp->string, o);

        char      e[CB_ERR_LEN] = "";
        esp_err_t r = settings_manager_restore(comp->string,
                                               (uint32_t)ver->valuedouble,
                                               data, dry, e, sizeof(e), NULL);

        if (r == ESP_ERR_NOT_FOUND)
        {
            skipped++;
            continue;
        }

        if (r != ESP_OK)
        {
            snprintf(err, n, "%s: %s", comp->string,
                     e[0] ? e : esp_err_to_name(r));
            result = r;
            break;
        }

        known++;
    }

    if (info != NULL)
    {
        cJSON_AddNumberToObject(info, "groups", known);
        cJSON_AddNumberToObject(info, "skipped", skipped);
        cJSON_AddNumberToObject(info, "bytes", (double)m->val_len);
    }

    cJSON_Delete(root);
    return result;
}

/* ---- vehicles -------------------------------------------------------------------------- */

typedef struct
{
    bool     dry;
    unsigned cars;
    unsigned pids;
    unsigned filters;
    unsigned tables;
    size_t   bytes;
    char    *err;
    size_t   err_len;
} veh_ctx_t;

static unsigned array_len_in(const cb_member_t *m, const char *key)
{
    cJSON   *root = cJSON_ParseWithLength(m->val, m->val_len);
    unsigned len = 0;

    if (root != NULL)
    {
        const cJSON *a = cJSON_GetObjectItemCaseSensitive(root, key);

        len = cJSON_IsArray(a) ? (unsigned)cJSON_GetArraySize(a) : 0;
    }

    cJSON_Delete(root);
    return len;
}

static esp_err_t write_member(const char *path, const cb_member_t *m)
{
    return filesystem_write(path, m->val, m->val_len);
}

static esp_err_t tables_cb(const cb_member_t *m, void *ctx)
{
    veh_ctx_t *v = ctx;

    if (!cb_vehicle_key_ok(m->key, m->key_len))
    {
        snprintf(v->err, v->err_len, "vehicles: bad table key");
        return ESP_ERR_INVALID_ARG;
    }

    if (m->val_len > CB_FILE_MAX || !cb_json_valid(m->val, m->val_len))
    {
        snprintf(v->err, v->err_len, "vehicles: table %.*s is not valid",
                 (int)m->key_len, m->key);
        return ESP_ERR_INVALID_ARG;
    }

    v->tables++;
    v->bytes += m->val_len;

    if (v->dry)
    {
        return ESP_OK;
    }

    char path[128];

    snprintf(path, sizeof(path), AP_TABLES_DIR "/%.*s.json", (int)m->key_len,
             m->key);
    return write_member(path, m);
}

static esp_err_t vehicles_cb(const cb_member_t *m, void *ctx)
{
    veh_ctx_t  *v = ctx;
    const char *path = NULL;
    char        key[16];

    if (m->key_len >= sizeof(key))
    {
        return ESP_OK; /* not one of ours */
    }

    memcpy(key, m->key, m->key_len);
    key[m->key_len] = '\0';

    if (strcmp(key, "tables") == 0)
    {
        if (!v->dry)
        {
            (void)cb_wipe_dir_files(AP_TABLES_DIR);
        }

        return cb_json_members(m->val, m->val_len, tables_cb, v);
    }

    if (strcmp(key, "index") == 0)
    {
        path = AP_DIR "/vehicles.json";
        v->cars = array_len_in(m, "vehicles");
    }
    else if (strcmp(key, "live") == 0)
    {
        path = AP_DIR "/config.json";
        v->pids = array_len_in(m, "pids");
        v->filters = array_len_in(m, "filters");
    }
    else if (strcmp(key, "std_scan") == 0)
    {
        path = AP_DIR "/std_scan.json";
    }
    else
    {
        return ESP_OK;
    }

    if (m->val_len > CB_FILE_MAX || !cb_json_valid(m->val, m->val_len))
    {
        snprintf(v->err, v->err_len, "vehicles: %s is not valid", key);
        return ESP_ERR_INVALID_ARG;
    }

    v->bytes += m->val_len;
    return v->dry ? ESP_OK : write_member(path, m);
}

static esp_err_t vehicles_walk(const cb_member_t *m, bool dry, cJSON *info,
                               char *err, size_t n)
{
    veh_ctx_t v = { .dry = dry, .err = err, .err_len = n };
    esp_err_t r = cb_json_members(m->val, m->val_len, vehicles_cb, &v);

    if (r != ESP_OK && err[0] == '\0')
    {
        snprintf(err, n, "vehicles: not an object");
    }

    if (info != NULL)
    {
        cJSON_AddNumberToObject(info, "cars", v.cars);
        cJSON_AddNumberToObject(info, "pids", v.pids);
        cJSON_AddNumberToObject(info, "filters", v.filters);
        cJSON_AddNumberToObject(info, "bytes", (double)v.bytes);
    }

    return r;
}

/* ---- files as base64 (databases, scripts) ---------------------------------------------- */

/* [{name,size,b64}] into @p dir; @p count and @p bytes accumulate */
static esp_err_t files_walk(const cJSON *arr, const char *dir, bool dry,
                            unsigned *count, size_t *bytes, char *err,
                            size_t n)
{
    if (!cJSON_IsArray(arr))
    {
        snprintf(err, n, "%s: not a list", dir);
        return ESP_ERR_INVALID_ARG;
    }

    if (!dry)
    {
        esp_err_t w = cb_wipe_dir_files(dir);

        if (w != ESP_OK && w != ESP_ERR_NOT_FOUND)
        {
            snprintf(err, n, "%s: could not clear", dir);
            return w;
        }
    }

    const cJSON *item = NULL;

    cJSON_ArrayForEach(item, arr)
    {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        const cJSON *size = cJSON_GetObjectItemCaseSensitive(item, "size");
        const cJSON *b64 = cJSON_GetObjectItemCaseSensitive(item, "b64");

        if (!cJSON_IsString(name) || !cJSON_IsNumber(size) ||
            !cJSON_IsString(b64) ||
            !cb_name_ok(name->valuestring, strlen(name->valuestring)) ||
            size->valuedouble < 0 || size->valuedouble > CB_FILE_MAX)
        {
            snprintf(err, n, "%s: a file entry is not valid", dir);
            return ESP_ERR_INVALID_ARG;
        }

        size_t   want = (size_t)size->valuedouble;
        size_t   blen = strlen(b64->valuestring);
        size_t   cap = (blen / 4) * 3 + 3;
        uint8_t *raw = heap_caps_malloc(cap + 1, MALLOC_CAP_SPIRAM |
                                                     MALLOC_CAP_8BIT);

        if (raw == NULL)
        {
            snprintf(err, n, "out of memory");
            return ESP_ERR_NO_MEM;
        }

        size_t    got = 0;
        esp_err_t r = cb_b64_decode(b64->valuestring, blen, raw, cap, &got);

        if (r != ESP_OK || got != want)
        {
            free(raw);
            snprintf(err, n, "%s/%s: the content does not match its size", dir,
                     name->valuestring);
            return ESP_ERR_INVALID_ARG;
        }

        if (!dry)
        {
            char path[128];

            snprintf(path, sizeof(path), "%s/%s", dir, name->valuestring);
            r = filesystem_write(path, raw, got);

            if (r != ESP_OK)
            {
                free(raw);
                snprintf(err, n, "%s: write failed", path);
                return r;
            }
        }

        free(raw);
        (*count)++;
        *bytes += got;
    }

    return ESP_OK;
}

static esp_err_t databases_walk(const cb_member_t *m, bool dry, cJSON *info,
                                char *err, size_t n)
{
    cJSON *root = cJSON_ParseWithLength(m->val, m->val_len);

    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        snprintf(err, n, "databases: not an object");
        return ESP_ERR_INVALID_ARG;
    }

    unsigned  dtc = 0;
    unsigned  dbc = 0;
    size_t    bytes = 0;
    esp_err_t r = files_walk(cJSON_GetObjectItemCaseSensitive(root, "dtc_db"),
                             DTC_DB_DIR, dry, &dtc, &bytes, err, n);

    if (r == ESP_OK)
    {
        r = files_walk(cJSON_GetObjectItemCaseSensitive(root, "dbc"), DBC_DIR,
                       dry, &dbc, &bytes, err, n);
    }

    if (info != NULL)
    {
        cJSON_AddNumberToObject(info, "dtc_db", dtc);
        cJSON_AddNumberToObject(info, "dbc", dbc);
        cJSON_AddNumberToObject(info, "bytes", (double)bytes);
    }

    cJSON_Delete(root);
    return r;
}

static esp_err_t scripts_walk(const cb_member_t *m, bool dry, cJSON *info,
                              char *err, size_t n)
{
    cJSON    *root = cJSON_ParseWithLength(m->val, m->val_len);
    unsigned  count = 0;
    size_t    bytes = 0;
    esp_err_t r = files_walk(root, SCRIPTS_DIR, dry, &count, &bytes, err, n);

    if (info != NULL)
    {
        cJSON_AddNumberToObject(info, "count", count);
        cJSON_AddNumberToObject(info, "bytes", (double)bytes);
    }

    cJSON_Delete(root);
    return r;
}

/* ---- dashboard -------------------------------------------------------------------------- */

static esp_err_t dashboard_walk(const cb_member_t *m, bool dry, cJSON *info,
                                char *err, size_t n)
{
    bool present = !cb_json_is_null(m);

    if (present && (m->val_len > CB_FILE_MAX || !cb_json_valid(m->val, m->val_len)))
    {
        snprintf(err, n, "dashboard: not valid");
        return ESP_ERR_INVALID_ARG;
    }

    if (info != NULL)
    {
        cJSON_AddBoolToObject(info, "present", present);
        cJSON_AddNumberToObject(info, "bytes", present ? (double)m->val_len : 0);
    }

    if (dry)
    {
        return ESP_OK;
    }

    if (!present)
    {
        esp_err_t r = filesystem_delete(DASHBOARD);

        return (r == ESP_ERR_NOT_FOUND) ? ESP_OK : r;
    }

    return write_member(DASHBOARD, m);
}

/* ---- certificates ------------------------------------------------------------------------- */

static bool pem_ok(const cJSON *s)
{
    return s == NULL ||
           (cJSON_IsString(s) && strlen(s->valuestring) <= CB_PEM_MAX &&
            strncmp(s->valuestring, "-----BEGIN", 10) == 0);
}

static esp_err_t cert_part_write(const char *set, const char *file,
                                 const cJSON *pem)
{
    if (!cJSON_IsString(pem))
    {
        return ESP_OK;
    }

    char path[128];

    snprintf(path, sizeof(path), CERTS_DIR "/%s/%s", set, file);
    return filesystem_write(path, pem->valuestring, strlen(pem->valuestring));
}

typedef struct
{
    char names[CB_LIST_MAX][CB_NAME_MAX];
    size_t n;
} set_list_t;

static esp_err_t sets_cb(const char *name, bool is_dir, size_t size, void *ctx)
{
    set_list_t *l = ctx;

    (void)size;

    if (is_dir && l->n < CB_LIST_MAX && cb_set_name_ok(name, strlen(name)))
    {
        snprintf(l->names[l->n++], CB_NAME_MAX, "%s", name);
    }

    return ESP_OK;
}

static esp_err_t certs_wipe(void)
{
    set_list_t *l = heap_caps_calloc(1, sizeof(*l), MALLOC_CAP_SPIRAM |
                                                        MALLOC_CAP_8BIT);

    if (l == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    (void)filesystem_list(CERTS_DIR, sets_cb, l);

    for (size_t i = 0; i < l->n; i++)
    {
        char dir[128];

        snprintf(dir, sizeof(dir), CERTS_DIR "/%s", l->names[i]);
        (void)cb_wipe_dir_files(dir);
        (void)filesystem_delete(dir);
    }

    free(l);
    return ESP_OK;
}

static esp_err_t certs_walk(const cb_member_t *m, bool dry, cJSON *info,
                            char *err, size_t n)
{
    cJSON *root = cJSON_ParseWithLength(m->val, m->val_len);

    if (!cJSON_IsArray(root))
    {
        cJSON_Delete(root);
        snprintf(err, n, "certificates: not a list");
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *item = NULL;
    esp_err_t    r = ESP_OK;
    unsigned     sets = 0;
    size_t       bytes = 0;

    cJSON_ArrayForEach(item, root)
    {
        const cJSON *set = cJSON_GetObjectItemCaseSensitive(item, "set");
        const cJSON *ca = cJSON_GetObjectItemCaseSensitive(item, "ca");
        const cJSON *cert = cJSON_GetObjectItemCaseSensitive(item, "cert");
        const cJSON *key = cJSON_GetObjectItemCaseSensitive(item, "key");

        if (!cJSON_IsString(set) ||
            !cb_set_name_ok(set->valuestring, strlen(set->valuestring)) ||
            !pem_ok(ca) || !pem_ok(cert) || !pem_ok(key))
        {
            snprintf(err, n, "certificates: a set is not valid");
            r = ESP_ERR_INVALID_ARG;
            break;
        }

        sets++;
        bytes += (cJSON_IsString(ca) ? strlen(ca->valuestring) : 0) +
                 (cJSON_IsString(cert) ? strlen(cert->valuestring) : 0) +
                 (cJSON_IsString(key) ? strlen(key->valuestring) : 0);
    }

    if (r == ESP_OK && !dry)
    {
        r = certs_wipe();

        cJSON_ArrayForEach(item, root)
        {
            const cJSON *set = cJSON_GetObjectItemCaseSensitive(item, "set");

            if (r == ESP_OK)
            {
                r = cert_part_write(set->valuestring, "ca.pem",
                                    cJSON_GetObjectItemCaseSensitive(item, "ca"));
            }

            if (r == ESP_OK)
            {
                r = cert_part_write(set->valuestring, "client.crt",
                                    cJSON_GetObjectItemCaseSensitive(item,
                                                                     "cert"));
            }

            if (r == ESP_OK)
            {
                r = cert_part_write(set->valuestring, "client.key",
                                    cJSON_GetObjectItemCaseSensitive(item,
                                                                     "key"));
            }
        }

        if (r != ESP_OK)
        {
            snprintf(err, n, "certificates: write failed");
        }
    }

    if (info != NULL)
    {
        cJSON_AddNumberToObject(info, "sets", sets);
        cJSON_AddNumberToObject(info, "bytes", (double)bytes);
    }

    cJSON_Delete(root);
    return r;
}

/* ---- the two entry points -------------------------------------------------------------- */

static esp_err_t walk(cb_part_t part, const cb_member_t *m,
                      const cb_apply_opts_t *o, bool dry, cJSON *info,
                      char *err, size_t n)
{
    err[0] = '\0';

    switch (part)
    {
        case CB_PART_SETTINGS:  return settings_walk(m, o, dry, info, err, n);
        case CB_PART_VEHICLES:  return vehicles_walk(m, dry, info, err, n);
        case CB_PART_DATABASES: return databases_walk(m, dry, info, err, n);
        case CB_PART_SCRIPTS:   return scripts_walk(m, dry, info, err, n);
        case CB_PART_DASHBOARD: return dashboard_walk(m, dry, info, err, n);
        case CB_PART_CERTS:     return certs_walk(m, dry, info, err, n);
        default:
            snprintf(err, n, "unknown part");
            return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t cb_check_part(cb_part_t part, const cb_member_t *m,
                        const cb_apply_opts_t *o, cJSON *info, char *err,
                        size_t err_len)
{
    return walk(part, m, o, true, info, err, err_len);
}

esp_err_t cb_apply_part(cb_part_t part, const cb_member_t *m,
                        const cb_apply_opts_t *o, char *err, size_t err_len)
{
    esp_err_t r = walk(part, m, o, false, NULL, err, err_len);

    if (r == ESP_OK)
    {
        ESP_LOGI(TAG, "restored part %s", CB_PART_NAMES[part]);
    }

    return r;
}
