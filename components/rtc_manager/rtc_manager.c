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
 * @file rtc_manager.c
 * @brief Lifecycle, boot-time system-clock restore, the SNTP sync task and
 *        the clock-step bookkeeping (see include/rtc_manager.h for the
 *        model). Settings live in rtc_manager_settings.c (standard 4.1).
 *
 * Every set of the system clock goes through clock_note_locked(): the
 * boot restore and the manual set write the clock here, lwIP's SNTP
 * writes it itself, and all three are measured the same way (the change
 * of wall - mono, esp_timer being the monotonic reference), counted and
 * published as `time.step`.
 */
#include "rtc_manager.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "dev_status_manager.h"
#include "log_manager.h"

#include "rtc_manager_private.h"

static const char *TAG = "rtc_manager";


static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf; /* internal: FreeRTOS object */
static bool s_started;
static bool s_time_valid;
static rtcm_clock_t s_clock;         /* the step bookkeeping, under s_lock */

static time_t s_last_sync;              /* epoch of the last SNTP sync */
static SemaphoreHandle_t s_sntp_lock;   /* task vs /api/rtc/sync       */
static StaticSemaphore_t s_sntp_lock_buf; /* internal: FreeRTOS object */

/* SNTP task (PSRAM stack: network only, no flash writes) */
static TaskHandle_t s_task;
static StaticTask_t s_tcb;                            /* internal: FreeRTOS */
static StackType_t s_stack[4096] EXT_RAM_BSS_ATTR;

/* ---- helpers ---------------------------------------------------------------- */

static int64_t wall_ms_now(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static int64_t mono_ms_now(void)
{
    return esp_timer_get_time() / 1000;
}

static esp_err_t write_rtc_from_system(void)
{
    time_t now = time(NULL);
    struct tm utc;
    uint8_t regs[RTC_REGS_LEN];

    gmtime_r(&now, &utc);
    rtc_time_encode(&utc, regs);
    return rtc_rx8130_write(regs);
}

/* The one place a set of the system clock is recorded (issue #984). The
 * writer may be this component (settimeofday just before) or lwIP's SNTP
 * (settimeofday at the reply): the step is the change of wall - mono
 * either way. s_lock held by the caller. */
static void clock_note_locked(uint8_t source, rtc_clock_step_t *out)
{
    rtcm_clock_note(&s_clock, wall_ms_now(), mono_ms_now(), source, out);
    s_time_valid = true;
}

/* after the lock is released: the status bit and the event */
static void clock_announce(const rtc_clock_step_t *step)
{
    dev_status_manager_set(DEV_STATUS_BIT_TIME_SYNCED);
    rtcm_events_step(step);
}

/* ---- SNTP task -------------------------------------------------------------
 * Sync policy (meatpi 2026-07-04): sync system time + RTC on EVERY
 * internet (re)connect (not just the first) plus a configurable
 * periodic resync while the link stays up. Failed attempts retry in
 * 60 s. */

/** One SNTP attempt against @p server (~@p tries x 2 s). */
static bool sntp_try_server(const char *server, int tries)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(server);

    cfg.start = true;

    if (esp_netif_sntp_init(&cfg) != ESP_OK)
    {
        return false;
    }

    esp_err_t err = ESP_ERR_TIMEOUT;

    for (int i = 0; i < tries && err == ESP_ERR_TIMEOUT; i++)
    {
        err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(2000));
    }

    bool ok = (err == ESP_OK);

    /* sanity floor (legacy MIN_VALID_TIME idea): a server answering
     * with a bogus early date must not poison the clock or the RTC */
    if (ok && time(NULL) < 1577836800LL) /* 2020-01-01 */
    {
        ESP_LOGW(TAG, "%s answered with an implausible time; ignored",
                 server);
        ok = false;
    }

    if (ok)
    {
        rtc_clock_step_t step;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        write_rtc_from_system();
        clock_note_locked(RTC_CLOCK_SRC_SNTP, &step);
        s_last_sync = time(NULL);
        xSemaphoreGive(s_lock);
        clock_announce(&step);

        char stamp[24];

        rtc_manager_now_iso8601(stamp, sizeof(stamp));
        ESP_LOGI(TAG, "SNTP synced via %s, RTC updated (%s): the clock "
                 "moved %+lld ms", server, stamp, (long long)step.delta_ms);
    }
    else
    {
        ESP_LOGD(TAG, "SNTP sync via %s failed", server);
    }

    esp_netif_sntp_deinit();
    return ok;
}

/** Primary server, then the optional fallback. Serialized (the sync
 *  task and POST /api/rtc/sync may race). */
static bool sntp_sync_once(void)
{
    const rtcm_config_t *cfg = rtcm_settings_config();

    xSemaphoreTake(s_sntp_lock, portMAX_DELAY);

    bool ok = sntp_try_server(cfg->ntp_server, 8);

    if (!ok && cfg->ntp_server2[0] != '\0')
    {
        ok = sntp_try_server(cfg->ntp_server2, 8);
    }

    xSemaphoreGive(s_sntp_lock);
    return ok;
}

static void sntp_task(void *arg)
{
    (void)arg;

    while (true)
    {
        dev_status_manager_wait_any(DEV_STATUS_NETWORK_CONNECTED_MASK,
                                    portMAX_DELAY);

        bool synced = sntp_sync_once();

        /* stay here while the link is up; leave on DISCONNECT (the next
         * reconnect edge re-syncs immediately at the loop top) or when
         * the periodic interval / retry timer expires */
        uint32_t resync_ms = synced
                                 ? rtcm_settings_config()->sync_interval_h *
                                       3600u * 1000u
                                 : 60u * 1000u;
        uint32_t waited_ms = 0;

        while (dev_status_manager_any_set(
                   DEV_STATUS_NETWORK_CONNECTED_MASK) &&
               waited_ms < resync_ms)
        {
            vTaskDelay(pdMS_TO_TICKS(2000));
            waited_ms += 2000;
        }
    }
}

/* ---- lifecycle -------------------------------------------------------------- */

esp_err_t rtc_manager_init(void)
{
    static const log_descriptor_t LOG_DESC =
        { "rtc_manager", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);

    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
        s_sntp_lock = xSemaphoreCreateMutexStatic(&s_sntp_lock_buf);
    }

    /* pin the timezone: time is UTC EVERYWHERE (meatpi), and no other
     * component may skew it via a stray setenv (legacy did the same) */
    setenv("TZ", "UTC0", 1);
    tzset();

    /* the clocks as found are the baseline: a warm reset carries the
     * system clock, a power-on reads 1970 + uptime; the first set of this
     * boot is measured against it */
    rtcm_clock_init(&s_clock, wall_ms_now(), mono_ms_now());

    rtcm_events_register();
    return rtcm_settings_register();
}

esp_err_t rtc_manager_start(void)
{
    if (!rtcm_settings_is_configured())
    {
        ESP_LOGE(TAG, "unconfigured; not starting");
        return ESP_ERR_INVALID_STATE; /* standard 4.3 step 5 */
    }

    if (s_started)
    {
        return ESP_OK;
    }

    if (!rtcm_settings_config()->enabled)
    {
        ESP_LOGI(TAG, "disabled in settings");
        return ESP_OK;
    }

    esp_err_t err = rtc_rx8130_init();

    if (err != ESP_OK)
    {
        return err;
    }

    /* restore the system clock from the battery-backed RTC: a pure
     * civil-to-epoch conversion, no mktime (which would follow whatever
     * TZ is set; the UTC0 pin above is belt and braces) */
    struct tm t;

    if (rtc_manager_get_time(&t) == ESP_OK)
    {
        struct timeval tv = { .tv_sec = (time_t)rtc_time_to_epoch(&t) };
        rtc_clock_step_t step;

        settimeofday(&tv, NULL);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        clock_note_locked(RTC_CLOCK_SRC_RTC, &step);
        xSemaphoreGive(s_lock);
        clock_announce(&step);
        ESP_LOGI(TAG, "system time restored from RTC: "
                 "%04d-%02d-%02dT%02d:%02d:%02dZ (the clock moved %+lld ms)",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
                 t.tm_min, t.tm_sec, (long long)step.delta_ms);
    }
    else
    {
        ESP_LOGW(TAG, "RTC time implausible (fresh board / drained caps); "
                 "waiting for SNTP");
    }

    if (rtcm_settings_config()->sntp && s_task == NULL)
    {
        s_task = xTaskCreateStatic(sntp_task, "rtc_sntp",
                                   sizeof(s_stack) / sizeof(s_stack[0]),
                                   NULL, 3, s_stack, &s_tcb);
    }

    s_started = true;
    return ESP_OK;
}

esp_err_t rtc_manager_stop(void)
{
    s_started = false; /* task keeps its schedule; chip access still safe */
    return ESP_OK;
}

/* ---- public API --------------------------------------------------------------- */

esp_err_t rtc_manager_get_time(struct tm *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t regs[RTC_REGS_LEN];

    xSemaphoreTake(s_lock, portMAX_DELAY);

    esp_err_t err = rtc_rx8130_read(regs);

    xSemaphoreGive(s_lock);

    if (err != ESP_OK)
    {
        return err;
    }

    return rtc_time_decode(regs, out) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t rtc_manager_sync_from_system(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);

    esp_err_t err = write_rtc_from_system();

    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t rtc_manager_set_time(time_t epoch)
{
    /* plausibility: 2020-01-01 .. 2099-12-31 (the RTC's century) */
    if (epoch < 1577836800LL || epoch > 4102444799LL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    struct timeval tv = { .tv_sec = epoch };
    rtc_clock_step_t step;

    settimeofday(&tv, NULL);

    xSemaphoreTake(s_lock, portMAX_DELAY);

    esp_err_t err = write_rtc_from_system();

    /* the system clock moved whether or not the chip took it */
    clock_note_locked(RTC_CLOCK_SRC_MANUAL, &step);
    xSemaphoreGive(s_lock);
    clock_announce(&step);

    char stamp[24];

    rtc_manager_now_iso8601(stamp, sizeof(stamp));

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "time set manually (%s), RTC updated: the clock moved "
                 "%+lld ms", stamp, (long long)step.delta_ms);
    }
    else
    {
        ESP_LOGW(TAG, "time set manually (%s) but the RTC write failed: %s",
                 stamp, esp_err_to_name(err));
    }

    return err;
}

esp_err_t rtc_manager_forget_chip(void)
{
    /* 2000-01-01 00:00:00: the plausibility gate rejects year 00 at the
     * next power-on, exactly what a drained-caps unit reads */
    struct tm t = { .tm_mday = 1, .tm_year = 100 };
    uint8_t regs[RTC_REGS_LEN];

    rtc_time_encode(&t, regs);

    xSemaphoreTake(s_lock, portMAX_DELAY);

    esp_err_t err = rtc_rx8130_write(regs);

    xSemaphoreGive(s_lock);

    if (err == ESP_OK)
    {
        ESP_LOGW(TAG, "RTC chip set to 2000-01-01 on request: the next "
                 "power-on boot starts without a clock (a software restart "
                 "keeps the running one)");
    }

    return err;
}

esp_err_t rtc_manager_now_iso8601(char *buf, size_t len)
{
    if (buf == NULL || len < 21)
    {
        return ESP_ERR_INVALID_ARG;
    }

    time_t now = time(NULL);
    struct tm utc;

    gmtime_r(&now, &utc);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &utc);
    return ESP_OK;
}

bool rtc_manager_time_valid(void)
{
    return s_time_valid;
}

void rtc_manager_clock_state(rtc_clock_state_t *out)
{
    if (out == NULL)
    {
        return;
    }

    if (s_lock == NULL)
    {
        memset(out, 0, sizeof(*out)); /* before init: nothing happened */
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    rtcm_clock_state(&s_clock, out);
    xSemaphoreGive(s_lock);
}

int rtc_manager_steps_since(uint32_t since, rtc_clock_step_t *out, int cap)
{
    if (out == NULL || cap <= 0 || s_lock == NULL)
    {
        return 0;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    int n = rtcm_clock_steps_since(&s_clock, since, out, cap);

    xSemaphoreGive(s_lock);
    return n;
}

const char *rtc_manager_source_name(uint8_t source)
{
    static const char *const NAMES[] = { "none", "rtc", "sntp", "manual" };

    return (source < sizeof(NAMES) / sizeof(NAMES[0])) ? NAMES[source] : "?";
}

esp_err_t rtc_manager_sync_now(void)
{
    if (!s_started)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!dev_status_manager_any_set(DEV_STATUS_NETWORK_CONNECTED_MASK))
    {
        return ESP_ERR_INVALID_STATE; /* no internet: nothing to try */
    }

    return sntp_sync_once() ? ESP_OK : ESP_FAIL;
}

time_t rtc_manager_last_sync(void)
{
    return s_last_sync;
}

const char *rtc_manager_ntp_server(void)
{
    return rtcm_settings_config()->ntp_server;
}

bool rtc_manager_sntp_enabled(void)
{
    return rtcm_settings_config()->sntp;
}
