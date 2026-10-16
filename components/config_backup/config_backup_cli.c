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
 * @file config_backup_cli.c
 * @brief `backup`: the status line (the key by date and state, the copies,
 *        the parts), `-r` refreshes the copies now. No file over the
 *        console, no secret in the output. Self-registered on the settings
 *        apply behind the `cli` setting (standard 6b).
 */
#include <string.h>

#include "argtable3/argtable3.h"
#include "cmdline_manager.h"

#include "config_backup_private.h"

static struct
{
    struct arg_lit *refresh;
    struct arg_end *end;
} s_args;

static void print_copy(const char *label, const cJSON *c)
{
    const cJSON *present = cJSON_GetObjectItemCaseSensitive(c, "present");
    const cJSON *ref = cJSON_GetObjectItemCaseSensitive(c, "refreshed");
    const cJSON *bytes = cJSON_GetObjectItemCaseSensitive(c, "bytes");
    const cJSON *parts = cJSON_GetObjectItemCaseSensitive(c, "parts");
    const cJSON *card = cJSON_GetObjectItemCaseSensitive(c, "card");

    if (cJSON_IsFalse(card))
    {
        cmdline_printf("  %s: no card\n", label);
        return;
    }

    if (!cJSON_IsTrue(present))
    {
        cmdline_printf("  %s: no copy yet\n", label);
        return;
    }

    cmdline_printf("  %s: %u parts, %u bytes, refreshed %s\n", label,
                   cJSON_IsNumber(parts) ? (unsigned)parts->valuedouble : 0u,
                   cJSON_IsNumber(bytes) ? (unsigned)bytes->valuedouble : 0u,
                   (cJSON_IsString(ref) && ref->valuestring[0])
                       ? ref->valuestring : "(clock unknown)");

    const cJSON *held = cJSON_GetObjectItemCaseSensitive(c, "held");
    const cJSON *since = cJSON_GetObjectItemCaseSensitive(c, "held_since");

    if (cJSON_IsTrue(held))
    {
        /* issue #987: the settings part as it was before the factory reset */
        cmdline_printf("    holds the settings from before the factory reset"
                       "%s%s (restore them to undo it; -r lets the copy "
                       "follow the device again)\n",
                       (cJSON_IsString(since) && since->valuestring[0])
                           ? " of " : "",
                       (cJSON_IsString(since) && since->valuestring[0])
                           ? since->valuestring : "");
    }
}

static int cmd_backup(int argc, char **argv)
{
    int errors = arg_parse(argc, argv, (void **)&s_args);

    if (errors != 0)
    {
        arg_print_errors(stdout, s_args.end, argv[0]);
        return 1;
    }

    if (s_args.refresh->count > 0)
    {
        cJSON    *written = cJSON_CreateArray();
        esp_err_t err = cb_copies_refresh(CB_REFRESH_WAIT_MS, written);

        if (err != ESP_OK)
        {
            cJSON_Delete(written);
            cmdline_printf("refresh failed: %s\n", esp_err_to_name(err));
            return 1;
        }

        cmdline_printf("copies refreshed (%d places)\n",
                       cJSON_GetArraySize(written));
        cJSON_Delete(written);
    }

    const cb_key_info_t *k = cb_key_info();
    char                 iso[CB_ISO_LEN];

    cb_iso_from_epoch(k->created, iso);
    cmdline_printf("key: made %s, %s\n", iso[0] ? iso : "(clock unknown)",
                   k->downloaded ? "downloaded" : "never downloaded");
    cmdline_printf("copies: %s\n", cb_settings_auto_copy() ? "kept up to date"
                                                            : "manual only");

    cJSON *copies = cJSON_CreateObject();

    cb_copies_status(copies);
    print_copy("flash", cJSON_GetObjectItemCaseSensitive(copies, "internal"));
    print_copy("card ", cJSON_GetObjectItemCaseSensitive(copies, "sd"));
    cJSON_Delete(copies);

    for (int p = 0; p < CB_PART_COUNT; p++)
    {
        cJSON *o = cJSON_CreateObject();

        if (cb_doc_part_status((cb_part_t)p, o) == ESP_OK)
        {
            const cJSON *bytes = cJSON_GetObjectItemCaseSensitive(o, "bytes");

            const cJSON *lo = cJSON_GetObjectItemCaseSensitive(o, "left_out");
            int          left = cJSON_IsArray(lo) ? cJSON_GetArraySize(lo) : 0;

            /* issue #989: what the part leaves out, counted here, named by
               the status route and the UI */
            cmdline_printf("  part %-10s %u bytes%s%d%s\n", CB_PART_NAMES[p],
                           cJSON_IsNumber(bytes) ? (unsigned)bytes->valuedouble
                                                 : 0u,
                           left > 0 ? ", " : "", left,
                           left > 0 ? " left out" : "");
        }

        cJSON_Delete(o);
    }

    cmdline_printf("OK\n");
    return 0;
}

esp_err_t config_backup_register_cli(void)
{
    s_args.refresh = arg_lit0("r", "refresh", "refresh the copies now");
    s_args.end = arg_end(2);

    static const esp_console_cmd_t CMD =
    {
        .command = "backup",
        .help = "Configuration backup: key state, the copies on the device, "
                "the parts; -r = refresh the copies now",
        .hint = NULL,
        .func = cmd_backup,
        .argtable = &s_args,
    };

    return cmdline_manager_register(&CMD);
}
