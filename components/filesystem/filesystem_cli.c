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
 * @file filesystem_cli.c
 * @brief The component's CLI command (`fs`): registered into
 *        cmdline_manager by filesystem_register_cli() (main wires it
 *        in CLI compositions only). Reports each backend's capacity.
 */
#include "cmdline_manager.h"

#include "filesystem.h"

static void print_backend(const char *prefix)
{
    uint64_t total = 0;
    uint64_t used = 0;

    if (filesystem_info(prefix, &total, &used) != ESP_OK)
    {
        cmdline_printf("%-6s unavailable\n", prefix);
        return;
    }

    /* KB in 64 bits: a card is bigger than the 4 GiB an unsigned held
       (issue #982: the 16 GB bench card printed 2967584 KB) */
    cmdline_printf("%-6s %llu KB used / %llu KB total\n", prefix,
                   (unsigned long long)(used / 1024),
                   (unsigned long long)(total / 1024));
}

static int cmd_fs(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    print_backend("/data");
    print_backend("/sd");
    return 0;
}

esp_err_t filesystem_register_cli(void)
{
    static const esp_console_cmd_t CMD =
    {
        .command = "fs",
        .help = "Filesystem backends usage (/data, /sd)",
        .func = cmd_fs,
    };

    return cmdline_manager_register(&CMD);
}
