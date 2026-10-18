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
 * @file fw_update_settings.c
 * @brief settings_manager descriptor for fw_update (schema version 1,
 *        reboot-to-apply): the field table, on_validate (the URL rules,
 *        the certificate set's name), on_apply (the boot-applied config
 *        and the console command behind `cli`).
 */
#include "fw_update_private.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"

#include "settings_manager.h"

static const char *TAG = FWU_TAG;

/* clang-format off */
static const settings_field_t FIELDS[] =
{
    SETTINGS_BOOL    ("enabled",          true),
    SETTINGS_STR_ENUM("channel",          "stable,beta,alfa", "stable"),
    SETTINGS_INT     ("check_interval_h", 1, 168, 24),
    SETTINGS_STR_LEN ("base_url",         8, 128, FWU_DEFAULT_BASE_URL),
    SETTINGS_STR     ("cert_set",         24, ""),
    SETTINGS_BOOL    ("cli",              true),
};
/* clang-format on */

static fwu_config_t s_cfg EXT_RAM_BSS_ATTR;
static bool s_configured;

static const char *str_of(const cJSON *o, const char *key, const char *def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

    return (cJSON_IsString(v) && v->valuestring != NULL) ? v->valuestring : def;
}

static bool cert_set_name_ok(const char *s)
{
    for (; *s != '\0'; s++)
    {
        unsigned char c = (unsigned char)*s;

        if (!(islower(c) || isdigit(c) || c == '_' || c == '-'))
        {
            return false;
        }
    }

    return true;
}

/** The base URL: https, or http only away from the default (a bench
 *  root), a trailing slash, printable ASCII. */
static esp_err_t check(const cJSON *settings, char *err, size_t err_len)
{
    const char *base = str_of(settings, "base_url", FWU_DEFAULT_BASE_URL);
    const char *cert = str_of(settings, "cert_set", "");
    bool http = false;

    if (!fwu_base_url_valid(base, &http))
    {
        snprintf(err, err_len, "base_url must be http(s)://host/... ending "
                 "with a slash, printable ASCII");
        return ESP_ERR_INVALID_ARG;
    }

    if (http && strcmp(base, FWU_DEFAULT_BASE_URL) == 0)
    {
        snprintf(err, err_len, "the default manifest root is https only");
        return ESP_ERR_INVALID_ARG;
    }

    if (!cert_set_name_ok(cert))
    {
        snprintf(err, err_len, "cert_set: a-z, 0-9, _ and - only");
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static esp_err_t on_validate(const cJSON *settings, char *err, size_t err_len)
{
    return check(settings, err, err_len);
}

static esp_err_t on_apply(const cJSON *settings)
{
    char err[96] = "";
    esp_err_t rc = check(settings, err, sizeof(err));

    if (rc != ESP_OK)
    {
        ESP_LOGW(TAG, "stored settings rejected: %s", err);
        return rc; /* the manager retries with defaults (§4.3 step 4) */
    }

    const cJSON *v;
    fwu_config_t c;

    memset(&c, 0, sizeof(c));
    v = cJSON_GetObjectItemCaseSensitive(settings, "enabled");
    c.enabled = !cJSON_IsFalse(v);

    int channel = fwu_channel_from_str(str_of(settings, "channel", "stable"));

    c.channel = (channel < 0 || channel > (int)FWU_CH_ALFA) ? FWU_CH_STABLE
                                                            : (uint8_t)channel;

    v = cJSON_GetObjectItemCaseSensitive(settings, "check_interval_h");
    c.check_interval_h = cJSON_IsNumber(v) ? (uint32_t)v->valuedouble : 24U;

    if (c.check_interval_h < 1U || c.check_interval_h > 168U)
    {
        c.check_interval_h = 24U;
    }

    strlcpy(c.base_url, str_of(settings, "base_url", FWU_DEFAULT_BASE_URL),
            sizeof(c.base_url));
    (void)fwu_base_url_valid(c.base_url, &c.base_is_http);
    strlcpy(c.cert_set, str_of(settings, "cert_set", ""), sizeof(c.cert_set));

    s_cfg = c;
    s_configured = true;

    /* CLI ownership: settings-gated self-registration (standard §6b) */
    if (!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(settings, "cli")))
    {
        static bool s_cli_registered;

        if (!s_cli_registered && fw_update_register_cli() == ESP_OK)
        {
            s_cli_registered = true;
        }
    }

    return ESP_OK;
}

esp_err_t fwu_settings_register(void)
{
    static const settings_descriptor_t DESC =
    {
        .name        = "fw_update",
        .version     = 1,
        .fields      = FIELDS,
        .field_count = sizeof(FIELDS) / sizeof(FIELDS[0]),
        .on_apply    = on_apply,
        .on_validate = on_validate,
    };

    return settings_manager_register(&DESC);
}

const fwu_config_t *fwu_settings_config(void)
{
    return &s_cfg;
}

bool fwu_settings_is_configured(void)
{
    return s_configured;
}
