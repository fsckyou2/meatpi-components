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
 * @file fw_update_core.h
 * @brief The PURE half of fw_update, host-tested: the version grammar and
 *        order, channel visibility, the bounded manifest parse, the release
 *        selection with stepping and yanked releases, the layout verdict
 *        and URL composition. No IDF dependencies (cJSON only).
 *
 * The contract it implements is the meatpi-devices manifest (schema 1,
 * internal/TASK_fw_update.md section 3): one manifest per device type at
 * `<base_url><device_type>/firmware/manifest.json`.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- versions ------------------------------------------------------------------- */

#define FWU_PRE_NONE 0U
#define FWU_PRE_ALFA 1U
#define FWU_PRE_BETA 2U

typedef struct
{
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
    uint8_t  pre;    /**< FWU_PRE_*                                     */
    uint16_t pre_n;  /**< the N of alfa.N / beta.N                      */
    bool     dev;    /**< a git describe tail or -dirty: a dev build    */
} fwu_version_t;

/** The strict grammar: MAJOR.MINOR.PATCH[-(alfa|beta).N], no leading
 *  zeros, every number below 65536. */
bool fwu_version_parse_strict(const char *s, fwu_version_t *out);

/** The tolerant grammar for what a device reports:
 *  [vV] MAJOR . MINOR [. PATCH] [p|u] [SEP (alfa|alpha|beta) SEP N] [BUILD]
 *  with SEP in "-_.", BUILD = -digits-g<hex>[-dirty] or -dirty. Missing
 *  PATCH is 0, leading zeros are allowed, the hardware letter is dropped. */
bool fwu_version_parse_tolerant(const char *s, fwu_version_t *out);

/** The order: numbers first; a release beats a prerelease; beta beats
 *  alfa; N numerically. `dev` does not take part. */
int fwu_version_cmp(const fwu_version_t *a, const fwu_version_t *b);

/** "6.1.0-alfa.1". snprintf semantics. */
int fwu_version_str(const fwu_version_t *v, char *out, size_t cap);

/* ---- channels ------------------------------------------------------------------- */

#define FWU_CH_STABLE 0U
#define FWU_CH_BETA   1U
#define FWU_CH_ALFA   2U

/** -1 for anything but stable / beta / alfa. */
int fwu_channel_from_str(const char *s);
const char *fwu_channel_str(uint8_t ch);

/** visible(stable) = {stable}; visible(beta) = {stable, beta};
 *  visible(alfa) = all three. */
bool fwu_channel_visible(uint8_t device_channel, uint8_t release_channel);

/* ---- the partition layout verdict ------------------------------------------------ */

typedef enum
{
    FWU_LAYOUT_NONE = 0, /**< the release carries the layout in flash: install */
    FWU_LAYOUT_LOSSLESS, /**< the first boot rewrites the table, nothing lost   */
    FWU_LAYOUT_WIPE,     /**< the rewrite formats the partitions in `wipes`     */
    FWU_LAYOUT_SERIAL,   /**< an app slot or an anchor moves: a USB flash       */
    FWU_LAYOUT_UNKNOWN,  /**< the manifest says nothing about this layout      */
} fwu_layout_t;

const char *fwu_layout_str(fwu_layout_t v);

/* ---- the manifest -------------------------------------------------------------- */

#define FWU_MAX_RELEASES 64U
#define FWU_MAX_FROM     4U
#define FWU_VERSTR_LEN   24U
#define FWU_FWVER_LEN    32U
#define FWU_SUMMARY_LEN  141U
#define FWU_NOTES_LEN    96U
#define FWU_URL_LEN      160U
#define FWU_SHA_LEN      65U
#define FWU_MD5_LEN      33U
#define FWU_NAME_LEN     32U
#define FWU_WIPES_LEN    64U
#define FWU_REASON_LEN   96U
#define FWU_DATE_LEN     11U

typedef struct
{
    char    md5[FWU_MD5_LEN];
    uint8_t migration;            /**< fwu_layout_t: LOSSLESS, WIPE or SERIAL */
    char    wipes[FWU_WIPES_LEN]; /**< comma list of the labels formatted      */
} fwu_layout_from_t;

typedef struct
{
    fwu_version_t version;
    char     version_str[FWU_VERSTR_LEN];
    char     fw_version[FWU_FWVER_LEN]; /**< what the device reports exactly  */
    uint8_t  channel;
    bool     yanked;
    bool     has_min;
    fwu_version_t min_version;
    char     min_version_str[FWU_VERSTR_LEN];
    char     date[FWU_DATE_LEN];
    char     summary[FWU_SUMMARY_LEN];
    char     notes[FWU_NOTES_LEN];      /**< relative to base_url            */
    char     url[FWU_URL_LEN];          /**< the image, absolute             */
    uint32_t size;
    char     sha256[FWU_SHA_LEN];       /**< lowercase hex                   */
    char     image_project[FWU_NAME_LEN];
    char     layout_md5[FWU_MD5_LEN];
    uint8_t  n_from;
    fwu_layout_from_t from[FWU_MAX_FROM];
    char     yank_reason[FWU_REASON_LEN];
} fwu_release_t;

typedef struct
{
    char   device_type[FWU_NAME_LEN];
    char   model[FWU_NAME_LEN];
    char   project_name[FWU_NAME_LEN];
    char   updated[24];
    size_t n;                          /**< releases kept, newest first      */
    size_t dropped;                    /**< releases skipped as malformed    */
    fwu_release_t rel[FWU_MAX_RELEASES];
} fwu_manifest_t;

typedef enum
{
    FWU_PARSE_OK = 0,
    FWU_PARSE_INVALID,      /**< not the document (schema, shape)          */
    FWU_PARSE_WRONG_DEVICE, /**< another device type's manifest            */
    FWU_PARSE_EMPTY,        /**< no release survived the field rules       */
} fwu_parse_rc_t;

/**
 * Parse a manifest document. Releases with a malformed required field are
 * skipped and counted in `dropped`; the result is sorted newest first.
 * @param allow_http  accept http:// image URLs (a bench base_url)
 * @param err         a short reason for the log (nullable)
 */
fwu_parse_rc_t fwu_manifest_parse(const char *json, size_t len,
                                  const char *device_type, bool allow_http,
                                  fwu_manifest_t *out, char *err,
                                  size_t err_len);

/* ---- the selection -------------------------------------------------------------- */

typedef struct
{
    bool available;        /**< a newer release the channel offers           */
    bool installable;      /**< and its layout verdict allows an OTA         */
    int  target;           /**< index into the manifest, -1 none             */
    int  latest;           /**< the channel's newest visible release, -1     */
    int  me;               /**< the installed release by fw_version, -1      */
    bool step;             /**< the target is a stepping stone               */
    bool installed_known;  /**< the installed string matched a release       */
    bool installed_parsed; /**< or at least parsed tolerantly                */
    bool dev_build;        /**< the installed string is a dev build          */
    bool installed_yanked; /**< the installed release is yanked              */
    fwu_layout_t verdict;  /**< for the target                               */
    const char *wipes;     /**< borrowed from the manifest; "" when none     */
    char reason[24];       /**< "" or up_to_date / no_release / no_path /
                                unknown_version / layout_serial /
                                layout_unknown                              */
} fwu_selection_t;

/**
 * The selection algorithm (TASK_fw_update.md 3.5), the same rule HA uses.
 * @param installed_fw_version  what this device reports (the app descriptor)
 * @param flashed_md5           the partition table's MD5 in flash, "" none
 */
void fwu_select(const fwu_manifest_t *m, uint8_t channel,
                const char *installed_fw_version, const char *flashed_md5,
                fwu_selection_t *out);

/** The release whose version string equals @p version, -1 when none. */
int fwu_find_version(const fwu_manifest_t *m, const char *version);

/** The verdict for one release against the layout in flash. @p wipes
 *  (nullable) receives the labels for a WIPE, "" otherwise. */
fwu_layout_t fwu_layout_verdict(const fwu_release_t *r,
                                const char *flashed_md5,
                                const char **wipes);

/* ---- URLs ----------------------------------------------------------------------- */

/** A base_url: http(s)://host..., printable ASCII, ends with '/'. @p is_http
 *  (nullable) says whether it is plain http. */
bool fwu_base_url_valid(const char *base_url, bool *is_http);

/** base_url + a relative path. False when it does not fit. */
bool fwu_compose_url(const char *base_url, const char *path, char *out,
                     size_t cap);

/* ---- the image head -------------------------------------------------------------- */

#define FWU_IMAGE_HEAD_LEN 288U

/**
 * The application descriptor of an ESP32-S3 application image, read from
 * its first 288 bytes (the image header, the first segment header, then
 * esp_app_desc_t): the project name and the version string. False when
 * the bytes are not such an image (the magic, the chip id, the descriptor
 * magic). @p len may exceed the head.
 */
bool fwu_image_head_parse(const uint8_t *head, size_t len, char *project,
                          size_t project_cap, char *version,
                          size_t version_cap);

#ifdef __cplusplus
}
#endif
