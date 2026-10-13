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
 * @file rtc_manager_time.c
 * @brief PURE RX8130 register-image <-> struct tm codec, and the pure
 *        civil-to-epoch conversion. No I2C, no RTOS, no libc time:
 *        host-testable.
 */
#include "rtc_manager_private.h"

static int bcd_to_dec(uint8_t bcd)
{
    return ((bcd >> 4) & 0x0F) * 10 + (bcd & 0x0F);
}

static uint8_t dec_to_bcd(int dec)
{
    return (uint8_t)(((dec / 10) << 4) | (dec % 10));
}

bool rtc_time_decode(const uint8_t regs[RTC_REGS_LEN], struct tm *out)
{
    int sec = bcd_to_dec(regs[0] & 0x7F);
    int min = bcd_to_dec(regs[1] & 0x7F);
    int hour = bcd_to_dec(regs[2] & 0x3F);
    /* regs[3] = weekday (one-hot): ignored */
    int day = bcd_to_dec(regs[4] & 0x3F);
    int month = bcd_to_dec(regs[5] & 0x1F);
    int year = bcd_to_dec(regs[6]);

    /* plausibility gate: a fresh board / drained caps reads garbage or
     * 2000-01-01, both must NOT overwrite the system clock */
    if (sec > 59 || min > 59 || hour > 23 ||
        day < 1 || day > 31 || month < 1 || month > 12 ||
        year < 20 || year > 99)
    {
        return false;
    }

    *out = (struct tm)
    {
        .tm_sec = sec,
        .tm_min = min,
        .tm_hour = hour,
        .tm_mday = day,
        .tm_mon = month - 1,
        .tm_year = 100 + year, /* 20xx, years since 1900 */
        .tm_isdst = 0,         /* UTC */
    };
    return true;
}

void rtc_time_encode(const struct tm *in, uint8_t regs[RTC_REGS_LEN])
{
    regs[0] = dec_to_bcd(in->tm_sec);
    regs[1] = dec_to_bcd(in->tm_min);
    regs[2] = dec_to_bcd(in->tm_hour);
    regs[3] = (uint8_t)(1u << (in->tm_wday & 0x07)); /* one-hot weekday */
    regs[4] = dec_to_bcd(in->tm_mday);
    regs[5] = dec_to_bcd(in->tm_mon + 1);
    regs[6] = dec_to_bcd(in->tm_year % 100);
}

/* days from civil (Howard Hinnant's algorithm): proleptic Gregorian,
 * 1970-01-01 = day 0; exact for every date the chip can hold */
int64_t rtc_time_to_epoch(const struct tm *utc)
{
    int64_t y = (int64_t)utc->tm_year + 1900;
    int64_t m = (int64_t)utc->tm_mon + 1;
    int64_t d = utc->tm_mday;

    if (m <= 2)
    {
        y -= 1;
    }

    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;                                  /* 0..399 */
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; /* 0..365 */
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;          /* 0..146096 */
    int64_t days = era * 146097 + doe - 719468;

    return days * 86400 + (int64_t)utc->tm_hour * 3600 +
           (int64_t)utc->tm_min * 60 + utc->tm_sec;
}
