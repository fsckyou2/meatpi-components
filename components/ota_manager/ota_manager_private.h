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
 * @file ota_manager_private.h
 * @brief Internal API: the PURE session state machine (host-testable,
 *        flash operations and the digest injected) and its contract with
 *        the esp_ota glue and the pipe.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "ota_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Injected flash backend (esp_ota on target, a recorder in host tests).
 *  The three hash ops are optional as a set: with them the session keeps
 *  the digest of every byte it wrote and can compare it at `end`. */
typedef struct
{
    esp_err_t (*begin)(void *io, size_t total_size);
    esp_err_t (*write)(void *io, const void *data, size_t len);
    esp_err_t (*end)(void *io);   /* validate + set boot partition */
    void      (*abort)(void *io);
    void      (*hash_reset)(void *io);
    void      (*hash_update)(void *io, const void *data, size_t len);
    void      (*hash_final)(void *io, uint8_t out[32]);
} ota_session_ops_t;

/* The image's identity (2026-10-18): the first 288 bytes are held back
 * until they are complete, then checked before a byte reaches the backend:
 * the 24-byte image header (magic 0xE9 at 0, the chip id at 12), the 8-byte
 * segment header, and the esp_app_desc_t at 32 (magic 0xABCD5432, the
 * version at +16, the project name at +48). */
#define OTA_SESSION_HEAD_LEN     288U
#define OTA_SESSION_IMAGE_MAGIC  0xE9U
#define OTA_SESSION_DESC_MAGIC   0xABCD5432U
#define OTA_SESSION_CHIP_ESP32S3 0x0009U
#define OTA_SESSION_NAME_LEN     33U

typedef struct
{
    const ota_session_ops_t *ops;
    void *io;
    ota_manager_state_t state;
    uint32_t received;
    uint32_t total;
    char error[64];
    /* the head check: on once ota_session_set_product() named a product */
    bool     check_head;
    const char *product;              /* "WiCAN PRO": the refusal text     */
    const char *const *prefixes;      /* accepted project_name prefixes     */
    size_t   prefix_count;
    uint16_t chip_id;                 /* 0 = any chip                       */
    uint8_t  head[OTA_SESSION_HEAD_LEN];
    size_t   head_len;
    bool     head_checked;
    char     project_name[OTA_SESSION_NAME_LEN]; /* read from the head      */
    char     version[OTA_SESSION_NAME_LEN];
    /* the expected digest, armed per session */
    bool     expect_digest;
    uint8_t  expected[32];
} ota_session_t;

/** Reset to IDLE with the given backend. */
void ota_session_init(ota_session_t *s, const ota_session_ops_t *ops,
                      void *io);

/** Turn the head check on: images whose project name starts with none of
 *  the @p prefixes (or whose chip id is not @p chip_id, 0 = any) are
 *  refused with "not a <product> image: <name>". The pointers must stay
 *  valid for the life of the firmware. */
void ota_session_set_product(ota_session_t *s, const char *product,
                             const char *const *prefixes, size_t count,
                             uint16_t chip_id);

/**
 * Transition rules (host-tested):
 *  - begin: allowed from IDLE / FAILED / READY (retry & re-upload);
 *    ESP_ERR_INVALID_STATE while RECEIVING. Backend failure -> FAILED.
 *    begin_ex arms an expected SHA-256 (NULL = none) for this session.
 *  - write: only while RECEIVING; overrun of an announced total, zero-len,
 *    a refused head (ESP_ERR_INVALID_RESPONSE) or backend failure ->
 *    FAILED (+ backend abort).
 *  - end: only while RECEIVING; zero bytes ("empty image"), short of an
 *    announced total ("short image"), shorter than the head, a digest
 *    that differs ("sha256 mismatch", ESP_ERR_INVALID_CRC) or backend
 *    failure -> FAILED.
 *  - abort: any state -> IDLE (backend abort when a session was open).
 */
esp_err_t ota_session_begin(ota_session_t *s, size_t total_size);
esp_err_t ota_session_begin_ex(ota_session_t *s, size_t total_size,
                               const uint8_t *expected_sha256);
esp_err_t ota_session_write(ota_session_t *s, const void *data, size_t len);
esp_err_t ota_session_end(ota_session_t *s);
esp_err_t ota_session_abort(ota_session_t *s);

/**
 * The head check alone: @p head holds at least OTA_SESSION_HEAD_LEN bytes.
 * @p name_out / @p version_out (nullable, OTA_SESSION_NAME_LEN) receive
 * the descriptor's fields; @p err (64) the refusal text. True = accepted.
 */
bool ota_image_head_check(const uint8_t *head, size_t len, uint16_t chip_id,
                          const char *product, const char *const *prefixes,
                          size_t prefix_count, char *name_out,
                          char *version_out, char *err, size_t err_len);

/* ---- the pipe (ota_pipe.c): the glue's internal-stack writer ----------------- */

esp_err_t ota_pipe_begin(size_t total_size, const uint8_t *expected_sha256);
esp_err_t ota_pipe_write(const uint8_t *data, size_t len, uint32_t wait_ms);
esp_err_t ota_pipe_end(uint32_t wait_ms);
esp_err_t ota_pipe_abort(void);
bool      ota_pipe_active(void);

#ifdef __cplusplus
}
#endif
