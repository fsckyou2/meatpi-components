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
 * @file config_backup_codec.c
 * @brief PURE (no IDF): the redaction rule and its inverse (over cJSON),
 *        the key-file codec (hex), the manifest codec of a copy.
 *        Host-tested in host_test/ with config_backup_core.c.
 */
#include <stdio.h>
#include <string.h>

#include "config_backup_private.h"

/* ---- the redaction rule ------------------------------------------------------------- */

static const char *const SECRET_SUFFIXES[] =
{
    "_password", "private_key", "preshared_key", "auth_key", "_token",
    "api_key",
};

typedef struct
{
    const char *component;
    const char *key;
} secret_extra_t;

/* usernames and the addresses that carry a token: out of a file without
   sensitive data, on top of the api_http suffixes */
static const secret_extra_t SECRET_EXTRA[] =
{
    { "mqtt_manager",      "username"       },
    { "espnetlink",        "apn_user"       },
    { "ha_webhooks",       "url"            },
    { "ha_webhooks",       "url2"           },
    { "data_destinations", "url"            },
    { "data_destinations", "basic_username" },
};

bool cb_key_is_secret(const char *component, const char *key)
{
    if (key == NULL)
    {
        return false;
    }

    size_t klen = strlen(key);

    for (size_t i = 0; i < sizeof(SECRET_SUFFIXES) / sizeof(SECRET_SUFFIXES[0]);
         i++)
    {
        size_t slen = strlen(SECRET_SUFFIXES[i]);

        if (klen >= slen && strcmp(key + klen - slen, SECRET_SUFFIXES[i]) == 0)
        {
            return true;
        }
    }

    if (component == NULL)
    {
        return false;
    }

    for (size_t i = 0; i < sizeof(SECRET_EXTRA) / sizeof(SECRET_EXTRA[0]); i++)
    {
        if (strcmp(SECRET_EXTRA[i].component, component) == 0 &&
            strcmp(SECRET_EXTRA[i].key, key) == 0)
        {
            return true;
        }
    }

    return false;
}

static void redact_value(cJSON *node, const char *component)
{
    cJSON *item = NULL;

    cJSON_ArrayForEach(item, node)
    {
        if (cJSON_IsObject(item) || cJSON_IsArray(item))
        {
            redact_value(item, component);
        }
        else if (cJSON_IsString(item) && item->string != NULL &&
                 item->valuestring != NULL && item->valuestring[0] != '\0' &&
                 cb_key_is_secret(component, item->string))
        {
            cJSON_SetValuestring(item, "");
        }
    }
}

void cb_redact_settings(cJSON *settings_part)
{
    cJSON *comp = NULL;

    cJSON_ArrayForEach(comp, settings_part)
    {
        cJSON *data = cJSON_GetObjectItemCaseSensitive(comp, "data");

        if (cJSON_IsObject(data))
        {
            redact_value(data, comp->string);
        }
    }
}

static const cJSON *stored_element(const cJSON *stored_arr, const cJSON *el,
                                   int idx)
{
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(el, "name");

    if (cJSON_IsString(name))
    {
        const cJSON *s = NULL;

        cJSON_ArrayForEach(s, stored_arr)
        {
            const cJSON *sn = cJSON_GetObjectItemCaseSensitive(s, "name");

            if (cJSON_IsString(sn) &&
                strcmp(sn->valuestring, name->valuestring) == 0)
            {
                return s;
            }
        }
    }

    return cJSON_GetArrayItem((cJSON *)stored_arr, idx);
}

void cb_unredact(cJSON *data, const cJSON *stored, const char *component)
{
    cJSON *item = NULL;

    if (!cJSON_IsObject(data) || !cJSON_IsObject(stored))
    {
        return;
    }

    cJSON_ArrayForEach(item, data)
    {
        if (item->string == NULL)
        {
            continue;
        }

        const cJSON *was = cJSON_GetObjectItemCaseSensitive(stored,
                                                            item->string);

        if (cJSON_IsObject(item))
        {
            cb_unredact(item, was, component);
        }
        else if (cJSON_IsArray(item) && cJSON_IsArray(was))
        {
            int     idx = 0;
            cJSON  *el = NULL;

            cJSON_ArrayForEach(el, item)
            {
                if (cJSON_IsObject(el))
                {
                    cb_unredact(el, stored_element(was, el, idx), component);
                }

                idx++;
            }
        }
        else if (cJSON_IsString(item) && item->valuestring != NULL &&
                 item->valuestring[0] == '\0' &&
                 cb_key_is_secret(component, item->string) &&
                 cJSON_IsString(was) && was->valuestring[0] != '\0')
        {
            cJSON_SetValuestring(item, was->valuestring);
        }
    }
}

/* ---- the key file ------------------------------------------------------------------- */

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }

    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }

    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }

    return -1;
}

void cb_hex_encode(const uint8_t *in, size_t n, char *out)
{
    static const char DIGITS[] = "0123456789abcdef";

    for (size_t i = 0; i < n; i++)
    {
        out[2 * i] = DIGITS[in[i] >> 4];
        out[2 * i + 1] = DIGITS[in[i] & 0x0f];
    }

    out[2 * n] = '\0';
}

bool cb_hex_decode(const char *in, size_t n_chars, uint8_t *out)
{
    if (in == NULL || out == NULL || (n_chars % 2) != 0)
    {
        return false;
    }

    for (size_t i = 0; i < n_chars; i += 2)
    {
        int hi = hex_nibble(in[i]);
        int lo = hex_nibble(in[i + 1]);

        if (hi < 0 || lo < 0)
        {
            return false;
        }

        out[i / 2] = (uint8_t)((hi << 4) | lo);
    }

    return true;
}

bool cb_key_text_decode(const char *text, size_t len, uint8_t key[CB_KEY_LEN])
{
    if (text == NULL || key == NULL)
    {
        return false;
    }

    size_t i = 0;

    while (i < len)
    {
        /* skip whitespace and comment lines */
        if (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' ||
            text[i] == '\n')
        {
            i++;
            continue;
        }

        if (text[i] == '#')
        {
            while (i < len && text[i] != '\n')
            {
                i++;
            }

            continue;
        }

        break;
    }

    if (i + 2 * CB_KEY_LEN > len)
    {
        return false;
    }

    if (!cb_hex_decode(text + i, 2 * CB_KEY_LEN, key))
    {
        return false;
    }

    /* nothing but whitespace or comments may follow */
    i += 2 * CB_KEY_LEN;

    while (i < len)
    {
        if (text[i] == '#')
        {
            while (i < len && text[i] != '\n')
            {
                i++;
            }

            continue;
        }

        if (text[i] != ' ' && text[i] != '\t' && text[i] != '\r' &&
            text[i] != '\n')
        {
            return false;
        }

        i++;
    }

    return true;
}

/* ---- the manifest ------------------------------------------------------------------- */

cJSON *cb_manifest_to_json(const cb_manifest_t *m)
{
    cJSON *j = cJSON_CreateObject();

    if (j == NULL || m == NULL)
    {
        cJSON_Delete(j);
        return NULL;
    }

    cJSON_AddNumberToObject(j, "wican_copy", CB_COPY_VERSION);
    cJSON_AddStringToObject(j, "device_id", m->device_id);
    cJSON_AddStringToObject(j, "fw_version", m->fw);
    cJSON_AddNumberToObject(j, "refreshed", (double)m->refreshed);

    cJSON *parts = cJSON_AddObjectToObject(j, "parts");

    for (int p = 0; p < CB_PART_COUNT; p++)
    {
        if (!m->parts[p].present)
        {
            continue;
        }

        cJSON *o = cJSON_AddObjectToObject(parts, CB_PART_NAMES[p]);
        char   hex[2 * CB_HMAC_LEN + 1];

        cb_hex_encode(m->parts[p].hmac, CB_HMAC_LEN, hex);
        cJSON_AddNumberToObject(o, "bytes", (double)m->parts[p].bytes);
        cJSON_AddStringToObject(o, "hmac", hex);
    }

    return j;
}

esp_err_t cb_manifest_from_json(const cJSON *j, cb_manifest_t *m)
{
    if (!cJSON_IsObject(j) || m == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *ver = cJSON_GetObjectItemCaseSensitive(j, "wican_copy");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(j, "device_id");
    const cJSON *fw = cJSON_GetObjectItemCaseSensitive(j, "fw_version");
    const cJSON *ref = cJSON_GetObjectItemCaseSensitive(j, "refreshed");
    const cJSON *parts = cJSON_GetObjectItemCaseSensitive(j, "parts");

    if (!cJSON_IsNumber(ver) || ver->valuedouble != CB_COPY_VERSION ||
        !cJSON_IsString(id) || !cJSON_IsObject(parts))
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(m, 0, sizeof(*m));
    snprintf(m->device_id, sizeof(m->device_id), "%s", id->valuestring);
    snprintf(m->fw, sizeof(m->fw), "%s",
             cJSON_IsString(fw) ? fw->valuestring : "");
    m->refreshed = cJSON_IsNumber(ref) ? (int64_t)ref->valuedouble : 0;

    const cJSON *o = NULL;

    cJSON_ArrayForEach(o, parts)
    {
        int p = cb_part_from_name(o->string, strlen(o->string));
        const cJSON *bytes = cJSON_GetObjectItemCaseSensitive(o, "bytes");
        const cJSON *hmac = cJSON_GetObjectItemCaseSensitive(o, "hmac");

        if (p < 0 || !cJSON_IsNumber(bytes) || !cJSON_IsString(hmac) ||
            strlen(hmac->valuestring) != 2 * CB_HMAC_LEN ||
            !cb_hex_decode(hmac->valuestring, 2 * CB_HMAC_LEN,
                           m->parts[p].hmac))
        {
            continue; /* a damaged entry reads as absent: rewritten */
        }

        m->parts[p].present = true;
        m->parts[p].bytes = (uint32_t)bytes->valuedouble;
    }

    m->loaded = true;
    return ESP_OK;
}

uint32_t cb_manifest_bytes(const cb_manifest_t *m)
{
    uint32_t sum = 0;

    for (int p = 0; p < CB_PART_COUNT; p++)
    {
        if (m->parts[p].present)
        {
            sum += m->parts[p].bytes;
        }
    }

    return sum;
}

unsigned cb_manifest_count(const cb_manifest_t *m)
{
    unsigned n = 0;

    for (int p = 0; p < CB_PART_COUNT; p++)
    {
        n += m->parts[p].present ? 1u : 0u;
    }

    return n;
}
