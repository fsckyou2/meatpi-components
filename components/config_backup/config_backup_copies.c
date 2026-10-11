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
 * @file config_backup_copies.c
 * @brief The copies the device keeps for itself: a folder of sealed parts
 *        with a readable manifest on the internal flash
 *        (/data/backups/latest) and on the SD card
 *        (/sd/backups/wican-<id>/latest). One worker on an internal stack
 *        sweeps 20 s after boot, 5 s after a touch, every 2 min otherwise:
 *        every part is built and HMAC'd, only a part whose HMAC differs
 *        from the manifest is sealed and written (standard section 11: a
 *        steady state writes nothing). A manual refresh writes the manifest
 *        even when no part changed, so its time moves.
 */
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "dev_status_manager.h"
#include "filesystem.h"

#include "config_backup_private.h"

static const char *TAG = "config_backup";

#define FILE_HDR_LEN (4 + 1 + CB_NONCE_LEN) /* magic, format, nonce */

static StaticTask_t      s_tcb;
static StackType_t       s_stack[8192]; /* internal: filesystem + PSA (§2) */
static TaskHandle_t      s_task;
static SemaphoreHandle_t s_done;
static StaticSemaphore_t s_done_buf;    /* internal: FreeRTOS object */
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;    /* internal: FreeRTOS object */
static cb_manifest_t     s_mf_int EXT_RAM_BSS_ATTR;
static cb_manifest_t     s_mf_sd EXT_RAM_BSS_ATTR;
static volatile bool     s_force;
static volatile bool     s_touched;
static volatile bool     s_stop;
static bool              s_started;

/* ---- the folders --------------------------------------------------------------------- */

static void sd_dir_own(char *out, size_t n)
{
    snprintf(out, n, CB_DIR_SD_FMT, cb_device_id());
}

static void part_path(const char *dir, cb_part_t p, char *out, size_t n)
{
    snprintf(out, n, "%s/%s" CB_PART_EXT, dir, CB_PART_NAMES[p]);
}

esp_err_t cb_copies_read_manifest(const char *dir, cb_manifest_t *m)
{
    char   path[128];
    char  *buf = NULL;
    size_t len = 0;

    memset(m, 0, sizeof(*m));
    snprintf(path, sizeof(path), "%s/" CB_MANIFEST_NAME, dir);

    esp_err_t err = cb_read_file(path, &buf, &len, 8192);

    if (err != ESP_OK)
    {
        m->loaded = true; /* known absent (or unreadable: rewritten) */
        return err;
    }

    cJSON *j = cJSON_ParseWithLength(buf, len);

    err = cb_manifest_from_json(j, m);
    cJSON_Delete(j);
    free(buf);
    m->loaded = true;

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "%s: manifest unreadable, the copy is rewritten", dir);
        memset(m->parts, 0, sizeof(m->parts));
    }

    return err;
}

static esp_err_t write_manifest(const char *dir, const cb_manifest_t *m)
{
    cJSON *j = cb_manifest_to_json(m);

    if (j == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    char *s = cJSON_PrintUnformatted(j);

    cJSON_Delete(j);

    if (s == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    char path[128];

    snprintf(path, sizeof(path), "%s/" CB_MANIFEST_NAME, dir);

    esp_err_t err = filesystem_write(path, s, strlen(s));

    cJSON_free(s);
    return err;
}

/* magic + format + nonce + ciphertext||tag */
static esp_err_t seal_part(cb_part_t p, const char *dev, const char *plain,
                           size_t len, uint8_t **out, size_t *out_len)
{
    char    aad[64];
    size_t  size = FILE_HDR_LEN + len + CB_TAG_LEN;
    uint8_t *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (buf == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    snprintf(aad, sizeof(aad), "wican_copy:%u:%s:%s", CB_COPY_VERSION, dev,
             CB_PART_NAMES[p]);
    memcpy(buf, CB_FILE_MAGIC, 4);
    buf[4] = (uint8_t)CB_COPY_VERSION;

    size_t    clen = 0;
    esp_err_t err = cb_seal(cb_device_key(), aad, (const uint8_t *)plain, len,
                            buf + 5, buf + FILE_HDR_LEN, size - FILE_HDR_LEN,
                            &clen);

    if (err != ESP_OK)
    {
        free(buf);
        return err;
    }

    *out = buf;
    *out_len = FILE_HDR_LEN + clen;
    return ESP_OK;
}

/* ---- the sweep ------------------------------------------------------------------------ */

static void sweep(bool force)
{
    char  sd_dir[80];
    bool  sd = dev_status_manager_is_set(DEV_STATUS_BIT_SDCARD_MOUNTED);
    bool  wrote_int = false;
    bool  wrote_sd = false;
    const char *dev = cb_device_id();

    sd_dir_own(sd_dir, sizeof(sd_dir));
    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (!s_mf_int.loaded)
    {
        (void)cb_copies_read_manifest(CB_DIR_INTERNAL, &s_mf_int);
    }

    if (sd && !s_mf_sd.loaded)
    {
        (void)cb_copies_read_manifest(sd_dir, &s_mf_sd);
    }

    if (!sd)
    {
        s_mf_sd.loaded = false; /* a card that comes back is re-read */
    }

    cb_buf_t plain;

    if (cb_buf_init(&plain, 16384) != ESP_OK)
    {
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "copies: no memory for the sweep");
        return;
    }

    for (int p = 0; p < CB_PART_COUNT; p++)
    {
        plain.len = 0;
        plain.data[0] = '\0';

        if (cb_doc_part((cb_part_t)p, false, &plain) != ESP_OK)
        {
            ESP_LOGW(TAG, "copies: part %s could not be built", CB_PART_NAMES[p]);
            continue;
        }

        uint8_t mac[CB_HMAC_LEN];

        if (cb_hmac(cb_device_key(), plain.data, plain.len, mac) != ESP_OK)
        {
            continue;
        }

        bool need_int = !s_mf_int.parts[p].present ||
                        memcmp(mac, s_mf_int.parts[p].hmac, CB_HMAC_LEN) != 0;
        bool need_sd = sd && (!s_mf_sd.parts[p].present ||
                              memcmp(mac, s_mf_sd.parts[p].hmac,
                                     CB_HMAC_LEN) != 0);

        if (!need_int && !need_sd)
        {
            continue;
        }

        uint8_t *sealed = NULL;
        size_t   sealed_len = 0;

        if (seal_part((cb_part_t)p, dev, plain.data, plain.len, &sealed,
                      &sealed_len) != ESP_OK)
        {
            ESP_LOGW(TAG, "copies: part %s could not be sealed", CB_PART_NAMES[p]);
            continue;
        }

        char path[128];

        if (need_int)
        {
            part_path(CB_DIR_INTERNAL, (cb_part_t)p, path, sizeof(path));

            if (filesystem_write(path, sealed, sealed_len) == ESP_OK)
            {
                s_mf_int.parts[p].present = true;
                s_mf_int.parts[p].bytes = (uint32_t)plain.len;
                memcpy(s_mf_int.parts[p].hmac, mac, CB_HMAC_LEN);
                wrote_int = true;
            }
            else
            {
                ESP_LOGW(TAG, "copies: %s not written", path);
            }
        }

        if (need_sd)
        {
            part_path(sd_dir, (cb_part_t)p, path, sizeof(path));

            if (filesystem_write(path, sealed, sealed_len) == ESP_OK)
            {
                s_mf_sd.parts[p].present = true;
                s_mf_sd.parts[p].bytes = (uint32_t)plain.len;
                memcpy(s_mf_sd.parts[p].hmac, mac, CB_HMAC_LEN);
                wrote_sd = true;
            }
            else
            {
                ESP_LOGW(TAG, "copies: %s not written", path);
            }
        }

        free(sealed);
    }

    cb_buf_free(&plain);

    int64_t now = cb_now_epoch();

    if (wrote_int || force)
    {
        snprintf(s_mf_int.device_id, sizeof(s_mf_int.device_id), "%s", dev);
        snprintf(s_mf_int.fw, sizeof(s_mf_int.fw), "%s", cb_fw_version());
        s_mf_int.refreshed = now;
        (void)write_manifest(CB_DIR_INTERNAL, &s_mf_int);
    }

    if (sd && (wrote_sd || force))
    {
        snprintf(s_mf_sd.device_id, sizeof(s_mf_sd.device_id), "%s", dev);
        snprintf(s_mf_sd.fw, sizeof(s_mf_sd.fw), "%s", cb_fw_version());
        s_mf_sd.refreshed = now;
        (void)write_manifest(sd_dir, &s_mf_sd);
    }

    xSemaphoreGive(s_lock);

    if (wrote_int || wrote_sd)
    {
        ESP_LOGI(TAG, "copies refreshed (%s%s)", wrote_int ? "flash" : "",
                 wrote_sd ? (wrote_int ? " + card" : "card") : "");
    }
}

static void worker(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(CB_BOOT_DELAY_MS));

    while (!s_stop)
    {
        bool force = s_force;
        bool touched = s_touched;

        s_force = false;
        s_touched = false;

        if (touched && !force)
        {
            /* touches within the window collapse into one sweep */
            vTaskDelay(pdMS_TO_TICKS(CB_TOUCH_DELAY_MS));
            s_touched = false;
        }

        if (force || cb_settings_auto_copy())
        {
            sweep(force);
        }

        if (force)
        {
            xSemaphoreGive(s_done);
        }

        ESP_LOGD(TAG, "copies worker stack_hw=%u B",
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CB_SWEEP_PERIOD_MS));
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

/* ---- the public surface --------------------------------------------------------------- */

esp_err_t cb_copies_start(void)
{
    if (s_started)
    {
        return ESP_OK;
    }

    s_done = xSemaphoreCreateBinaryStatic(&s_done_buf);
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    s_stop = false;
    s_task = xTaskCreateStatic(worker, "cb_copies",
                               sizeof(s_stack) / sizeof(s_stack[0]), NULL, 3,
                               s_stack, &s_tcb);

    if (s_task == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    return ESP_OK;
}

void cb_copies_stop(void)
{
    if (s_task != NULL)
    {
        s_stop = true;
        xTaskNotifyGive(s_task);
    }

    s_started = false;
}

void cb_copies_touch(void)
{
    s_touched = true;

    if (s_task != NULL)
    {
        xTaskNotifyGive(s_task);
    }
}

esp_err_t cb_copies_refresh(uint32_t wait_ms, cJSON *written)
{
    if (s_task == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    /* drain a stale give, then ask */
    (void)xSemaphoreTake(s_done, 0);
    s_force = true;
    xTaskNotifyGive(s_task);

    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(wait_ms)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    if (written != NULL)
    {
        xSemaphoreTake(s_lock, portMAX_DELAY);

        if (cb_manifest_count(&s_mf_int) > 0)
        {
            cJSON_AddItemToArray(written, cJSON_CreateString("internal"));
        }

        if (cb_manifest_count(&s_mf_sd) > 0 && s_mf_sd.loaded)
        {
            cJSON_AddItemToArray(written, cJSON_CreateString("sd"));
        }

        xSemaphoreGive(s_lock);
    }

    return ESP_OK;
}

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

    cJSON_AddBoolToObject(into, "auto", cb_settings_auto_copy());

    if (s_lock != NULL)
    {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }

    copy_status(in, &s_mf_int, s_mf_int.loaded);
    cJSON_AddBoolToObject(sdj, "card", sd);

    if (sd)
    {
        bool own = s_mf_sd.loaded && cb_manifest_count(&s_mf_sd) > 0;

        copy_status(sdj, &s_mf_sd, s_mf_sd.loaded);

        if (own)
        {
            cJSON_AddStringToObject(sdj, "device_id", s_mf_sd.device_id);
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

    if (s_lock != NULL)
    {
        xSemaphoreGive(s_lock);
    }
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

    sd_dir_own(own, sizeof(own));
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

        part_path(dir, (cb_part_t)p, path, sizeof(path));
        err = cb_read_file(path, &raw, &raw_len, CB_DOC_MAX);

        if (err != ESP_OK)
        {
            break;
        }

        if (raw_len < FILE_HDR_LEN + CB_TAG_LEN ||
            memcmp(raw, CB_FILE_MAGIC, 4) != 0 || raw[4] != CB_COPY_VERSION)
        {
            free(raw);
            err = ESP_ERR_INVALID_CRC;
            break;
        }

        char     aad[64];
        size_t   plen = 0;
        size_t   psize = raw_len - FILE_HDR_LEN - CB_TAG_LEN + 1;
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
                        (const uint8_t *)raw + FILE_HDR_LEN,
                        raw_len - FILE_HDR_LEN, plain, psize, &plen);
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
