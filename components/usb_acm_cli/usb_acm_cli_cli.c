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
 * @file usb_acm_cli_cli.c
 * @brief `acm` console command: status, or send a line to the modem.
 *        `acm`            -> connection status
 *        `acm AT+CSQ`     -> send the line, print the response
 */
#include <stdio.h>
#include <string.h>

#include "cmdline_manager.h"

#include "usb_acm_cli.h"
#include "usb_acm_cli_private.h"

static int cmd_acm(int argc, char **argv)
{
    if (argc < 2)
    {
        cmdline_printf("ACM console: %s\n",
                       usb_acm_cli_connected() ? "connected" : "no device");
        cmdline_printf("usage: acm <AT command>\nOK\n");
        return 0;
    }

    if (!usb_acm_cli_console_enabled())
    {
        cmdline_printf("the ESPNetLink console is off in settings "
                       "(usb_acm_cli.console)\n");
        return 1;
    }

    if (!usb_acm_cli_connected())
    {
        cmdline_printf("no CDC-ACM device attached\n");
        return 1;
    }

    /* join argv[1..] into one line */
    char line[192];
    size_t o = 0;

    for (int i = 1; i < argc && o < sizeof(line) - 1; i++)
    {
        int n = snprintf(line + o, sizeof(line) - o, "%s%s",
                         (i > 1) ? " " : "", argv[i]);
        o += (n > 0) ? (size_t)n : 0;
    }

    static char resp[1024];
    size_t rn = 0;

    /* cap, not latency: collection ends at the dongle's prompt */
    esp_err_t err = usb_acm_cli_command(line, resp, sizeof(resp), &rn, 8000);

    if (err != ESP_OK)
    {
        cmdline_printf("acm error: %s\n", esp_err_to_name(err));
        return 1;
    }

    if (rn > 0)
    {
        cmdline_write(resp, rn); /* raw: lte -j etc. exceed printf's buf */
        cmdline_printf("\n");
    }
    else
    {
        cmdline_printf("(no response)\n");
    }

    cmdline_printf("OK\n");
    return 0;
}

esp_err_t usb_acm_cli_register_cli(void)
{
    static const esp_console_cmd_t CMD =
    {
        .command = "acm",
        .help = "CDC-ACM modem console: acm <AT command>",
        .func = cmd_acm,
    };

    return cmdline_manager_register(&CMD);
}
