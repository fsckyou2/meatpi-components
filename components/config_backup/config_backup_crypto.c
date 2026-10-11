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
 * @file config_backup_crypto.c
 * @brief The backup key (made once from the hardware random source, kept
 *        in NVS outside the settings partition so a factory reset keeps
 *        it), AES-256-GCM sealing through the PSA API, HMAC-SHA256 for the
 *        manifest, base64.
 *
 * The key is never logged and never leaves this file except through
 * cb_device_key() (the sealing paths) and the key route (the file the user
 * asks for).
 */
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "psa/crypto.h"

#include "config_backup_private.h"

static const char *TAG = "config_backup";

#define NVS_NS       "cfg_backup"
#define NVS_KEY      "key"
#define NVS_CREATED  "created"
#define NVS_DL       "dl"
#define NVS_DL_AT    "dl_at"

static uint8_t       s_key[CB_KEY_LEN]; /* internal: never in PSRAM */
static bool          s_have_key;
static cb_key_info_t s_info;

/* ---- the key in NVS --------------------------------------------------------------- */

static esp_err_t key_load(void)
{
    nvs_handle_t h;
    esp_err_t    err = nvs_open(NVS_NS, NVS_READONLY, &h);

    if (err != ESP_OK)
    {
        return err; /* ESP_ERR_NVS_NOT_FOUND: never made */
    }

    size_t len = sizeof(s_key);

    err = nvs_get_blob(h, NVS_KEY, s_key, &len);

    if (err == ESP_OK && len != sizeof(s_key))
    {
        err = ESP_ERR_INVALID_SIZE;
    }

    if (err == ESP_OK)
    {
        int64_t v = 0;
        uint8_t dl = 0;

        if (nvs_get_i64(h, NVS_CREATED, &v) == ESP_OK)
        {
            s_info.created = v;
        }

        if (nvs_get_u8(h, NVS_DL, &dl) == ESP_OK)
        {
            s_info.downloaded = (dl != 0);
        }

        if (nvs_get_i64(h, NVS_DL_AT, &v) == ESP_OK)
        {
            s_info.downloaded_at = v;
        }
    }

    nvs_close(h);
    return err;
}

static esp_err_t key_store_new(void)
{
    nvs_handle_t h;
    esp_err_t    err = nvs_open(NVS_NS, NVS_READWRITE, &h);

    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_set_blob(h, NVS_KEY, s_key, sizeof(s_key));

    if (err == ESP_OK)
    {
        err = nvs_set_i64(h, NVS_CREATED, s_info.created);
    }

    if (err == ESP_OK)
    {
        err = nvs_set_u8(h, NVS_DL, 0);
    }

    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }

    nvs_close(h);
    return err;
}

esp_err_t cb_crypto_init(void)
{
    psa_status_t st = psa_crypto_init();

    if (st != PSA_SUCCESS)
    {
        ESP_LOGE(TAG, "psa_crypto_init failed (%d)", (int)st);
        return ESP_FAIL;
    }

    memset(&s_info, 0, sizeof(s_info));

    esp_err_t err = key_load();

    if (err == ESP_OK)
    {
        s_have_key = true;
        return ESP_OK;
    }

    if (err != ESP_ERR_NVS_NOT_FOUND && err != ESP_ERR_NVS_NOT_INITIALIZED)
    {
        ESP_LOGW(TAG, "stored key unreadable (%s): making a new one",
                 esp_err_to_name(err));
    }

    /* once per device: the hardware random source, event-driven write */
    esp_fill_random(s_key, sizeof(s_key));
    s_info.created = cb_now_epoch();
    s_info.downloaded = false;
    s_info.downloaded_at = 0;
    err = key_store_new();

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "key store failed: %s", esp_err_to_name(err));
        memset(s_key, 0, sizeof(s_key));
        return err;
    }

    s_have_key = true;
    ESP_LOGI(TAG, "backup key made");
    return ESP_OK;
}

const uint8_t *cb_device_key(void)
{
    return s_have_key ? s_key : NULL;
}

const cb_key_info_t *cb_key_info(void)
{
    return &s_info;
}

esp_err_t cb_key_mark_downloaded(void)
{
    if (!s_have_key)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_info.downloaded = true;
    s_info.downloaded_at = cb_now_epoch();

    nvs_handle_t h;
    esp_err_t    err = nvs_open(NVS_NS, NVS_READWRITE, &h);

    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_set_u8(h, NVS_DL, 1);

    if (err == ESP_OK)
    {
        err = nvs_set_i64(h, NVS_DL_AT, s_info.downloaded_at);
    }

    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }

    nvs_close(h);
    return err;
}

/* ---- AES-256-GCM through PSA ------------------------------------------------------ */

static psa_status_t import_aes(const uint8_t *key, psa_key_id_t *id)
{
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;

    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&a, PSA_ALG_GCM);
    psa_set_key_type(&a, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&a, 8 * CB_KEY_LEN);
    return psa_import_key(&a, key, CB_KEY_LEN, id);
}

esp_err_t cb_seal(const uint8_t *key, const char *aad, const uint8_t *in,
                  size_t in_len, uint8_t nonce[CB_NONCE_LEN], uint8_t *out,
                  size_t out_size, size_t *out_len)
{
    if (key == NULL || aad == NULL || out == NULL || out_len == NULL ||
        out_size < in_len + CB_TAG_LEN)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_fill_random(nonce, CB_NONCE_LEN);

    psa_key_id_t id = 0;
    psa_status_t st = import_aes(key, &id);

    if (st != PSA_SUCCESS)
    {
        return ESP_FAIL;
    }

    st = psa_aead_encrypt(id, PSA_ALG_GCM, nonce, CB_NONCE_LEN,
                          (const uint8_t *)aad, strlen(aad), in, in_len, out,
                          out_size, out_len);
    psa_destroy_key(id);
    return (st == PSA_SUCCESS) ? ESP_OK : ESP_FAIL;
}

esp_err_t cb_unseal(const uint8_t *key, const char *aad,
                    const uint8_t nonce[CB_NONCE_LEN], const uint8_t *in,
                    size_t in_len, uint8_t *out, size_t out_size,
                    size_t *out_len)
{
    if (key == NULL || aad == NULL || in == NULL || out == NULL ||
        out_len == NULL || in_len < CB_TAG_LEN ||
        out_size < in_len - CB_TAG_LEN)
    {
        return ESP_ERR_INVALID_ARG;
    }

    psa_key_id_t id = 0;
    psa_status_t st = import_aes(key, &id);

    if (st != PSA_SUCCESS)
    {
        return ESP_FAIL;
    }

    st = psa_aead_decrypt(id, PSA_ALG_GCM, nonce, CB_NONCE_LEN,
                          (const uint8_t *)aad, strlen(aad), in, in_len, out,
                          out_size, out_len);
    psa_destroy_key(id);

    if (st == PSA_ERROR_INVALID_SIGNATURE)
    {
        return ESP_ERR_INVALID_CRC; /* wrong key, or a touched file */
    }

    return (st == PSA_SUCCESS) ? ESP_OK : ESP_FAIL;
}

esp_err_t cb_hmac(const uint8_t *key, const void *data, size_t len,
                  uint8_t out[CB_HMAC_LEN])
{
    if (key == NULL || out == NULL || (data == NULL && len > 0))
    {
        return ESP_ERR_INVALID_ARG;
    }

    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t         id = 0;

    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&a, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_type(&a, PSA_KEY_TYPE_HMAC);

    psa_status_t st = psa_import_key(&a, key, CB_KEY_LEN, &id);

    if (st != PSA_SUCCESS)
    {
        return ESP_FAIL;
    }

    size_t mac_len = 0;

    st = psa_mac_compute(id, PSA_ALG_HMAC(PSA_ALG_SHA_256), data, len, out,
                         CB_HMAC_LEN, &mac_len);
    psa_destroy_key(id);
    return (st == PSA_SUCCESS && mac_len == CB_HMAC_LEN) ? ESP_OK : ESP_FAIL;
}

/* ---- base64 --------------------------------------------------------------------------- */

size_t cb_b64_size(size_t n)
{
    return ((n + 2) / 3) * 4 + 1;
}

esp_err_t cb_b64_encode(const uint8_t *in, size_t n, char *out,
                        size_t out_size, size_t *out_len)
{
    size_t olen = 0;
    int    r = mbedtls_base64_encode((unsigned char *)out, out_size, &olen, in,
                                     n);

    if (out_len != NULL)
    {
        *out_len = olen;
    }

    return (r == 0) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t cb_b64_decode(const char *in, size_t n, uint8_t *out,
                        size_t out_size, size_t *out_len)
{
    size_t olen = 0;
    int    r = mbedtls_base64_decode(out, out_size, &olen,
                                     (const unsigned char *)in, n);

    if (out_len != NULL)
    {
        *out_len = olen;
    }

    if (r == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    return (r == 0) ? ESP_OK : ESP_ERR_INVALID_ARG;
}
