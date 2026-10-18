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
 * @file ota_session.c
 * @brief The PURE update-session state machine: no esp_ota, no locks
 *        (the glue serializes); flash operations and the digest are
 *        injected, so every transition host-tests without hardware.
 *
 *        2026-10-18: the first 288 bytes of an image are held back and
 *        checked (an application image for this chip whose project name
 *        is one of ours: an ESPNetLink image no longer flashes onto a
 *        WiCAN PRO), and an expected SHA-256 is compared before the image
 *        is activated, so the manifest's digest is what decides.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "ota_manager_private.h"

static void fail(ota_session_t *s, const char *why, bool abort_backend)
{
    if (abort_backend && s->ops->abort != NULL)
    {
        s->ops->abort(s->io);
    }

    snprintf(s->error, sizeof(s->error), "%s", why);
    s->state = OTA_MANAGER_FAILED;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/** A 32-byte descriptor field: printable, NUL-terminated. */
static void field(const uint8_t *p, char *out)
{
    size_t n = 0;

    while (n < 32U && p[n] != 0U)
    {
        unsigned char c = p[n];

        out[n] = (c >= 0x20U && c < 0x7FU) ? (char)c : '?';
        n++;
    }

    out[n] = '\0';
}

bool ota_image_head_check(const uint8_t *head, size_t len, uint16_t chip_id,
                          const char *product, const char *const *prefixes,
                          size_t prefix_count, char *name_out,
                          char *version_out, char *err, size_t err_len)
{
    char name[OTA_SESSION_NAME_LEN];

    if (head == NULL || len < OTA_SESSION_HEAD_LEN)
    {
        snprintf(err, err_len, "short image (no application descriptor)");
        return false;
    }

    if (head[0] != OTA_SESSION_IMAGE_MAGIC)
    {
        snprintf(err, err_len, "not an application image");
        return false;
    }

    if (chip_id != 0U && rd16(head + 12) != chip_id)
    {
        snprintf(err, err_len, "an image for another chip (id 0x%04x)",
                 (unsigned)rd16(head + 12));
        return false;
    }

    if (rd32(head + 32) != OTA_SESSION_DESC_MAGIC)
    {
        snprintf(err, err_len, "no application descriptor");
        return false;
    }

    field(head + 80, name);

    if (name_out != NULL)
    {
        snprintf(name_out, OTA_SESSION_NAME_LEN, "%s", name);
    }

    if (version_out != NULL)
    {
        field(head + 48, version_out);
    }

    if (prefixes != NULL && prefix_count > 0)
    {
        bool ok = false;

        for (size_t i = 0; i < prefix_count && !ok; i++)
        {
            ok = strncmp(name, prefixes[i], strlen(prefixes[i])) == 0;
        }

        if (!ok)
        {
            snprintf(err, err_len, "not a %s image: %s",
                     (product != NULL) ? product : "firmware", name);
            return false;
        }
    }

    err[0] = '\0';
    return true;
}

void ota_session_init(ota_session_t *s, const ota_session_ops_t *ops,
                      void *io)
{
    memset(s, 0, sizeof(*s));
    s->ops = ops;
    s->io = io;
    s->state = OTA_MANAGER_IDLE;
}

void ota_session_set_product(ota_session_t *s, const char *product,
                             const char *const *prefixes, size_t count,
                             uint16_t chip_id)
{
    s->check_head = true;
    s->product = product;
    s->prefixes = prefixes;
    s->prefix_count = count;
    s->chip_id = chip_id;
}

esp_err_t ota_session_begin_ex(ota_session_t *s, size_t total_size,
                               const uint8_t *expected_sha256)
{
    if (s->state == OTA_MANAGER_RECEIVING)
    {
        return ESP_ERR_INVALID_STATE; /* one session at a time */
    }

    /* IDLE, FAILED (retry) and READY (re-upload before reboot) may begin */
    s->received = 0;
    s->total = (uint32_t)total_size;
    s->error[0] = '\0';
    s->head_len = 0;
    s->head_checked = false;
    s->project_name[0] = '\0';
    s->version[0] = '\0';
    s->expect_digest = expected_sha256 != NULL;

    if (s->expect_digest)
    {
        memcpy(s->expected, expected_sha256, sizeof(s->expected));
    }

    if (s->ops->hash_reset != NULL)
    {
        s->ops->hash_reset(s->io);
    }

    esp_err_t err = s->ops->begin(s->io, total_size);

    if (err != ESP_OK)
    {
        fail(s, "flash begin failed", false);
        return err;
    }

    s->state = OTA_MANAGER_RECEIVING;
    return ESP_OK;
}

esp_err_t ota_session_begin(ota_session_t *s, size_t total_size)
{
    return ota_session_begin_ex(s, total_size, NULL);
}

/** Bytes to the backend (and the digest). */
static esp_err_t push(ota_session_t *s, const uint8_t *data, size_t len)
{
    esp_err_t err;

    if (len == 0)
    {
        return ESP_OK;
    }

    if (s->ops->hash_update != NULL)
    {
        s->ops->hash_update(s->io, data, len);
    }

    err = s->ops->write(s->io, data, len);

    if (err != ESP_OK)
    {
        fail(s, "flash write failed", true);
    }

    return err;
}

esp_err_t ota_session_write(ota_session_t *s, const void *data, size_t len)
{
    const uint8_t *p = data;

    if (s->state != OTA_MANAGER_RECEIVING)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (data == NULL || len == 0)
    {
        fail(s, "empty write", true);
        return ESP_ERR_INVALID_ARG;
    }

    if (s->total != 0 && s->received + len > s->total)
    {
        fail(s, "image larger than announced size", true);
        return ESP_ERR_INVALID_SIZE;
    }

    s->received += (uint32_t)len;

    if (s->check_head && !s->head_checked)
    {
        size_t take = OTA_SESSION_HEAD_LEN - s->head_len;

        if (take > len)
        {
            take = len;
        }

        memcpy(s->head + s->head_len, p, take);
        s->head_len += take;
        p += take;
        len -= take;

        if (s->head_len < OTA_SESSION_HEAD_LEN)
        {
            return ESP_OK; /* held back until the head is complete */
        }

        char why[64];

        if (!ota_image_head_check(s->head, s->head_len, s->chip_id, s->product,
                                  s->prefixes, s->prefix_count,
                                  s->project_name, s->version, why,
                                  sizeof(why)))
        {
            fail(s, why, true);
            return ESP_ERR_INVALID_RESPONSE;
        }

        s->head_checked = true;

        esp_err_t err = push(s, s->head, OTA_SESSION_HEAD_LEN);

        if (err != ESP_OK)
        {
            return err;
        }
    }

    return push(s, p, len);
}

esp_err_t ota_session_end(ota_session_t *s)
{
    if (s->state != OTA_MANAGER_RECEIVING)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (s->received == 0)
    {
        fail(s, "empty image", true);
        return ESP_ERR_INVALID_SIZE;
    }

    if (s->total != 0 && s->received < s->total)
    {
        fail(s, "short image (connection dropped?)", true);
        return ESP_ERR_INVALID_SIZE;
    }

    if (s->check_head && !s->head_checked)
    {
        fail(s, "short image (no application descriptor)", true);
        return ESP_ERR_INVALID_SIZE;
    }

    if (s->expect_digest && s->ops->hash_final != NULL)
    {
        uint8_t got[32];

        s->ops->hash_final(s->io, got);

        if (memcmp(got, s->expected, sizeof(got)) != 0)
        {
            fail(s, "sha256 mismatch", true);
            return ESP_ERR_INVALID_CRC;
        }
    }

    esp_err_t err = s->ops->end(s->io);

    if (err != ESP_OK)
    {
        /* esp_ota_end already released the handle on failure */
        fail(s, "image validation failed", false);
        return err;
    }

    s->state = OTA_MANAGER_READY;
    return ESP_OK;
}

esp_err_t ota_session_abort(ota_session_t *s)
{
    if (s->state == OTA_MANAGER_RECEIVING && s->ops->abort != NULL)
    {
        s->ops->abort(s->io);
    }

    s->state = OTA_MANAGER_IDLE;
    s->received = 0;
    s->total = 0;
    s->error[0] = '\0';
    s->head_len = 0;
    s->head_checked = false;
    s->expect_digest = false;
    return ESP_OK;
}
