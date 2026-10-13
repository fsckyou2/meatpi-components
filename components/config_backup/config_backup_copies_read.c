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
 * @file config_backup_copies_read.c
 * @brief The copies as the rest of the firmware reads them (split out of
 *        config_backup_copies.c 2026-10-13, the 700-line rule): the status
 *        route's view of both copies, the folder a restore reads from (our
 *        own, else the one other device's on the card), and the plain
 *        document assembled from a copy's sealed parts, each part checked
 *        against the manifest's CRC before it is unsealed. The writer side
 *        (the worker, the sweep, the manifests) stays in
 *        config_backup_copies.c and lends its lock, manifests and paths
 *        through the cb_copies_* accessors.
 */
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"

#include "dev_status_manager.h"
#include "filesystem.h"

#include "config_backup_private.h"

/* a card that served another WiCAN: its folder on the card, if exactly one */
typedef struct
{
    char other[CB_ID_LEN];
    int  count;
} other_t;

static esp_err_t other_cb(const char *name, bool is_dir, size_t size, void *ctx)
{
    other_t *o = ctx;

    (void)size;

    if (is_dir && strncmp(name, "wican-", 6) == 0 &&
        strcmp(name + 6, cb_device_id()) != 0 && strlen(name + 6) < CB_ID_LEN)
    {
        snprintf(o->other, sizeof(o->other), "%s", name + 6);
        o->count++;
    }

    return ESP_OK;
}

static void copy_status(cJSON *into, const cb_manifest_t *m, bool loaded)
{
    bool present = loaded && cb_manifest_count(m) > 0;
    char iso[CB_ISO_LEN];

    cb_iso_from_epoch(present ? m->refreshed : 0, iso);
    cJSON_AddBoolToObject(into, "present", present);
    cJSON_AddStringToObject(into, "refreshed", iso);
    cJSON_AddNumberToObject(into, "bytes", present ? cb_manifest_bytes(m) : 0);
    cJSON_AddNumberToObject(into, "parts", present ? cb_manifest_count(m) : 0);
    cJSON_AddStringToObject(into, "fw_version", present ? m->fw : "");
}

void cb_copies_status(cJSON *into)
{
    bool   sd = dev_status_manager_is_set(DEV_STATUS_BIT_SDCARD_MOUNTED);
    cJSON *in = cJSON_AddObjectToObject(into, "internal");
    cJSON *sdj = cJSON_AddObjectToObject(into, "sd");

    const cb_manifest_t *mi = cb_copies_manifest(false);
    const cb_manifest_t *ms = cb_copies_manifest(true);

    cJSON_AddBoolToObject(into, "auto", cb_settings_auto_copy());

    cb_copies_lock();

    copy_status(in, mi, mi->loaded);
    cJSON_AddBoolToObject(sdj, "card", sd);

    if (sd)
    {
        bool own = ms->loaded && cb_manifest_count(ms) > 0;

        copy_status(sdj, ms, ms->loaded);

        if (own)
        {
            cJSON_AddStringToObject(sdj, "device_id", ms->device_id);
        }
        else
        {
            other_t o = { .count = 0 };

            (void)filesystem_list(CB_DIR_SD_ROOT, other_cb, &o);

            if (o.count == 1)
            {
                cb_manifest_t om;
                char          dir[80];

                snprintf(dir, sizeof(dir), CB_DIR_SD_FMT, o.other);

                if (cb_copies_read_manifest(dir, &om) == ESP_OK)
                {
                    cJSON_DeleteItemFromObject(sdj, "present");
                    cJSON_DeleteItemFromObject(sdj, "refreshed");
                    cJSON_DeleteItemFromObject(sdj, "bytes");
                    cJSON_DeleteItemFromObject(sdj, "parts");
                    cJSON_DeleteItemFromObject(sdj, "fw_version");
                    copy_status(sdj, &om, true);
                    cJSON_AddStringToObject(sdj, "device_id", o.other);
                }
            }
        }

        uint64_t total = 0;
        uint64_t free_b = 0;

        if (esp_vfs_fat_info("/sd", &total, &free_b) == ESP_OK)
        {
            cJSON_AddNumberToObject(sdj, "free_bytes", (double)free_b);
        }
    }
    else
    {
        cJSON_AddBoolToObject(sdj, "present", false);
    }

    cb_copies_unlock();
}

esp_err_t cb_copies_dir(const char *where, char *out, size_t out_len,
                        char device_id[CB_ID_LEN])
{
    if (strcmp(where, "internal") == 0)
    {
        snprintf(out, out_len, "%s", CB_DIR_INTERNAL);
        snprintf(device_id, CB_ID_LEN, "%s", cb_device_id());
        return ESP_OK;
    }

    if (strcmp(where, "sd") != 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!dev_status_manager_is_set(DEV_STATUS_BIT_SDCARD_MOUNTED))
    {
        return ESP_ERR_INVALID_STATE;
    }

    char own[80];
    char path[128];

    cb_copies_sd_dir(own, sizeof(own));
    snprintf(path, sizeof(path), "%s/" CB_MANIFEST_NAME, own);

    if (filesystem_exists(path))
    {
        snprintf(out, out_len, "%s", own);
        snprintf(device_id, CB_ID_LEN, "%s", cb_device_id());
        return ESP_OK;
    }

    other_t o = { .count = 0 };

    (void)filesystem_list(CB_DIR_SD_ROOT, other_cb, &o);

    if (o.count != 1)
    {
        return ESP_ERR_NOT_FOUND;
    }

    snprintf(out, out_len, CB_DIR_SD_FMT, o.other);
    snprintf(device_id, CB_ID_LEN, "%s", o.other);
    return ESP_OK;
}

esp_err_t cb_copies_load_doc(const char *dir, const uint8_t *key,
                             const cb_manifest_t *m, cb_buf_t *doc)
{
    bool own = strcmp(m->device_id, cb_device_id()) == 0;

    if (own)
    {
        key = cb_device_key();
    }

    if (key == NULL)
    {
        return ESP_ERR_INVALID_STATE; /* locked */
    }

    char iso[CB_ISO_LEN];

    cb_iso_from_epoch(m->refreshed, iso);

    esp_err_t err = cb_buf_printf(doc,
                                  "{\"wican_backup\":%u,\"device_id\":\"%s\","
                                  "\"fw_version\":\"%s\",\"created\":\"%s\","
                                  "\"redacted\":false,\"parts\":{",
                                  CB_DOC_VERSION, m->device_id, m->fw, iso);
    bool first = true;

    for (int p = 0; p < CB_PART_COUNT && err == ESP_OK; p++)
    {
        if (!m->parts[p].present)
        {
            continue;
        }

        char   path[128];
        char  *raw = NULL;
        size_t raw_len = 0;

        cb_copies_part_path(dir, (cb_part_t)p, path, sizeof(path));
        err = cb_read_file(path, &raw, &raw_len, CB_DOC_MAX);

        if (err != ESP_OK)
        {
            break;
        }

        if (raw_len < CB_FILE_HDR_LEN + CB_TAG_LEN ||
            memcmp(raw, CB_FILE_MAGIC, 4) != 0 || raw[4] != CB_COPY_VERSION ||
            (m->parts[p].crc != 0 &&
             cb_crc32((const uint8_t *)raw, raw_len) != m->parts[p].crc))
        {
            /* a part the manifest does not describe (a restart between the
               part's write and the manifest's) is refused, never restored */
            free(raw);
            err = ESP_ERR_INVALID_CRC;
            break;
        }

        char     aad[64];
        size_t   plen = 0;
        size_t   psize = raw_len - CB_FILE_HDR_LEN - CB_TAG_LEN + 1;
        uint8_t *plain = heap_caps_malloc(psize, MALLOC_CAP_SPIRAM |
                                                     MALLOC_CAP_8BIT);

        if (plain == NULL)
        {
            free(raw);
            err = ESP_ERR_NO_MEM;
            break;
        }

        snprintf(aad, sizeof(aad), "wican_copy:%u:%s:%s", CB_COPY_VERSION,
                 m->device_id, CB_PART_NAMES[p]);
        err = cb_unseal(key, aad, (const uint8_t *)raw + 5,
                        (const uint8_t *)raw + CB_FILE_HDR_LEN,
                        raw_len - CB_FILE_HDR_LEN, plain, psize, &plen);
        free(raw);

        if (err == ESP_OK)
        {
            err = cb_buf_printf(doc, "%s\"%s\":", first ? "" : ",",
                                CB_PART_NAMES[p]);
        }

        if (err == ESP_OK)
        {
            err = cb_buf_append(doc, plain, plen);
        }

        free(plain);
        first = false;
    }

    if (err == ESP_OK)
    {
        err = cb_buf_puts(doc, "}}");
    }

    return err;
}
