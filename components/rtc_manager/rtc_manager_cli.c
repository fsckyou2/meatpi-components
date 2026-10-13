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
 * @file rtc_manager_cli.c
 * @brief The component's CLI command (`rtc`): registered into
 *        cmdline_manager by rtc_manager_register_cli() (main wires it
 *        in CLI compositions only). Legacy option interface preserved
 *        (-s/--sync, -r/--read, -i/--id); bare = the v6 summary. The
 *        RX8130 has no ID register, so -i reports a responding probe
 *        (legacy read back a fixed register). `--forget` (2026-10-13) is
 *        the bench's drained-caps unit on demand.
 */
#include <time.h>

#include "argtable3/argtable3.h"

#include "cmdline_manager.h"

#include "rtc_manager.h"

static struct
{
    struct arg_lit *sync;
    struct arg_lit *read;
    struct arg_lit *id;
    struct arg_lit *forget;
    struct arg_end *end;
} s_args;

static int rtc_summary(void)
{
    char iso[24];
    struct tm rtc_tm;
    rtc_clock_state_t cs;

    if (rtc_manager_now_iso8601(iso, sizeof(iso)) == ESP_OK)
    {
        cmdline_printf("System time: %s (UTC)\n", iso);
    }

    if (rtc_manager_get_time(&rtc_tm) == ESP_OK)
    {
        cmdline_printf("RTC time:    %04d-%02d-%02dT%02d:%02d:%02dZ\n",
                       rtc_tm.tm_year + 1900, rtc_tm.tm_mon + 1,
                       rtc_tm.tm_mday, rtc_tm.tm_hour, rtc_tm.tm_min,
                       rtc_tm.tm_sec);
    }
    else
    {
        cmdline_printf("RTC time:    invalid (fresh board?)\n");
    }

    cmdline_printf("Time valid:  %s\n",
                   rtc_manager_time_valid() ? "yes" : "no");
    cmdline_printf("SNTP:        %s (%s), last sync %ld\n",
                   rtc_manager_sntp_enabled() ? "yes" : "no",
                   rtc_manager_ntp_server(),
                   (long)rtc_manager_last_sync());
    rtc_manager_clock_state(&cs);

    if (cs.steps_this_boot == 0)
    {
        cmdline_printf("Clock steps: none this boot (1970 + uptime)\n");
    }
    else
    {
        cmdline_printf("Clock steps: %lu this boot, last %+lld ms by %s "
                       "(generation %lu)\n",
                       (unsigned long)cs.steps_this_boot,
                       (long long)cs.last.delta_ms,
                       rtc_manager_source_name(cs.last.source),
                       (unsigned long)cs.generation);
    }

    cmdline_printf("OK\n");
    return 0;
}

static int cmd_rtc(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_args);

    if (nerrors != 0)
    {
        arg_print_errors(stderr, s_args.end, argv[0]);
        return 1;
    }

    if (s_args.sync->count > 0)
    {
        if (rtc_manager_sync_now() != ESP_OK)
        {
            cmdline_printf("Error: Failed to sync time\n");
            return 1;
        }

        cmdline_printf("Time synchronized successfully\n");
        cmdline_printf("OK\n");
        return 0;
    }

    if (s_args.read->count > 0)
    {
        struct tm t;

        if (rtc_manager_get_time(&t) != ESP_OK)
        {
            cmdline_printf("Error: Failed to read RTC time/date\n");
            return 1;
        }

        mktime(&t); /* TZ is pinned UTC0; recomputes tm_wday */
        cmdline_printf("%04d-%02d-%02d %02d:%02d:%02d (Day %d)\n",
                       t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                       t.tm_hour, t.tm_min, t.tm_sec, t.tm_wday);
        cmdline_printf("OK\n");
        return 0;
    }

    if (s_args.id->count > 0)
    {
        struct tm t;

        if (rtc_manager_get_time(&t) != ESP_OK)
        {
            cmdline_printf("Error: Failed to read RTC module\n");
            return 1;
        }

        cmdline_printf("RTC Module: RX8130CE (responding)\n");
        cmdline_printf("OK\n");
        return 0;
    }

    if (s_args.forget->count > 0)
    {
        if (rtc_manager_forget_chip() != ESP_OK)
        {
            cmdline_printf("Error: Failed to write the RTC chip\n");
            return 1;
        }

        cmdline_printf("RTC chip set to 2000-01-01: the next POWER-ON boot "
                       "starts without a clock (a software restart keeps "
                       "the running one)\n");
        cmdline_printf("OK\n");
        return 0;
    }

    return rtc_summary();
}

esp_err_t rtc_manager_register_cli(void)
{
    static const esp_console_cmd_t CMD =
    {
        .command = "rtc",
        .help = "RTC module control",
        .hint = "Options: -s/--sync, -r/--read, -i/--id, --forget",
        .func = cmd_rtc,
        .argtable = &s_args,
    };

    s_args.sync = arg_lit0("s", "sync", "Sync time from internet");
    s_args.read = arg_lit0("r", "read", "Read current time and date");
    s_args.id = arg_lit0("i", "id", "Get RTC module device ID");
    s_args.forget = arg_lit0(NULL, "forget",
                             "Write 2000-01-01 into the chip: the next "
                             "power-on boot has no clock (bench aid)");
    s_args.end = arg_end(4);
    return cmdline_manager_register(&CMD);
}
