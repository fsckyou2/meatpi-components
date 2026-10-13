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
 * @file filesystem_private.h
 * @brief Internals shared between filesystem.c and the pure path module
 *        (filesystem_path.c, host-testable: keep it free of IDF/VFS deps).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Longest accepted logical path, including the NUL. Temp names append
 * FS_TEMP_SUFFIX and must still fit. */
#define FS_PATH_MAX 128

#define FS_PREFIX_INTERNAL "/data"
#define FS_PREFIX_SD       "/sd"

#define FS_TEMP_SUFFIX ".tmp"

typedef enum
{
    FS_BACKEND_INTERNAL = 0,
    FS_BACKEND_SD,
    FS_BACKEND_COUNT,
} fs_backend_t;

/**
 * Validate a logical path and identify its backend.
 *
 * Accepted: "<prefix>" or "<prefix>/seg[/seg...]" where prefix is /data or
 * /sd; segments are printable ASCII without '\' ; no empty ("//"), "." or
 * ".." segments; no trailing '/'; total length < FS_PATH_MAX.
 *
 * Returns ESP_ERR_INVALID_ARG on any violation. @p out_backend nullable.
 */
esp_err_t fs_path_resolve(const char *path, fs_backend_t *out_backend);

/**
 * Temp-file sibling used by atomic replace: "<path>.tmp" (same directory,
 * so rename() stays within one filesystem). ESP_ERR_INVALID_ARG if it
 * doesn't fit in @p out_len or FS_PATH_MAX.
 */
esp_err_t fs_path_temp_name(const char *path, char *out, size_t out_len);

/**
 * Directory part of a validated path: "/data/a/b.txt" -> "/data/a";
 * "/data/b.txt" -> "/data". ESP_ERR_INVALID_ARG if @p path has no parent
 * (is itself a prefix root) or the result doesn't fit.
 */
esp_err_t fs_path_parent(const char *path, char *out, size_t out_len);

/**
 * True when @p len bytes at @p buf are all erased flash (0xFF). Used to
 * recognise a never-formatted partition (first boot after an erase) so it
 * can be formatted explicitly instead of letting lfs_mount fail loudly.
 * Pure: host-tested. NULL or empty buffers are NOT blank.
 */
bool fs_region_is_blank(const uint8_t *buf, size_t len);

/**
 * What the superblock pair of a LittleFS partition holds (2026-10-06).
 *
 * A unit that ran the factory firmware keeps that firmware's 6 MB LittleFS
 * where this firmware has its partitions, and a flash without an erase
 * leaves it there. LittleFS mounts a superblock whatever block count it
 * claims (the ESP port takes the count from the superblock), so a
 * filesystem from another layout "mounts" and every access past the
 * partition's end fails: error lines at boot, a latched fault, writes that
 * fail. Read the pair before mounting: the superblock entry sits at the
 * start of the pair's blocks as the 8-byte name "littlefs", a 4-byte tag
 * and the inline struct {version, block_size, block_count, ...} as
 * little-endian u32s (tags are XOR-chained, the data is not).
 *
 * @p blk0 / @p blk1: the first @p len bytes of blocks 0 and 1 (256 is
 * plenty: the entry is the first thing in a block). @p block_size: the
 * size this firmware mounts with. BLANK = both erased (never formatted);
 * LITTLEFS = a superblock of a version-2 filesystem with that block size,
 * its block count in @p block_count; OTHER = neither (another filesystem,
 * or data blocks of one). Pure: host-tested. A copy lives in
 * settings_manager (settings_manager_lfs.c), which sits below this
 * component. */
typedef enum
{
    FS_LFS_BLANK = 0,
    FS_LFS_LITTLEFS,
    FS_LFS_OTHER,
} fs_lfs_kind_t;

fs_lfs_kind_t fs_lfs_probe(const uint8_t *blk0, const uint8_t *blk1,
                           size_t len, uint32_t block_size,
                           uint32_t *block_count);

/* ---- filesystem.c internals shared with filesystem_stream.c (esp target
 * only: never referenced by the host-built path module) ------------------- */

/** filesystem_mount.c: probe the superblock pair of @p label and format a
 *  blank or foreign partition quietly before the mount (one I or W line). */
void fs_mount_prepare(const char *label);

esp_err_t fs_check_path(const char *path, fs_backend_t *out_backend);
esp_err_t fs_mkdirs_in_lock(const char *dir_path); /* call under fs_lock */
void fs_lock(void);
void fs_unlock(void);
uint8_t *fs_scratch(size_t *size_out); /* internal-RAM bounce buffer;
                                          use under fs_lock only */
esp_err_t fs_errno_to_esp(int err);
/** rename(2) that replaces an existing target on FAT too (unlink first on
 *  EEXIST); 0 or -1 with errno. */
int fs_rename_replace(const char *tmp, const char *path);

/** Start the async-pipe writer service (called by filesystem_init). */
esp_err_t fs_stream_service_init(void);

/* ---- the internal-stack service (filesystem_stream.c, 2026-10-13) ------------
   Every /data operation reads or writes the flash with the cache off, which
   a task on a PSRAM stack cannot survive (standard §2). A public operation
   that finds itself on such a stack hands the call to the stream writer's
   internal stack through fs_run_internal: synchronous, one job at a time,
   the caller's buffers wherever they are. The job calls the same public
   function again on the writer task, where the check passes. */
typedef esp_err_t (*fs_job_fn_t)(void *arg);
bool      fs_on_internal_stack(void);  /* the current task's stack is DRAM */
esp_err_t fs_run_internal(fs_job_fn_t fn, void *arg);

#ifdef __cplusplus
}
#endif
