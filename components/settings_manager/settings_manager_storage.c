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
 * @file settings_manager_storage.c
 * @brief LittleFS ownership + atomic settings file IO. Target-only.
 *
 * The Settings Manager is the only code that mounts or touches the settings FS.
 * Writes use temp-file-plus-rename so a power loss mid-write leaves the previous
 * good file intact (rollback-on-corruption, Coding Standard §7).
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_partition.h"

#include "settings_manager_private.h"

#define SM_PARTITION_LABEL "settings"
#define SM_BASE_PATH       "/settings"
#define SM_DIR             SM_BASE_PATH "/cfg"
#define SM_PATH_MAX        128

/* LittleFS superblock pair = blocks {0,1}, one 4 KiB flash sector each */
#define SM_LFS_BLOCK_SIZE 4096u
#define SM_PROBE_LEN      256u  /* of each superblock block */

static const char *TAG = "settings_manager";
static bool s_mounted;

/**
 * What the settings partition's superblock pair holds (sm_lfs_probe):
 * BLANK after an erase, a LittleFS with its block count, or something
 * else. A missing partition or a read error answers LITTLEFS with the
 * partition's own block count, so the normal mount path runs.
 */
static sm_lfs_kind_t partition_probe(uint32_t *blocks, uint32_t *found)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, SM_PARTITION_LABEL);
    static uint8_t probe[2][SM_PROBE_LEN]; /* boot only; internal: the
                                              settings FS lives before PSRAM
                                              attrs are worth a dependency */

    *blocks = 0;
    *found = 0;

    if (part == NULL)
    {
        return SM_LFS_LITTLEFS;
    }

    *blocks = part->size / SM_LFS_BLOCK_SIZE;
    *found = *blocks;

    for (uint32_t blk = 0; blk < 2; blk++)
    {
        if (esp_partition_read(part, blk * SM_LFS_BLOCK_SIZE, probe[blk],
                               SM_PROBE_LEN) != ESP_OK)
        {
            return SM_LFS_LITTLEFS;
        }
    }

    return sm_lfs_probe(probe[0], probe[1], SM_PROBE_LEN, SM_LFS_BLOCK_SIZE,
                        found);
}

/**
 * Format the partition before the mount when it is not ours. First boot
 * after an erase: both superblock blocks read 0xFF, and letting
 * lfs_mount() discover that logs "Corrupted dir pair" at E level, which
 * latched a boot_errors fault on every new unit. A unit flashed over the
 * factory firmware without an erase (2026-10-06): that firmware's 6 MB
 * LittleFS starts exactly here, LittleFS mounts it whatever block count
 * it claims, and every access past our 256 KB fails: 83 E lines at boot,
 * the fault, no setting persisted. A LittleFS of our own size that fails
 * to mount still takes the loud path: that IS a fault worth seeing.
 */
static void format_if_not_ours(void)
{
    uint32_t blocks;
    uint32_t found;
    sm_lfs_kind_t kind = partition_probe(&blocks, &found);

    if (kind == SM_LFS_BLANK)
    {
        ESP_LOGI(TAG, "'%s' is blank (first boot): formatting", SM_PARTITION_LABEL);
    }
    else if (kind == SM_LFS_OTHER)
    {
        ESP_LOGW(TAG, "'%s' holds no LittleFS superblock (another firmware's "
                 "data): formatting", SM_PARTITION_LABEL);
    }
    else if (found != blocks)
    {
        ESP_LOGW(TAG, "'%s' holds a LittleFS of %lu blocks on a %lu-block "
                 "partition (another firmware's layout, the factory image?): "
                 "formatting", SM_PARTITION_LABEL, (unsigned long)found,
                 (unsigned long)blocks);
    }
    else
    {
        return;
    }

    esp_err_t ferr = esp_littlefs_format(SM_PARTITION_LABEL);

    if (ferr != ESP_OK)
    {
        ESP_LOGW(TAG, "pre-format failed: %s (mount will retry)",
                 esp_err_to_name(ferr));
    }
}

static void build_path(char *buf, size_t buf_len, const char *name, const char *ext)
{
    snprintf(buf, buf_len, "%s/%s%s", SM_DIR, name, ext);
}

esp_err_t sm_storage_mount(void)
{
    if (s_mounted)
    {
        return ESP_OK;
    }

    format_if_not_ours();

    esp_vfs_littlefs_conf_t conf =
    {
        .base_path              = SM_BASE_PATH,
        .partition_label        = SM_PARTITION_LABEL,
        .format_if_mount_failed = true,
        .dont_mount             = false,
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "littlefs mount failed: %s", esp_err_to_name(err));
        return err;
    }

    mkdir(SM_DIR, 0775);   /* harmless if it already exists */
    s_mounted = true;
    return ESP_OK;
}

esp_err_t sm_storage_unmount(void)
{
    if (!s_mounted)
    {
        return ESP_OK;
    }

    esp_err_t err = esp_vfs_littlefs_unregister(SM_PARTITION_LABEL);
    s_mounted = false;
    return err;
}

/* Read an entire file into a newly malloc'd, NUL-terminated buffer. */
static esp_err_t read_file(const char *path, char **out)
{
    FILE *f = fopen(path, "rb");

    if (f == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0)
    {
        fclose(f);
        return ESP_FAIL;
    }

    char *buf = malloc((size_t)size + 1);

    if (buf == NULL)
    {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t read = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[read] = '\0';

    *out = buf;
    return ESP_OK;
}

esp_err_t sm_storage_load(const char *name, uint32_t *version_out, cJSON **data_out)
{
    if (name == NULL || data_out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    char path[SM_PATH_MAX];
    build_path(path, sizeof(path), name, ".json");

    char *raw = NULL;
    esp_err_t err = read_file(path, &raw);

    if (err != ESP_OK)
    {
        return err;   /* NOT_FOUND on first boot is expected */
    }

    err = sm_codec_decode(raw, version_out, data_out);
    free(raw);
    return err;
}

esp_err_t sm_storage_save(const char *name, uint32_t version, const cJSON *data)
{
    if (name == NULL || data == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    char *env = NULL;
    esp_err_t err = sm_codec_encode(version, data, &env);

    if (err != ESP_OK)
    {
        return err;
    }

    size_t env_len = strlen(env);

    /* internal: the source buffer is read during the flash write (cache off). */
    char *scratch = heap_caps_malloc(env_len, MALLOC_CAP_INTERNAL);

    if (scratch == NULL)
    {
        free(env);
        return ESP_ERR_NO_MEM;
    }

    memcpy(scratch, env, env_len);
    free(env);

    char tmp_path[SM_PATH_MAX];
    char path[SM_PATH_MAX];
    build_path(tmp_path, sizeof(tmp_path), name, ".tmp");
    build_path(path, sizeof(path), name, ".json");

    FILE *f = fopen(tmp_path, "wb");

    if (f == NULL)
    {
        free(scratch);
        ESP_LOGE(TAG, "open %s failed", tmp_path);
        return ESP_FAIL;
    }

    size_t written = fwrite(scratch, 1, env_len, f);
    fflush(f);
    fsync(fileno(f));   /* force to media before rename: fflush alone only empties
                           stdio buffers, leaving a power-cut corruption window */
    fclose(f);
    free(scratch);

    if (written != env_len)
    {
        remove(tmp_path);
        return ESP_FAIL;
    }

    /* Atomic swap: the .json is either the old good file or the new one. */
    if (rename(tmp_path, path) != 0)
    {
        remove(tmp_path);
        ESP_LOGE(TAG, "rename %s -> %s failed", tmp_path, path);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t sm_storage_wipe(void)
{
    if (!s_mounted)
    {
        return ESP_ERR_INVALID_STATE;
    }

    DIR *dir = opendir(SM_DIR);

    if (dir == NULL)
    {
        return ESP_FAIL;
    }

    /* unlink everything in the cfg dir (json + any stray .tmp):
       the next boot's load pass finds nothing and applies pure
       factory defaults */
    int removed = 0;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL)
    {
        if (entry->d_type == DT_DIR)
        {
            continue;
        }

        char path[SM_PATH_MAX + 256]; /* dir + d_name can't truncate */

        snprintf(path, sizeof(path), "%s/%s", SM_DIR, entry->d_name);

        if (remove(path) == 0)
        {
            removed++;
        }
        else
        {
            ESP_LOGE(TAG, "factory reset: remove %s failed", path);
        }
    }

    closedir(dir);
    ESP_LOGW(TAG, "factory reset: %d settings file(s) deleted", removed);
    return ESP_OK;
}

bool sm_storage_saved(const char *name)
{
    if (name == NULL)
    {
        return false;
    }

    char path[SM_PATH_MAX];
    build_path(path, sizeof(path), name, ".json");

    struct stat st;
    return stat(path, &st) == 0;
}

bool sm_storage_file_exists(const char *name)
{
    if (name == NULL)
    {
        return false;
    }

    char path[SM_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", SM_BASE_PATH, name);

    struct stat st;
    return stat(path, &st) == 0;
}
