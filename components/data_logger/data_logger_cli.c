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
 * @file data_logger_cli.c
 * @brief The `logger` CLI command: registered by
 *        data_logger_register_cli() on the settings apply (§6b).
 *        `logger test <rows>` queues synthetic param records and
 *        `logger frametest <n>` synthetic CAN frames (bench smoke +
 *        engine throughput without a bus: run `logger` afterwards and
 *        watch `written`/`frames` move).
 */
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cmdline_manager.h"

#include "data_logger.h"
#include "data_logger_private.h"

static int cmd_logger_test(int rows)
{
    static dl_param_t s_param = -1;

    if (rows < 1 || rows > 20000)
    {
        cmdline_printf("rows must be 1..20000\n");
        return 1;
    }

    if (s_param < 0 &&
        data_logger_register_param("test", "value", &s_param) != ESP_OK)
    {
        cmdline_printf("param registration failed\n");
        return 1;
    }

    int queued = 0;

    for (int i = 0; i < rows; i++)
    {
        data_logger_stats_t st;

        /* pace against the writer so big counts land instead of
         * cycling the drop-oldest ring (bench realism, not a stress
         * of the backpressure path) */
        while (data_logger_stats(&st) == ESP_OK && st.queued > 384)
        {
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        if (data_logger_write(s_param, (double)i * 0.5) == ESP_OK)
        {
            queued++;
        }
    }

    cmdline_printf("queued %d/%d test.value records\n", queued, rows);
    return 0;
}

static int cmd_logger_frametest(int n)
{
    if (n < 1 || n > 20000)
    {
        cmdline_printf("frames must be 1..20000\n");
        return 1;
    }

    int queued = 0;

    while (queued < n)
    {
        data_logger_stats_t st;

        /* pace against the writer (same rationale as `logger test`) */
        while (data_logger_stats(&st) == ESP_OK &&
               st.can_queued > 1024)
        {
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        int chunk = (n - queued > 256) ? 256 : (n - queued);

        if (dl_can_test_push(chunk) != ESP_OK)
        {
            cmdline_printf("queued %d/%d: CAN stream off "
                           "(enabled + can_log required)\n", queued, n);
            return 1;
        }

        queued += chunk;
    }

    cmdline_printf("queued %d/%d synthetic frames\n", queued, n);
    return 0;
}

static int cmd_logger(int argc, char **argv)
{
    data_logger_stats_t st;

    if (argc >= 3 && strcmp(argv[1], "test") == 0)
    {
        return cmd_logger_test(atoi(argv[2]));
    }

    if (argc >= 3 && strcmp(argv[1], "frametest") == 0)
    {
        return cmd_logger_frametest(atoi(argv[2]));
    }

    (void)data_logger_stats(&st);

    cmdline_printf("logger: %s%s%s, storage %s\n",
                   st.enabled ? "enabled" : "disabled",
                   st.running ? " (running)" : "",
                   st.paused ? " (paused by rule)" : "",
                   st.storage_ok ? "OK" : "unavailable");
    cmdline_printf("  params: %s (%lu rows), %lu files; written %lu, "
                   "dropped %lu\n",
                   st.file[0] ? st.file : "-",
                   (unsigned long)st.file_rows,
                   (unsigned long)st.files,
                   (unsigned long)st.written,
                   (unsigned long)st.dropped);
    cmdline_printf("  can:    %s%s (%lu frames), %lu files; written "
                   "%lu, dropped %lu\n",
                   st.can_enabled ? "" : "(off) ",
                   st.can_file[0] ? st.can_file : "-",
                   (unsigned long)st.can_file_rows,
                   (unsigned long)st.can_files,
                   (unsigned long)st.frames_written,
                   (unsigned long)st.frames_dropped);
    cmdline_printf("  queued %lu+%lu, errors %lu, rotations %lu+%lu\n",
                   (unsigned long)st.queued,
                   (unsigned long)st.can_queued,
                   (unsigned long)st.errors,
                   (unsigned long)st.rotations,
                   (unsigned long)st.can_rotations);
    cmdline_printf("  clock:  %s (%s), %lu step(s) this boot, %lu rotation(s) "
                   "by the clock, journal %lu B\n",
                   st.clock_valid ? "valid" : "not set",
                   st.clock_source, (unsigned long)st.clock_steps,
                   (unsigned long)st.clock_rotations,
                   (unsigned long)st.journal_bytes);
    return 0;
}

esp_err_t data_logger_register_cli(void)
{
    static const esp_console_cmd_t CMD =
    {
        .command = "logger",
        .help = "Data logger status; `logger test <rows>` / "
                "`logger frametest <n>` queue synthetic records",
        .func = cmd_logger,
    };

    return cmdline_manager_register(&CMD);
}
