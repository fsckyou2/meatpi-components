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
 * @file ota_manager.c
 * @brief esp_ota glue around the pure session: partition selection, flash
 *        I/O, the SHA-256 digest (PSA), the boot-partition switch, the
 *        confirm of a pending image, and the mutex that serializes
 *        transports. The pipe for PSRAM-stack producers is ota_pipe.c.
 */
#include "ota_manager.h"

#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "psa/crypto.h"

#include "log_manager.h"

#include "ota_manager_private.h"

static const char *TAG = "ota_manager";

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf; /* internal: FreeRTOS object */

static ota_session_t s_session;
static esp_ota_handle_t s_handle;
static const esp_partition_t *s_target;
static bool s_inited;
static ota_manager_event_cb_t s_event_cb;

/* the digest of the session (PSA: mbedtls 4.1 has no mbedtls_sha256 API) */
static psa_hash_operation_t s_hash;
static bool s_hash_open;

/* an expected digest armed for the next begin (the upload route's ?sha256=) */
static bool s_expect_armed;
static uint8_t s_expect[32];

/** Fire the state-change callback (outside s_lock: the callback may do
 *  I/O, e.g. main's LED-indication glue). */
static void notify(ota_manager_state_t before, ota_manager_state_t after)
{
    if (s_event_cb != NULL && before != after)
    {
        s_event_cb(after);
    }
}

/* ---- the injected flash backend (esp_ota) ------------------------------------ */

/** A running image the bootloader still waits to hear about is confirmed
 *  before a new session: esp_ota_begin refuses otherwise with the rollback
 *  option on, and a restart into the new image would otherwise fall back
 *  to the one that was never confirmed. */
esp_err_t ota_manager_confirm_running(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;

    if (running == NULL ||
        esp_ota_get_state_partition(running, &st) != ESP_OK)
    {
        return ESP_ERR_NOT_FOUND;
    }

    if (st == ESP_OTA_IMG_VALID || st == ESP_OTA_IMG_UNDEFINED)
    {
        return ESP_OK; /* nothing to confirm */
    }

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "running image %s confirmed (was %s)", running->label,
                 ota_manager_img_state_str(st));
    }
    else
    {
        ESP_LOGW(TAG, "running image not confirmed: %s", esp_err_to_name(err));
    }

    return err;
}

static esp_err_t io_begin(void *io, size_t total_size)
{
    (void)io;
    s_target = esp_ota_get_next_update_partition(NULL);

    if (s_target == NULL)
    {
        ESP_LOGE(TAG, "no OTA partition available");
        return ESP_ERR_NOT_FOUND;
    }

    (void)ota_manager_confirm_running();

    ESP_LOGI(TAG, "update session -> %s (%s size, erased as written)",
             s_target->label, (total_size > 0) ? "announced" : "unknown");
    /* OTA_WITH_SEQUENTIAL_WRITES: esp_ota erases each sector right before
       its first write. Any other size erases the whole image size up front
       in ONE call (IDF esp_ota_begin), 4 to 5 s with the cache off for a
       3.8 MB image: IDLE0 starved and the task watchdog fired in the
       middle of an upload (the release test, 2026-10-21). The session
       writes in sequence (the held-back head goes first) and still
       enforces the announced size on overrun. */
    return esp_ota_begin(s_target, OTA_WITH_SEQUENTIAL_WRITES, &s_handle);
}

static esp_err_t io_write(void *io, const void *data, size_t len)
{
    (void)io;
    return esp_ota_write(s_handle, data, len);
}

static esp_err_t io_end(void *io)
{
    (void)io;

    esp_err_t err = esp_ota_end(s_handle); /* validates magic + digest */

    if (err == ESP_OK)
    {
        err = esp_ota_set_boot_partition(s_target);
    }

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "image valid; next boot from %s", s_target->label);
    }

    return err;
}

static void io_abort(void *io)
{
    (void)io;
    esp_ota_abort(s_handle);
    ESP_LOGW(TAG, "update session aborted");
}

static void io_hash_reset(void *io)
{
    (void)io;

    if (s_hash_open)
    {
        psa_hash_abort(&s_hash);
    }

    s_hash = psa_hash_operation_init();
    s_hash_open = psa_hash_setup(&s_hash, PSA_ALG_SHA_256) == PSA_SUCCESS;

    if (!s_hash_open)
    {
        ESP_LOGW(TAG, "SHA-256 not available: the digest check is off");
    }
}

static void io_hash_update(void *io, const void *data, size_t len)
{
    (void)io;

    if (s_hash_open && psa_hash_update(&s_hash, data, len) != PSA_SUCCESS)
    {
        psa_hash_abort(&s_hash);
        s_hash_open = false;
    }
}

static void io_hash_final(void *io, uint8_t out[32])
{
    size_t n = 0;

    (void)io;
    memset(out, 0, 32);

    if (s_hash_open)
    {
        (void)psa_hash_finish(&s_hash, out, 32, &n);
        s_hash_open = false;
    }
}

static const ota_session_ops_t IO_OPS =
{
    .begin = io_begin,
    .write = io_write,
    .end = io_end,
    .abort = io_abort,
    .hash_reset = io_hash_reset,
    .hash_update = io_hash_update,
    .hash_final = io_hash_final,
};

/* ---- lifecycle ----------------------------------------------------------------- */

esp_err_t ota_manager_init(void)
{
    if (s_inited)
    {
        return ESP_OK;
    }

    static const log_descriptor_t LOG_DESC = { "ota_manager", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    ota_session_init(&s_session, &IO_OPS, NULL);
    (void)psa_crypto_init();
    s_inited = true;
    return ESP_OK;
}

esp_err_t ota_manager_start(void)
{
    if (!s_inited)
    {
        return ESP_ERR_INVALID_STATE;
    }

    /* the confirm of a pending image is fw_update's (the verify milestone),
       not a boot-time formality: a run that confirmed itself at start could
       never be rolled back by the bootloader */
    ESP_LOGI(TAG, "ready (running from %s, image check %s)",
             esp_ota_get_running_partition()->label,
             s_session.check_head ? s_session.product : "off");
    return ESP_OK;
}

esp_err_t ota_manager_stop(void)
{
    return ota_manager_abort();
}

esp_err_t ota_manager_set_product(const char *product,
                                  const char *const *prefixes, size_t count)
{
    if (!s_inited || product == NULL || prefixes == NULL || count == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    ota_session_set_product(&s_session, product, prefixes, count,
                            OTA_SESSION_CHIP_ESP32S3);
    return ESP_OK;
}

esp_err_t ota_manager_expect_sha256(const uint8_t sha256[32])
{
    if (!s_inited)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_expect_armed = sha256 != NULL;

    if (s_expect_armed)
    {
        memcpy(s_expect, sha256, sizeof(s_expect));
    }

    xSemaphoreGive(s_lock);
    return ESP_OK;
}

/* ---- session API (mutex-serialized; transports call these) --------------------- */

esp_err_t ota_manager_begin(size_t total_size)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);

    ota_manager_state_t before = s_session.state;
    const uint8_t *expected = s_expect_armed ? s_expect : NULL;
    esp_err_t err = ota_session_begin_ex(&s_session, total_size, expected);
    ota_manager_state_t after = s_session.state;

    if (err != ESP_ERR_INVALID_STATE)
    {
        s_expect_armed = false; /* consumed by this session, hit or miss */
    }

    xSemaphoreGive(s_lock);
    notify(before, after);
    return err;
}

esp_err_t ota_manager_write(const uint8_t *data, size_t len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);

    ota_manager_state_t before = s_session.state;
    esp_err_t err = ota_session_write(&s_session, data, len);
    ota_manager_state_t after = s_session.state;

    xSemaphoreGive(s_lock);
    notify(before, after);
    return err;
}

esp_err_t ota_manager_end(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);

    ota_manager_state_t before = s_session.state;
    esp_err_t err = ota_session_end(&s_session);
    ota_manager_state_t after = s_session.state;

    xSemaphoreGive(s_lock);
    notify(before, after);
    return err;
}

esp_err_t ota_manager_abort(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);

    ota_manager_state_t before = s_session.state;
    esp_err_t err = ota_session_abort(&s_session);
    ota_manager_state_t after = s_session.state;

    xSemaphoreGive(s_lock);
    notify(before, after);
    return err;
}

esp_err_t ota_manager_set_event_cb(ota_manager_event_cb_t cb)
{
    s_event_cb = cb; /* pre-start composition glue; one consumer */
    return ESP_OK;
}

esp_err_t ota_manager_status(ota_manager_status_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->state = s_session.state;
    out->received = s_session.received;
    out->total = s_session.total;
    strlcpy(out->error, s_session.error, sizeof(out->error));
    strlcpy(out->target, (s_target != NULL) ? s_target->label : "",
            sizeof(out->target));
    strlcpy(out->project_name, s_session.project_name,
            sizeof(out->project_name));
    strlcpy(out->version, s_session.version, sizeof(out->version));
    out->piped = ota_pipe_active();
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

const char *ota_manager_img_state_str(int esp_ota_img_state)
{
    switch (esp_ota_img_state)
    {
        case ESP_OTA_IMG_NEW:            return "new";
        case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
        case ESP_OTA_IMG_VALID:          return "valid";
        case ESP_OTA_IMG_INVALID:        return "invalid";
        case ESP_OTA_IMG_ABORTED:        return "aborted";
        case ESP_OTA_IMG_UNDEFINED:      return "undefined";
        default:                         return "?";
    }
}

/* ---- the pipe: the public face of ota_pipe.c ---------------------------------- */

esp_err_t ota_manager_pipe_begin(size_t total_size,
                                 const uint8_t *expected_sha256)
{
    return s_inited ? ota_pipe_begin(total_size, expected_sha256)
                    : ESP_ERR_INVALID_STATE;
}

esp_err_t ota_manager_pipe_write(const uint8_t *data, size_t len,
                                 uint32_t wait_ms)
{
    return ota_pipe_write(data, len, wait_ms);
}

esp_err_t ota_manager_pipe_end(uint32_t wait_ms)
{
    return ota_pipe_end(wait_ms);
}

esp_err_t ota_manager_pipe_abort(void)
{
    return ota_pipe_abort();
}
