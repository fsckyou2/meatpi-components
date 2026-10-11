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
 * @file usb_acm_cli_poll.c
 * @brief The GPS fix cache, its readers, and the ESPNetLink console poll task.
 *
 * Two writers fill ONE cache: the console poll task here (the dongle's
 * `gps -p -j` every 5 s, the legacy usb_host cadence; only while the
 * console switch is on and the bound device is not a receiver) and the RX
 * task in usb_acm_cli.c (a receiver's NMEA sentences as they arrive). Two
 * surfaces read it, never with any ACM I/O: `GET /api/gps` and
 * usb_acm_cli_gps_get(), which falls back to espnetlink_link's HTTP-polled
 * fix when the cache has no live one (main wires the fallback). Every fresh
 * fix also goes to the sink main wired (the autopid publisher).
 *
 * Split out of usb_acm_cli.c on 2026-10-11 (the 700-line rule).
 */
#include "usb_acm_cli.h"

#include <string.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "usb_acm_cli_private.h"

/* A valid fix older than ACM_GPS_MAX_AGE_MS is reported as no-fix (the
 * poll went silent, or the receiver stopped talking). */
#define ACM_GPS_POLL_MS    5000
#define ACM_GPS_MAX_AGE_MS 20000
#define ACM_GPS_STACK      4096
#define ACM_GPS_PRIO       3

static usb_acm_gps_t s_gps;
static uint32_t s_gps_stamp_ms;
static SemaphoreHandle_t s_gps_lock;
static StaticSemaphore_t s_gps_lock_buf;
static usb_acm_gps_sink_t s_gps_sink;
static usb_acm_gps_source_t s_gps_fallback;

/* PSRAM-stacked poll task: USB + a pure parse only, no flash */
static TaskHandle_t s_gps_task;
static StaticTask_t s_gps_tcb;
EXT_RAM_BSS_ATTR static StackType_t s_gps_stack[ACM_GPS_STACK];
static volatile bool s_gps_run;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* ---- the cache ---------------------------------------------------------------- */

void usb_acm_cli_gps_cache_set(const usb_acm_gps_t *fix)
{
    if (s_gps_lock == NULL)
    {
        return;
    }

    xSemaphoreTake(s_gps_lock, portMAX_DELAY);
    if (fix != NULL)
    {
        s_gps = *fix;
    }
    else
    {
        s_gps.valid = false;
    }
    s_gps_stamp_ms = now_ms();
    xSemaphoreGive(s_gps_lock);
}

void usb_acm_cli_gps_publish(const usb_acm_gps_t *fix)
{
    if (s_gps_sink != NULL && fix != NULL)
    {
        s_gps_sink(fix); /* -> autopid publisher (main-wired) */
    }
}

void usb_acm_cli_set_gps_sink(usb_acm_gps_sink_t sink)
{
    s_gps_sink = sink;
}

void usb_acm_cli_set_gps_fallback(usb_acm_gps_source_t source)
{
    s_gps_fallback = source;
}

esp_err_t usb_acm_cli_gps_get(usb_acm_gps_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_gps_lock == NULL)
    {
        memset(out, 0, sizeof(*out));
    }
    else
    {
        xSemaphoreTake(s_gps_lock, portMAX_DELAY);
        *out = s_gps;
        uint32_t age = now_ms() - s_gps_stamp_ms;
        xSemaphoreGive(s_gps_lock);

        /* a fix nobody refreshed (the device gone, or quiet) is stale */
        if (out->valid && (!usb_acm_cli_connected() ||
                           age > ACM_GPS_MAX_AGE_MS))
        {
            memset(out, 0, sizeof(*out));
        }
        else
        {
            out->age_ms = out->valid ? age : 0;
        }
    }

    /* no live fix of our own: ask the other GPS source (espnetlink_link's
     * HTTP poll, the WiFi-modem topology has no console at all) */
    if (!out->valid && s_gps_fallback != NULL)
    {
        usb_acm_gps_t alt;

        if (s_gps_fallback(&alt) == ESP_OK && alt.valid)
        {
            *out = alt;
        }
    }

    return ESP_OK;
}

/* ---- the ESPNetLink console poll ----------------------------------------------- */

static void gps_task(void *arg)
{
    (void)arg;
    static char resp[768]; /* lte -j is the biggest; gps -p -j ~410 B */

    /* the task never exits: stop() just idles it (s_gps_run false), so a
     * stop()/start() cycle can't lose the poll to a teardown race */
    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(ACM_GPS_POLL_MS));

        /* a receiver streaming NMEA is read by the RX task: no command */
        if (!s_gps_run || !usb_acm_cli_connected() ||
            strcmp(usb_acm_cli_mode(), "nmea") == 0)
        {
            continue;
        }

        size_t rn = 0;

        if (usb_acm_cli_command("gps -p -j", resp, sizeof(resp), &rn,
                                3000) != ESP_OK)
        {
            continue; /* console busy this round: try again next tick */
        }

        usb_acm_gps_t fix;

        if (!usb_acm_gps_parse(resp, &fix))
        {
            /* no live fix: mark the cache invalid so readers see no-fix */
            usb_acm_cli_gps_cache_set(NULL);
            continue;
        }

        usb_acm_cli_gps_cache_set(&fix);
        usb_acm_cli_gps_publish(&fix);
    }
}

esp_err_t usb_acm_cli_poll_init(void)
{
    if (s_gps_lock == NULL)
    {
        s_gps_lock = xSemaphoreCreateMutexStatic(&s_gps_lock_buf);
    }

    return (s_gps_lock != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t usb_acm_cli_poll_start(void)
{
    s_gps_run = true;

    if (s_gps_task == NULL)
    {
        s_gps_task = xTaskCreateStatic(gps_task, "usb_acm_gps",
                                       ACM_GPS_STACK, NULL, ACM_GPS_PRIO,
                                       s_gps_stack, &s_gps_tcb);
    }

    return (s_gps_task != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

void usb_acm_cli_poll_stop(void)
{
    s_gps_run = false;
}
