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
 * @file wifi_manager_try.c
 * @brief The connection trial over the real driver (2026-10-06): the glue
 *        around the pure machine in wifi_manager_trial.c. A runtime knob,
 *        never a setting: the credentials live here for the seconds of the
 *        trial and are gone with it (the driver's config storage is RAM).
 *        Three tasks touch the trial: httpd (begin, status), the WiFi event
 *        task (the hooks wifi_manager.c calls) and esp_timer (the 1 s tick
 *        for the budgets); one mutex orders them, and the driver is called
 *        outside it. The reconnect task stands back while a trial runs
 *        (wm_try_active()) and reconnects the configured network afterwards
 *        on its own lap.
 */
#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "wifi_manager_private.h"

static const char *TAG = "wifi_manager";

#define TRY_TICK_MS  1000u
/* the radio action starts this long after the request was accepted: a page
   that reaches us over the station would never see its 202 otherwise (the
   station drops inside the handler; the settings restart delays for the
   same reason) */
#define TRY_START_MS 300u

static wm_trial_t         s_trial EXT_RAM_BSS_ATTR; /* PSRAM by default (§2) */
static SemaphoreHandle_t  s_try_lock;
static StaticSemaphore_t  s_try_lock_buf;
static esp_timer_handle_t s_try_timer;
static esp_timer_handle_t s_start_timer;   /* one-shot: the deferred start */
static wm_trial_action_t  s_start_act;
static bool               s_start_busy;

static bool               s_leave_pending; /* our own letting-go follows */
static bool               s_restore_cue;   /* a trial ended: re-join now  */

static void act_finish(bool still_on_air);

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void lock(void)
{
    xSemaphoreTake(s_try_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_try_lock);
}

static void ensure_init(void)
{
    if (s_try_lock == NULL)
    {
        s_try_lock = xSemaphoreCreateMutexStatic(&s_try_lock_buf);
        wm_trial_init(&s_trial);
    }
}

static void timer_run(bool on)
{
    if (s_try_timer == NULL)
    {
        return;
    }

    if (on)
    {
        (void)esp_timer_start_periodic(s_try_timer, TRY_TICK_MS * 1000ull);
    }
    else
    {
        (void)esp_timer_stop(s_try_timer);
    }
}

/* the one place a trial's connect attempt is issued */
static void act_connect(void)
{
    esp_err_t err = wm_sta_apply_trial(s_trial.ssid, s_trial.password,
                                       s_trial.hint);

    if (err == ESP_OK)
    {
        err = esp_wifi_connect();
    }

    if (err != ESP_OK)
    {
        /* the driver would not take it (a radio that is stopping, a mode
           change under our feet): report, do not hang the budget */
        ESP_LOGW(TAG, "trial: '%s' could not start (%s)", s_trial.ssid,
                 esp_err_to_name(err));
        lock();
        (void)wm_trial_on_disconnect(&s_trial, 0, now_ms());
        unlock();
        act_finish(false);
    }
}

/* the result is in: let go of the network if we are still on it, put the
   configured station back, say what happened */
static void act_finish(bool still_on_air)
{
    wm_trial_t t;

    lock();
    t = s_trial;
    unlock();
    timer_run(false);

    if (still_on_air)
    {
        /* the DISCONNECTED event this raises (ASSOC_LEAVE) is ours too:
           wm_try_on_disconnected claims it so the configured networks'
           attempt memory never sees the trial */
        lock();
        s_leave_pending = true;
        unlock();
        (void)esp_wifi_disconnect();
    }

    wm_sta_restore_config();
    lock();
    s_restore_cue = true;
    unlock();

    switch (t.result)
    {
        case WM_TRIAL_CONNECTED:
            ESP_LOGI(TAG, "trial: '%s' connected, %s, %d dBm, channel %u, "
                          "in %lu ms",
                     t.ssid, t.ip, t.rssi, t.channel,
                     (unsigned long)wm_trial_took_ms(&t));
            break;

        case WM_TRIAL_PASSWORD:
            ESP_LOGI(TAG, "trial: '%s' did not accept the password "
                          "(reason %u) after %lu ms",
                     t.ssid, t.reason, (unsigned long)wm_trial_took_ms(&t));
            break;

        case WM_TRIAL_NOT_FOUND:
            ESP_LOGI(TAG, "trial: '%s' not found (reason %u) after %lu ms",
                     t.ssid, t.reason, (unsigned long)wm_trial_took_ms(&t));
            break;

        default:
            ESP_LOGI(TAG, "trial: '%s' %s (reason %u) after %lu ms", t.ssid,
                     wm_trial_result_name(t.result), t.reason,
                     (unsigned long)wm_trial_took_ms(&t));
            break;
    }
}

static void perform(wm_trial_action_t act, bool still_on_air)
{
    if (act == WM_TRIAL_ACT_CONNECT)
    {
        act_connect();
    }
    else if (act == WM_TRIAL_ACT_FINISH)
    {
        act_finish(still_on_air);
    }
}

/* the deferred start: let go of the current network if there is one (its
   DISCONNECTED event connects), else connect now */
static void start_cb(void *arg)
{
    (void)arg;
    timer_run(true);

    if (s_start_busy)
    {
        (void)esp_wifi_disconnect();
    }

    perform(s_start_act, false);
}

static void tick_cb(void *arg)
{
    wm_trial_action_t act;
    bool on_air;

    (void)arg;
    lock();
    on_air = s_trial.state != WM_TRIAL_DONE; /* a pending connect too */
    act = wm_trial_tick(&s_trial, now_ms());
    unlock();
    /* a budget that ran out: esp_wifi_disconnect() also cancels a connect
       the driver is still working on */
    perform(act, on_air);
}

/* ---- the hooks wifi_manager.c calls ----------------------------------------- */

bool wm_try_active(void)
{
    bool on;

    if (s_try_lock == NULL)
    {
        return false;
    }

    lock();
    on = wm_trial_active(&s_trial);
    unlock();
    return on;
}

bool wm_try_take_restore_cue(void)
{
    bool cue;

    if (s_try_lock == NULL)
    {
        return false;
    }

    lock();
    cue = s_restore_cue;
    s_restore_cue = false;
    unlock();
    return cue;
}

bool wm_try_on_disconnected(uint8_t reason)
{
    wm_trial_action_t act;
    bool ours;

    if (s_try_lock == NULL)
    {
        return false;
    }

    lock();
    ours = wm_trial_active(&s_trial);
    act = ours ? wm_trial_on_disconnect(&s_trial, reason, now_ms())
               : WM_TRIAL_ACT_NONE;

    if (!ours && s_leave_pending)
    {
        s_leave_pending = false; /* the letting-go after a result */
        ours = true;
    }

    unlock();
    /* a disconnect ended it: the station is already down, nothing to let
       go of */
    perform(act, false);
    return ours;
}

void wm_try_on_associated(uint8_t channel)
{
    if (s_try_lock == NULL)
    {
        return;
    }

    lock();
    wm_trial_on_associated(&s_trial, channel, now_ms());
    unlock();
}

bool wm_try_on_got_ip(const char *ip)
{
    wifi_ap_record_t ap = { 0 };
    wm_trial_action_t act;
    bool ours;

    if (s_try_lock == NULL)
    {
        return false;
    }

    lock();
    ours = wm_trial_active(&s_trial);
    bool leaving = s_trial.state == WM_TRIAL_LEAVING;
    unlock();

    if (!ours)
    {
        return false;
    }

    if (leaving)
    {
        /* the configured network's connect landed while the trial was
           letting go (the reconnect task's lap was past its check when
           the trial began): let go of that too, its DISCONNECTED event
           is the cue the trial waits for */
        ESP_LOGD(TAG, "trial: the configured network connected meanwhile; "
                      "letting go again");
        (void)esp_wifi_disconnect();
        return true;
    }

    (void)esp_wifi_sta_get_ap_info(&ap); /* the signal, while we are on it */

    lock();
    act = wm_trial_on_got_ip(&s_trial, ip, ap.rssi, now_ms());
    unlock();
    perform(act, true);
    return true;
}

/* ---- the public surface ------------------------------------------------------ */

esp_err_t wifi_manager_sta_try(const char *ssid, const char *password,
                               uint8_t channel)
{
    const wm_config_t *cfg = wm_settings_config();
    wm_trial_action_t act;
    bool busy;
    char hint[24] = "";

    if (ssid == NULL || password == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    ensure_init();

    if (cfg == NULL || !wm_core_started() ||
        (cfg->mode != WM_MODE_APSTA && cfg->mode != WM_MODE_STA) ||
        wm_suspend_sta_active())
    {
        return ESP_ERR_NOT_SUPPORTED; /* no station interface to try with */
    }

    if (s_try_timer == NULL)
    {
        const esp_timer_create_args_t args =
        {
            .callback = tick_cb,
            .name = "wifi_try",
        };
        const esp_timer_create_args_t start =
        {
            .callback = start_cb,
            .name = "wifi_try_start",
        };

        if (esp_timer_create(&args, &s_try_timer) != ESP_OK ||
            esp_timer_create(&start, &s_start_timer) != ESP_OK)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    busy = wifi_manager_is_sta_connected() || wm_sta_attempt_in_flight();

    lock();

    if (wm_trial_active(&s_trial))
    {
        unlock();
        return ESP_ERR_INVALID_STATE; /* one at a time */
    }

    s_leave_pending = false; /* a letting-go that never came is forgotten */
    act = wm_trial_begin(&s_trial, ssid, password, channel, busy, now_ms());

    if (!wm_trial_active(&s_trial))
    {
        unlock();
        return ESP_ERR_INVALID_ARG; /* the credentials are out of range */
    }

    if (s_trial.hint != 0)
    {
        snprintf(hint, sizeof(hint), " (channel %u first)", s_trial.hint);
    }

    unlock();

    if (busy)
    {
        wm_sta_note_rejoin_hint(); /* the re-join after the trial scans
                                      the channel it is leaving first */
    }

    ESP_LOGI(TAG, "trying '%s' on request%s%s", ssid, hint,
             busy ? " (the station lets go of its network first)" : "");
    s_start_act = act;
    s_start_busy = busy;
    (void)esp_timer_start_once(s_start_timer, TRY_START_MS * 1000ull);
    return ESP_OK;
}

void wifi_manager_sta_try_status(wifi_manager_try_t *out)
{
    wm_trial_t t;

    memset(out, 0, sizeof(*out));

    if (s_try_lock == NULL)
    {
        strcpy(out->state, "idle");
        strcpy(out->result, "none");
        return;
    }

    lock();
    t = s_trial;
    unlock();

    strcpy(out->state, wm_trial_state_name(t.state));
    strcpy(out->result, (t.state == WM_TRIAL_DONE)
                            ? wm_trial_result_name(t.result) : "none");
    strcpy(out->ssid, t.ssid);
    strcpy(out->ip, t.ip);
    out->reason = t.reason;
    out->rssi = t.rssi;
    out->channel = t.channel;
    out->took_ms = wm_trial_took_ms(&t);
    out->age_s = (t.state == WM_TRIAL_DONE) ? (now_ms() - t.t_done) / 1000u
                                            : 0;
}
