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
 *        even when no part changed, so its time moves. The read side (the
 *        status, the restore source, the copy loader) lives in
 *        config_backup_copies_read.c since 2026-10-13.
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

static StaticTask_t      s_tcb;         /* internal: FreeRTOS object */
/* PSRAM since 2026-10-13: the filesystem runs every /data operation of a
 * PSRAM-stack caller on its own internal-stack task (filesystem.h); the
 * PSA work and the SD writes never needed internal RAM. Before that the
 * 8 KB sat in internal RAM for the life of the firmware. */
static StackType_t       s_stack[8192] EXT_RAM_BSS_ATTR;
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
    size_t  size = CB_FILE_HDR_LEN + len + CB_TAG_LEN;
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
                            buf + 5, buf + CB_FILE_HDR_LEN, size - CB_FILE_HDR_LEN,
                            &clen);

    if (err != ESP_OK)
    {
        free(buf);
        return err;
    }

    *out = buf;
    *out_len = CB_FILE_HDR_LEN + clen;
    return ESP_OK;
}

/* ---- the sweep ------------------------------------------------------------------------ */

/* The files against the manifest, once per boot (2026-10-13): a restart
 * between a part's write and the manifest's leaves a part the manifest
 * does not describe, and when the setting then moves back the HMAC
 * compare says "equal" and the stale part is served forever (the config
 * backup bench restored auto_copy=false out of such a copy). A part whose
 * file is missing, unreadable or of another CRC than recorded, or whose
 * manifest predates the CRC, reads as absent: the sweep rewrites it. */
static void verify_parts(const char *dir, cb_manifest_t *m)
{
    for (int p = 0; p < CB_PART_COUNT; p++)
    {
        if (!m->parts[p].present)
        {
            continue;
        }

        char   path[128];
        char  *raw = NULL;
        size_t len = 0;
        bool   ok = false;

        part_path(dir, (cb_part_t)p, path, sizeof(path));

        if (cb_read_file(path, &raw, &len, CB_DOC_MAX) == ESP_OK)
        {
            ok = m->parts[p].crc != 0 &&
                 cb_crc32((const uint8_t *)raw, len) == m->parts[p].crc;
            free(raw);
        }

        if (!ok)
        {
            ESP_LOGI(TAG, "copies: %s/%s does not match its manifest; "
                     "rewritten", dir, CB_PART_NAMES[p]);
            m->parts[p].present = false;
        }
    }
}

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
        verify_parts(CB_DIR_INTERNAL, &s_mf_int);
    }

    if (sd && !s_mf_sd.loaded)
    {
        (void)cb_copies_read_manifest(sd_dir, &s_mf_sd);
        verify_parts(sd_dir, &s_mf_sd);
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

        char     path[128];
        uint32_t crc = cb_crc32(sealed, sealed_len); /* the file as written */

        if (need_int)
        {
            part_path(CB_DIR_INTERNAL, (cb_part_t)p, path, sizeof(path));

            if (filesystem_write(path, sealed, sealed_len) == ESP_OK)
            {
                s_mf_int.parts[p].present = true;
                s_mf_int.parts[p].bytes = (uint32_t)plain.len;
                s_mf_int.parts[p].crc = crc;
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
                s_mf_sd.parts[p].crc = crc;
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

/* ---- lent to the read side (config_backup_copies_read.c) ------------------------- */

void cb_copies_lock(void)
{
    if (s_lock != NULL)
    {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

void cb_copies_unlock(void)
{
    if (s_lock != NULL)
    {
        xSemaphoreGive(s_lock);
    }
}

const cb_manifest_t *cb_copies_manifest(bool sd)
{
    return sd ? &s_mf_sd : &s_mf_int;
}

void cb_copies_sd_dir(char *out, size_t n)
{
    sd_dir_own(out, n);
}

void cb_copies_part_path(const char *dir, cb_part_t p, char *out, size_t n)
{
    part_path(dir, p, out, n);
}
