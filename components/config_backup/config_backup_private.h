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
 * @file config_backup_private.h
 * @brief What the files of config_backup share: the parts, the limits, the
 *        growable PSRAM buffer, the pure core (host-tested), the crypto,
 *        the document builder, the copies, the staged restore.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "cJSON.h"

/* ---- the parts --------------------------------------------------------------------- */

typedef enum
{
    CB_PART_SETTINGS = 0, /* every settings component: {name:{version,data}} */
    CB_PART_VEHICLES,     /* /data/autopid: vehicles.json, the per-car tables
                             under vehicles/, config.json, std_scan.json */
    CB_PART_DATABASES,    /* the files under /data/autopid/dtc_db and dbc */
    CB_PART_SCRIPTS,      /* the files under /data/scripts */
    CB_PART_DASHBOARD,    /* /data/dashboard.json */
    CB_PART_CERTS,        /* /data/certs/<set>/{ca.pem,client.crt,client.key}:
                             the copies only, never a file */
    CB_PART_COUNT
} cb_part_t;

#define CB_MASK(p)    (1u << (unsigned)(p))
#define CB_MASK_ALL   ((1u << CB_PART_COUNT) - 1u)
#define CB_MASK_FILE  (CB_MASK_ALL & ~CB_MASK(CB_PART_CERTS))

extern const char *const CB_PART_NAMES[CB_PART_COUNT];

/* ---- limits and places ----------------------------------------------------------- */

#define CB_DOC_VERSION    2u                   /* "wican_backup": 2 */
#define CB_COPY_VERSION   1u                   /* "wican_copy": 1, the .enc format */
#define CB_KEY_LEN        32
#define CB_NONCE_LEN      12
#define CB_TAG_LEN        16
#define CB_HMAC_LEN       32
#define CB_DOC_MAX        (2u * 1024u * 1024u) /* a document, a staged upload */
#define CB_FILE_MAX       (512u * 1024u)       /* one file inside a part */
#define CB_PEM_MAX        8192u                /* one certificate part */
#define CB_NAME_MAX       48                   /* a file or set name + NUL */
#define CB_LIST_MAX       128                  /* entries read from one dir
                                                  (32 until issue #989; the
                                                  list lives in PSRAM) */
#define CB_ID_LEN         16                   /* device id + NUL */
#define CB_FW_LEN         32
#define CB_ISO_LEN        24
#define CB_ERR_LEN        128

#define CB_DIR_INTERNAL   "/data/backups/latest"
#define CB_DIR_SD_ROOT    "/sd/backups"
#define CB_DIR_SD_FMT     "/sd/backups/wican-%s/latest"
#define CB_MANIFEST_NAME  "manifest.json"
#define CB_PART_EXT       ".enc"
#define CB_FILE_MAGIC     "WCBS"
#define CB_FILE_HDR_LEN   (4 + 1 + CB_NONCE_LEN)  /* magic, format, nonce */

#define CB_BOOT_DELAY_MS    20000
#define CB_TOUCH_DELAY_MS   5000
#define CB_SWEEP_PERIOD_MS  120000
#define CB_STAGE_LIFE_S     600
#define CB_REFRESH_WAIT_MS  30000

/* ---- what a part leaves out, and why (issue #989) ---------------------------------- */

/* Listed in the document (`left_out`), the status route and the UI instead
 * of a log line nobody reads: a file over the size limit, a name the backup
 * cannot carry, a folder beyond the list cap, a file gone or torn. */
typedef enum
{
    CB_WHY_NAME = 0, /* over CB_NAME_MAX, or not printable ASCII */
    CB_WHY_SIZE,     /* over CB_FILE_MAX */
    CB_WHY_COUNT,    /* beyond CB_LIST_MAX entries in its folder */
    CB_WHY_READ,     /* unreadable, or gone between the listing and the read */
    CB_WHY_JSON,     /* a JSON file that is not valid JSON */
} cb_why_t;

#define CB_LEFT_MAX 8 /* names kept per part; beyond that only a count */

typedef struct
{
    char    name[CB_NAME_MAX];
    uint8_t why;
} cb_left_item_t;

typedef struct
{
    cb_left_item_t items[CB_LEFT_MAX];
    size_t         n;
    size_t         more; /* left out beyond the names kept */
} cb_left_out_t;

/* ---- a growable PSRAM buffer and the file helpers (config_backup_files.c) --------- */

typedef struct
{
    char  *data;
    size_t len;
    size_t cap;
    bool   failed; /* a growth past CB_DOC_MAX or out of memory: stop early */
} cb_buf_t;

esp_err_t cb_buf_init(cb_buf_t *b, size_t cap0);
esp_err_t cb_buf_append(cb_buf_t *b, const void *data, size_t len);
esp_err_t cb_buf_puts(cb_buf_t *b, const char *s);
esp_err_t cb_buf_printf(cb_buf_t *b, const char *fmt, ...);
void      cb_buf_free(cb_buf_t *b);
/** A JSON string literal (the escapes names and PEM need). */
esp_err_t cb_json_string_append(cb_buf_t *b, const char *s, size_t n);

/** A directory read into a list (names, sizes, dir flags); the callback
 *  runs under the fs lock and only collects. Caller frees. */
typedef struct
{
    char          names[CB_LIST_MAX][CB_NAME_MAX];
    size_t        sizes[CB_LIST_MAX];
    bool          dirs[CB_LIST_MAX];
    size_t        n;
    bool          overflow;
    cb_left_out_t left; /* the names refused and the entries past the cap */
} cb_dir_t;

cb_dir_t *cb_dir_read(const char *path);
/** Read a whole file into a PSRAM buffer (caller frees). */
esp_err_t cb_read_file(const char *path, char **out, size_t *out_len,
                       size_t cap);
/** Delete every file directly inside @p dir (not the dir itself). */
esp_err_t cb_wipe_dir_files(const char *dir);
/** A JSON file appended verbatim (trimmed). ESP_ERR_NOT_FOUND when absent
 *  or unreadable, ESP_ERR_INVALID_SIZE when over CB_FILE_MAX,
 *  ESP_ERR_INVALID_RESPONSE when not JSON; a failure of the buffer itself
 *  comes with @p b->failed set. */
esp_err_t cb_append_json_file(cb_buf_t *b, const char *path);
/** {"name","size","b64"} for one file, read in blocks. */
esp_err_t cb_append_file_b64(cb_buf_t *b, const char *dir, const char *name,
                             size_t size);
/** [ {file}, ... ] for every file of a dir that fits; what does not goes
 *  into @p left (NULL = a sink) instead of failing the document. */
esp_err_t cb_append_dir_b64(cb_buf_t *b, const char *dir, cb_left_out_t *left);

/* ---- the pure core (config_backup_core.c + config_backup_codec.c, host-tested) ----- */

/** One member of a JSON object: the key (no quotes) and the raw value text. */
typedef struct
{
    const char *key;
    size_t      key_len;
    const char *val;
    size_t      val_len;
} cb_member_t;

typedef esp_err_t (*cb_member_cb_t)(const cb_member_t *m, void *ctx);

/** Returned by a member callback to stop the walk with ESP_OK. */
#define CB_STOP ((esp_err_t)0x7A5A)

/** Walk the members of the top-level object in @p buf (no parse of the
 *  values: a 200 KB base64 string costs nothing). ESP_ERR_INVALID_ARG on
 *  malformed text; the callback's error otherwise. */
esp_err_t cb_json_members(const char *buf, size_t len, cb_member_cb_t cb,
                          void *ctx);

/** Find one member by key. */
bool cb_json_find(const char *buf, size_t len, const char *key,
                  cb_member_t *out);

/** Whole-buffer validity: one JSON value with nothing but whitespace
 *  around it (the check for a file appended verbatim). */
bool cb_json_valid(const char *buf, size_t len);

/** A member's string value, unescaped (the simple escapes), into @p out.
 *  False when the value is not a string or does not fit. */
bool cb_json_string(const cb_member_t *m, char *out, size_t out_len);
bool cb_json_bool(const cb_member_t *m);
bool cb_json_is_null(const cb_member_t *m);
long cb_json_long(const cb_member_t *m);

/** Which part a name is: -1 when none. */
int cb_part_from_name(const char *name, size_t len);

/** The redaction rule: the api_http secret suffixes plus the usernames
 *  and the addresses that carry a token, per component. */
bool cb_key_is_secret(const char *component, const char *key);

/** Blank every secret in a settings part ({name:{version,data}}). */
void cb_redact_settings(cJSON *settings_part);

/** A blank secret in @p data takes the value @p stored has (the PUT rule). */
void cb_unredact(cJSON *data, const cJSON *stored, const char *component);

/** File names that may be carried and written back: the filesystem's own
 *  segment rule (printable ASCII, no `/` or `\`, not "." or "..") under
 *  CB_NAME_MAX. Spaces and a leading dot are fine (issue #989). */
bool cb_name_ok(const char *name, size_t len);

/** Record one more left-out entry (a NULL list is a sink); past CB_LEFT_MAX
 *  names only the count grows. Unprintable bytes of the name become '?'. */
void cb_left_out_add(cb_left_out_t *lo, const char *name, size_t len,
                     cb_why_t why);
/** Everything of @p src into @p dst (names first, then the count). */
void cb_left_out_merge(cb_left_out_t *dst, const cb_left_out_t *src);
/** The reason as the UI shows it. */
const char *cb_why_str(cb_why_t why);

/** A certificate set name: [a-z0-9_-]{1,24} (cert_manager's rule). */
bool cb_set_name_ok(const char *name, size_t len);

/** A vehicle key: the VIN or fp:<8 hex>: [A-Za-z0-9_:.-]{1,40}. */
bool cb_vehicle_key_ok(const char *name, size_t len);

/** The key file: comment lines (#) and whitespace around 64 hex chars. */
bool cb_key_text_decode(const char *text, size_t len, uint8_t key[CB_KEY_LEN]);
void cb_hex_encode(const uint8_t *in, size_t n, char *out /* 2n+1 */);
bool cb_hex_decode(const char *in, size_t n_chars, uint8_t *out);
/** CRC-32 (IEEE, 0xEDB88320) of the sealed part file as written: the
 *  manifest records it, the first sweep of a boot verifies the file
 *  against it, a restore refuses a part that does not match. */
uint32_t cb_crc32(const uint8_t *p, size_t n);

/** Epoch -> "YYYY-MM-DDTHH:MM:SSZ"; 0 -> "". */
void cb_iso_from_epoch(int64_t t, char out[CB_ISO_LEN]);

/* the manifest of a copy */
typedef struct
{
    bool     present;
    uint32_t bytes;               /* the plain part */
    uint32_t crc;                 /* CRC-32 of the sealed file; 0 = not
                                     recorded (a manifest from before
                                     2026-10-13): rewritten once */
    uint8_t  hmac[CB_HMAC_LEN];   /* HMAC-SHA256 of the plain part, keyed */
} cb_manifest_part_t;

typedef struct
{
    bool               loaded;    /* read from its folder (or known absent) */
    char               device_id[CB_ID_LEN];
    char               fw[CB_FW_LEN];
    int64_t            refreshed; /* epoch, 0 = clock unknown */
    bool               held;      /* the settings part stays as it was before
                                     a factory reset until a restore of a
                                     settings part or a manual refresh lets
                                     it go (issue #987) */
    int64_t            held_since; /* epoch of the boot after that reset,
                                      0 = clock unknown */
    cb_manifest_part_t parts[CB_PART_COUNT];
} cb_manifest_t;

cJSON    *cb_manifest_to_json(const cb_manifest_t *m);
esp_err_t cb_manifest_from_json(const cJSON *j, cb_manifest_t *m);
/** [{"name","why"}, ...] plus a final {"name":"","why":"and N more"} when
 *  the names ran out; NULL when nothing was left out. */
cJSON    *cb_left_out_json(const cb_left_out_t *lo);
uint32_t  cb_manifest_bytes(const cb_manifest_t *m);
unsigned  cb_manifest_count(const cb_manifest_t *m);

/* ---- crypto (config_backup_crypto.c) ------------------------------------------------ */

typedef struct
{
    int64_t created;       /* epoch, 0 = the clock was unknown */
    bool    downloaded;
    int64_t downloaded_at;
} cb_key_info_t;

esp_err_t            cb_crypto_init(void);
const uint8_t       *cb_device_key(void);
const cb_key_info_t *cb_key_info(void);
esp_err_t            cb_key_mark_downloaded(void);

/** AES-256-GCM. @p out holds ciphertext + tag (in_len + CB_TAG_LEN). */
esp_err_t cb_seal(const uint8_t *key, const char *aad, const uint8_t *in,
                  size_t in_len, uint8_t nonce[CB_NONCE_LEN], uint8_t *out,
                  size_t out_size, size_t *out_len);
/** @p in is ciphertext + tag; @p out gets in_len - CB_TAG_LEN bytes. */
esp_err_t cb_unseal(const uint8_t *key, const char *aad,
                    const uint8_t nonce[CB_NONCE_LEN], const uint8_t *in,
                    size_t in_len, uint8_t *out, size_t out_size,
                    size_t *out_len);
esp_err_t cb_hmac(const uint8_t *key, const void *data, size_t len,
                  uint8_t out[CB_HMAC_LEN]);
size_t    cb_b64_size(size_t n);  /* chars for n bytes, NUL included */
esp_err_t cb_b64_encode(const uint8_t *in, size_t n, char *out,
                        size_t out_size, size_t *out_len);
esp_err_t cb_b64_decode(const char *in, size_t n, uint8_t *out,
                        size_t out_size, size_t *out_len);

/* ---- the document (config_backup_doc.c) -------------------------------------------- */

/** Append the JSON VALUE of one part (no key). ESP_OK with an empty
 *  collection when there is nothing on the device. What the part leaves
 *  out is listed in @p left (NULL = a sink). */
esp_err_t cb_doc_part(cb_part_t part, bool redact, cb_buf_t *out,
                      cb_left_out_t *left);
/** The whole plain document for the parts in @p mask, with a top-level
 *  `left_out` object when a part left something out. */
esp_err_t cb_doc_build(uint32_t mask, bool redact, cb_buf_t *out);
/** Counts and bytes of one part, as the status route reports them, plus
 *  `left_out` when the listing shows something the part would skip. */
esp_err_t cb_doc_part_status(cb_part_t part, cJSON *into);

/* ---- the copies (config_backup_copies.c, config_backup_copies_read.c) ------------- */

esp_err_t cb_copies_start(void);
void      cb_copies_stop(void);
void      cb_copies_touch(void);
/** Refresh both copies now; waits up to @p wait_ms. A held settings part
 *  (issue #987) is let go: the user asked for the copies to follow. */
esp_err_t cb_copies_refresh(uint32_t wait_ms, cJSON *written);
/** Let a held settings part follow the device again at once (a restore of
 *  a settings part): both manifests are rewritten before the caller's
 *  restart. */
void      cb_copies_release(void);
void      cb_copies_status(cJSON *into);
/** The folder of the copy to restore from: "internal", or "sd" (our own
 *  folder, else the one other device's folder on the card). */
esp_err_t cb_copies_dir(const char *where, char *out, size_t out_len,
                        char device_id[CB_ID_LEN]);
esp_err_t cb_copies_read_manifest(const char *dir, cb_manifest_t *m);
/* the writer side lends these to config_backup_copies_read.c */
void      cb_copies_lock(void);
void      cb_copies_unlock(void);
const cb_manifest_t *cb_copies_manifest(bool sd);  /* under the lock */
void      cb_copies_sd_dir(char *out, size_t n);   /* this device on the card */
void      cb_copies_part_path(const char *dir, cb_part_t p, char *out, size_t n);
/** Assemble a plain document from a copy's sealed parts; ESP_ERR_INVALID_STATE
 *  when the folder belongs to another device and @p key is NULL;
 *  ESP_ERR_INVALID_CRC when a part fails its tag. */
esp_err_t cb_copies_load_doc(const char *dir, const uint8_t *key,
                             const cb_manifest_t *m, cb_buf_t *doc);

/* ---- the staged restore (config_backup_restore.c, config_backup_apply.c) ---------- */

typedef struct
{
    bool keep_identity;  /* this device's ap_ssid + hostname stay */
    bool keep_vpn;       /* this device's vpn_manager object stays */
    bool redacted;       /* blank secrets take the stored values */
} cb_apply_opts_t;

/** Take ownership of an uploaded document (PSRAM) and stage it. */
esp_err_t cb_restore_stage_file(char *raw, size_t len, const char *name);
/** Stage a copy kept on the device ("internal" | "sd"). */
esp_err_t cb_restore_stage_copy(const char *where);
/** A key for another device's staged backup. */
esp_err_t cb_restore_unlock(const uint8_t key[CB_KEY_LEN]);
/** The manifest of the staged backup (a fresh cJSON, caller frees); NULL
 *  when nothing is staged. */
cJSON    *cb_restore_manifest(void);
/** Apply the staged backup's parts in @p mask; @p resp gets restored[]. */
esp_err_t cb_restore_apply(uint32_t mask, bool keep_identity, bool keep_vpn,
                           cJSON *resp, char *err, size_t err_len);
void      cb_restore_drop(void);
bool      cb_restore_staged(void);

/** Dry-run one part: counts into @p info, the first error into @p err.
 *  ESP_ERR_INVALID_VERSION = newer than the firmware. The options decide
 *  how a redacted document is read (blank secrets take the stored values
 *  before validation). */
esp_err_t cb_check_part(cb_part_t part, const cb_member_t *m,
                        const cb_apply_opts_t *o, cJSON *info, char *err,
                        size_t err_len);
esp_err_t cb_apply_part(cb_part_t part, const cb_member_t *m,
                        const cb_apply_opts_t *o, char *err, size_t err_len);

/* ---- settings, cli, lifecycle (config_backup_settings.c, _cli.c, config_backup.c) -- */

esp_err_t cb_settings_register(void);
bool      cb_settings_auto_copy(void);
esp_err_t config_backup_register_cli(void);
int64_t   cb_now_epoch(void);               /* 0 while the clock is unknown */
void      cb_now_iso(char out[CB_ISO_LEN]); /* "" while the clock is unknown */
void      cb_schedule_restart(void);
const char *cb_device_id(void);
const char *cb_fw_version(void);
