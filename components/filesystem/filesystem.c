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
 * @file filesystem.c
 * @brief Lifecycle and the writing operations of the WiCAN filesystem
 *        component; the reading operations live in filesystem_read.c
 *        (split 2026-10-13, the standard's 700-line rule).
 *
 * Owns the internal-flash LittleFS mount (/data). The /sd backend is
 * reserved for external_storage and reports ESP_ERR_INVALID_STATE until
 * that component exists and wires it up. Settings live in
 * filesystem_settings.c (standard §4.1).
 *
 * The stack rule (2026-10-13): every /data operation reads or writes the
 * flash with the cache off, which a task on a PSRAM stack cannot survive
 * (standard §2). An operation called from such a task runs on the stream
 * writer's internal stack instead (fs_run_internal, filesystem_stream.c),
 * synchronously, so callers no longer have to know.
 */
#include "filesystem.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_attr.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#include "log_manager.h"

#include "filesystem_private.h"

static const char *TAG = "filesystem";

/* LittleFS keeps its superblock in the dir pair {block 0, block 1}; on
 * ESP flash a block is one 4 KiB sector. Erased flash reads 0xFF. */

typedef struct
{
    bool mounted;
} backend_state_t;

typedef struct
{
    bool            inited;
    bool            started;
    backend_state_t backend[FS_BACKEND_COUNT];
} fs_state_t;

static fs_state_t s_fs EXT_RAM_BSS_ATTR; /* tiny; PSRAM by default (§2) */

static StaticSemaphore_t s_lock_buf; /* internal: FreeRTOS object */
static SemaphoreHandle_t s_lock;

/* Bounce buffer so caller data (possibly PSRAM) is never handed to the
 * flash path directly. */
static uint8_t s_scratch[2048]; /* internal: cache-off during FS write */

#define FS_LOCK()   xSemaphoreTakeRecursive(s_lock, portMAX_DELAY)
#define FS_UNLOCK() xSemaphoreGiveRecursive(s_lock)

/* ---- helpers ----------------------------------------------------------- */

static esp_err_t errno_to_esp(int e)
{
    switch (e)
    {
        case ENOENT:  return ESP_ERR_NOT_FOUND;
        case ENOMEM:  return ESP_ERR_NO_MEM;
        case ENOSPC:  return ESP_ERR_NO_MEM;
        case EINVAL:  return ESP_ERR_INVALID_ARG;
        default:      return ESP_FAIL;
    }
}

/** Validate the path and confirm its backend is usable right now. */
static esp_err_t check_path(const char *path, fs_backend_t *out_backend)
{
    fs_backend_t backend;
    esp_err_t    err = fs_path_resolve(path, &backend);

    if (err != ESP_OK)
    {
        return err;
    }

    if (!s_fs.inited || !s_fs.backend[backend].mounted)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (out_backend != NULL)
    {
        *out_backend = backend;
    }

    return ESP_OK;
}

static esp_err_t mkdirs_locked(const char *dir_path)
{
    char partial[FS_PATH_MAX];
    size_t len = strlen(dir_path);

    if (len >= sizeof(partial))
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* walk segment by segment, creating as we go; the prefix root ("/data")
     * always exists as the mount point */
    for (size_t i = 1; i <= len; i++)
    {
        if (dir_path[i] == '/' || dir_path[i] == '\0')
        {
            memcpy(partial, dir_path, i);
            partial[i] = '\0';

            if (strcmp(partial, FS_PREFIX_INTERNAL) == 0 ||
                strcmp(partial, FS_PREFIX_SD) == 0)
            {
                continue;
            }

            if (mkdir(partial, 0775) != 0 && errno != EEXIST)
            {
                ESP_LOGE(TAG, "mkdir '%s' failed: errno %d", partial, errno);
                return errno_to_esp(errno);
            }
        }
    }

    return ESP_OK;
}

/* ---- lifecycle ---------------------------------------------------------- */

esp_err_t filesystem_init(void)
{
    if (s_fs.inited)
    {
        return ESP_OK;
    }

    static const log_descriptor_t LOG_DESC = { "filesystem", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC); /* per-TAG level control (§9.2) */

    esp_err_t stream_err = fs_stream_service_init();

    if (stream_err != ESP_OK)
    {
        ESP_LOGE(TAG, "async writer service failed: %s (streamed writes "
                 "unavailable)", esp_err_to_name(stream_err));
    }

    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateRecursiveMutexStatic(&s_lock_buf);
    }

    fs_mount_prepare(CONFIG_FILESYSTEM_PARTITION_LABEL); /* filesystem_mount.c */

    esp_vfs_littlefs_conf_t conf =
    {
        .base_path              = FS_PREFIX_INTERNAL,
        .partition_label        = CONFIG_FILESYSTEM_PARTITION_LABEL,
        .format_if_mount_failed = true,
        .dont_mount             = false,
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "mount '%s' at %s failed: %s",
                 CONFIG_FILESYSTEM_PARTITION_LABEL, FS_PREFIX_INTERNAL,
                 esp_err_to_name(err));
        return err;
    }

    s_fs.backend[FS_BACKEND_INTERNAL].mounted = true;
    s_fs.inited = true;

    size_t total = 0;
    size_t used = 0;

    if (esp_littlefs_info(CONFIG_FILESYSTEM_PARTITION_LABEL,
                          &total, &used) == ESP_OK)
    {
        ESP_LOGI(TAG, "mounted %s (%u KiB used / %u KiB)",
                 FS_PREFIX_INTERNAL,
                 (unsigned)(used / 1024), (unsigned)(total / 1024));
    }

    return ESP_OK;
}

esp_err_t filesystem_start(void)
{
    if (!s_fs.inited)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_fs.started = true;
    return ESP_OK;
}

esp_err_t filesystem_stop(void)
{
    if (!s_fs.inited)
    {
        return ESP_OK;
    }

    FS_LOCK();

    esp_err_t err = esp_vfs_littlefs_unregister(
        CONFIG_FILESYSTEM_PARTITION_LABEL);

    s_fs.backend[FS_BACKEND_INTERNAL].mounted = false;
    s_fs.inited = false;
    s_fs.started = false;

    FS_UNLOCK();
    return err;
}

esp_err_t filesystem_sd_set_mounted(bool mounted)
{
    if (!s_fs.inited)
    {
        return ESP_ERR_INVALID_STATE;
    }

    FS_LOCK();
    s_fs.backend[FS_BACKEND_SD].mounted = mounted;
    FS_UNLOCK();
    ESP_LOGI(TAG, "/sd backend %s", mounted ? "enabled" : "disabled");
    return ESP_OK;
}

/* ---- the writing operations ------------------------------------------------
   Each one starts with the stack rule: a /data operation from a PSRAM
   stack is handed to the writer task (the job re-enters the same function
   there, where the rule passes). */

typedef struct
{
    const char *path;
    const void *data;
    size_t      len;
} write_args_t;

static esp_err_t write_job(void *p)
{
    write_args_t *a = p;

    return filesystem_write(a->path, a->data, a->len);
}

esp_err_t filesystem_write(const char *path, const void *data, size_t len)
{
    fs_backend_t backend;
    esp_err_t    err = check_path(path, &backend);

    if (err != ESP_OK)
    {
        return err;
    }

    if (data == NULL && len > 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (backend == FS_BACKEND_INTERNAL && !fs_on_internal_stack())
    {
        write_args_t a = { path, data, len };

        return fs_run_internal(write_job, &a);
    }

    char tmp[FS_PATH_MAX];
    char parent[FS_PATH_MAX];

    err = fs_path_temp_name(path, tmp, sizeof(tmp));

    if (err != ESP_OK)
    {
        return err;
    }

    FS_LOCK();

    if (fs_path_parent(path, parent, sizeof(parent)) == ESP_OK)
    {
        err = mkdirs_locked(parent);

        if (err != ESP_OK)
        {
            FS_UNLOCK();
            return err;
        }
    }

    FILE *f = fopen(tmp, "wb");

    if (f == NULL)
    {
        ESP_LOGE(TAG, "open '%s' for write failed: errno %d", tmp, errno);
        FS_UNLOCK();
        return errno_to_esp(errno);
    }

    const uint8_t *src = data;
    size_t         left = len;

    while (left > 0)
    {
        size_t chunk = (left > sizeof(s_scratch)) ? sizeof(s_scratch) : left;

        memcpy(s_scratch, src, chunk); /* PSRAM -> internal, cache on */

        if (fwrite(s_scratch, 1, chunk, f) != chunk)
        {
            ESP_LOGE(TAG, "write '%s' failed: errno %d", path, errno);
            fclose(f);
            unlink(tmp);
            FS_UNLOCK();
            return ESP_FAIL;
        }

        src += chunk;
        left -= chunk;
    }

    fflush(f);
    fsync(fileno(f)); /* force to media before the rename (REVIEW M-4) */

    if (fclose(f) != 0)
    {
        unlink(tmp);
        FS_UNLOCK();
        return ESP_FAIL;
    }

    if (fs_rename_replace(tmp, path) != 0)
    {
        ESP_LOGE(TAG, "rename '%s' -> '%s' failed: errno %d",
                 tmp, path, errno);
        unlink(tmp);
        FS_UNLOCK();
        return errno_to_esp(errno);
    }

    FS_UNLOCK();
    return ESP_OK;
}

static esp_err_t delete_job(void *p)
{
    return filesystem_delete(p);
}

esp_err_t filesystem_delete(const char *path)
{
    fs_backend_t backend;
    esp_err_t    err = check_path(path, &backend);

    if (err != ESP_OK)
    {
        return err;
    }

    if (backend == FS_BACKEND_INTERNAL && !fs_on_internal_stack())
    {
        return fs_run_internal(delete_job, (void *)path);
    }

    FS_LOCK();

    struct stat st;

    if (stat(path, &st) != 0)
    {
        FS_UNLOCK();
        return errno_to_esp(errno);
    }

    int rc = S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path);

    FS_UNLOCK();
    return (rc == 0) ? ESP_OK : errno_to_esp(errno);
}

static esp_err_t mkdirs_job(void *p)
{
    return filesystem_mkdirs(p);
}

esp_err_t filesystem_mkdirs(const char *dir_path)
{
    fs_backend_t backend;
    esp_err_t    err = check_path(dir_path, &backend);

    if (err != ESP_OK)
    {
        return err;
    }

    if (backend == FS_BACKEND_INTERNAL && !fs_on_internal_stack())
    {
        return fs_run_internal(mkdirs_job, (void *)dir_path);
    }

    FS_LOCK();
    err = mkdirs_locked(dir_path);
    FS_UNLOCK();
    return err;
}

/* ---- internals shared with filesystem_read.c and filesystem_stream.c ------- */

esp_err_t fs_check_path(const char *path, fs_backend_t *out_backend)
{
    return check_path(path, out_backend);
}

esp_err_t fs_mkdirs_in_lock(const char *dir_path)
{
    return mkdirs_locked(dir_path);
}

void fs_lock(void)
{
    FS_LOCK();
}

void fs_unlock(void)
{
    FS_UNLOCK();
}

uint8_t *fs_scratch(size_t *size_out)
{
    *size_out = sizeof(s_scratch);
    return s_scratch;
}

/* FAT (the card) refuses to rename over an existing file (errno 17, found
   2026-10-11 under config_backup's copies); LittleFS replaces in one step.
   The unlink costs FAT the one-step replace: a power cut between the two
   calls leaves the temp file beside no target, which every reader treats as
   absent (config_backup rewrites its copy at the next sweep). */
int fs_rename_replace(const char *tmp, const char *path)
{
    if (rename(tmp, path) == 0)
    {
        return 0;
    }

    if (errno != EEXIST)
    {
        return -1;
    }

    if (unlink(path) != 0 && errno != ENOENT)
    {
        return -1;
    }

    return rename(tmp, path);
}

esp_err_t fs_errno_to_esp(int err)
{
    return errno_to_esp(err);
}
