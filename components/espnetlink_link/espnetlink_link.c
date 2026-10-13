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
 * @file espnetlink_link.c
 * @brief Link task: every second drive the USB pairing machine, detect
 *        where the dongle is reachable (its AP via the STA, or the USB
 *        link in usb_ncm mode), poll GPS (`/api/gps`) and dongle health
 *        (`/api/wifi_modem`) over HTTP, publish fixes through the one
 *        sink.
 *
 * State is derived, not event-driven: every tick reads wifi_manager's
 * connected flag + the driver's AP record and usb_host_manager's status,
 * no callback ownership fight with the other consumers, and a missed
 * event can never strand the state.
 */
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "dev_status_manager.h"
#include "http_client_manager.h"
#include "log_manager.h"
#include "usb_host_manager.h"
#include "wifi_manager.h"

#include "espnetlink_link.h"
#include "espnetlink_link_core.h"
#include "espnetlink_link_private.h"

static const char *TAG = "espnetlink";

#define ESPNL_TICK_MS           1000
#define ESPNL_GPS_TIMEOUT_MS    3000
#define ESPNL_HEALTH_TIMEOUT_MS 4000
/* usb-mode AP-path usb_data restore: retry interval */
#define ESPNL_RESTORE_RETRY_MS 15000
#define ESPNL_MAX_RESP          1024
/* paired + dongle present + cut, yet the STA sits on nothing for this
 * long -> the stored key is presumed stale (re-read over USB) */
#define ESPNL_AP_STALE_MS       180000
/* the dongle's AP reboots faster than the STA's beacon-loss window, so
 * the driver keeps a ZOMBIE association the dongle has forgotten: every
 * socket times out on a "connected" STA. Kick the STA (re-join) after
 * this many consecutive failed polls, and hold polls this long around a
 * dongle reboot we caused ourselves. */
#define ESPNL_KICK_AFTER_FAILS  3
#define ESPNL_POLL_HOLD_MS      15000

static TaskHandle_t s_task;
static volatile bool s_run;
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;

static espnetlink_gps_sink_t s_sink;
static usb_acm_gps_t s_gps;
static uint32_t s_gps_stamp_ms;
static espnl_health_t s_health;
static uint32_t s_health_stamp_ms;   /* when s_health was read (valid)     */
static bool s_carrier_synced = true; /* nothing to relay until a pass says */
static char s_carrier_note[96] EXT_RAM_BSS_ATTR;
static espnetlink_link_status_t s_st EXT_RAM_BSS_ATTR;
static int s_consec_fail;          /* polls failed in a row (task-only) */
static volatile uint32_t s_poll_hold_until_ms;
static volatile bool s_kick_sta;   /* the USB side saw the dongle reboot */
static espnl_nofix_t s_nofix;      /* the GPS hint accumulator (task-only) */
static uint32_t s_nofix_s;         /* its reading, under s_lock             */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ---- public getters ------------------------------------------------ */

void espnetlink_link_set_gps_sink(espnetlink_gps_sink_t sink)
{
    s_sink = sink;
}

esp_err_t espnetlink_link_gps_get(usb_acm_gps_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL)
    {
        memset(out, 0, sizeof(*out));
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_gps;
    if (s_gps.valid)
    {
        out->age_ms = now_ms() - s_gps_stamp_ms;
    }
    xSemaphoreGive(s_lock);
    return out->valid ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t espnetlink_link_status(espnetlink_link_status_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL)
    {
        memset(out, 0, sizeof(*out));
        out->pair_state = "off";
        return ESP_ERR_INVALID_STATE;
    }

    espnl_usb_status_t usb;

    espnl_usb_status(&usb);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_st;
    out->gps_valid = s_gps.valid;
    out->gps_age_ms = s_gps.valid ? now_ms() - s_gps_stamp_ms : 0;
    out->health_valid = s_health.valid;
    out->lte_connected = s_health.lte_connected;
    out->rssi_dbm = s_health.rssi_dbm;
    strncpy(out->operator_name, s_health.operator_name,
            sizeof(out->operator_name) - 1);
    out->operator_name[sizeof(out->operator_name) - 1] = '\0';
    strncpy(out->network_type, s_health.network_type,
            sizeof(out->network_type) - 1);
    out->network_type[sizeof(out->network_type) - 1] = '\0';
    out->dongle_gps_fix = s_health.gps_fix;
    out->dongle_usb_data = s_health.usb_data;
    snprintf(out->dongle_sim, sizeof(out->dongle_sim), "%s", s_health.sim);
    snprintf(out->dongle_stage, sizeof(out->dongle_stage), "%s",
             s_health.stage);
    snprintf(out->dongle_ip, sizeof(out->dongle_ip), "%s", s_health.ip);
    out->health_age_ms = s_health.valid ? now_ms() - s_health_stamp_ms : 0;
    out->no_fix_s = s_nofix_s;
    out->no_fix_hint = s_nofix_s >= ESPNL_NOFIX_HINT_S;
    snprintf(out->apn, sizeof(out->apn), "%s", espnl_config()->apn);
    out->carrier_synced = s_carrier_synced;
    snprintf(out->carrier_note, sizeof(out->carrier_note), "%s",
             s_carrier_note);
    xSemaphoreGive(s_lock);

    out->usb_attached = usb.attached;
    out->dongle_console = usb.dongle_console;
    out->pair_state = usb.state;
    out->cuts = usb.cuts;
    out->vbus_cycles = usb.vbus_cycles;
    out->pair_errors = usb.errors;
    return ESP_OK;
}

void espnl_engine_note_info(const char *device_id, const char *fw_version,
                            int api_level)
{
    if (s_lock == NULL)
    {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (device_id != NULL)
    {
        strncpy(s_st.device_id, device_id, sizeof(s_st.device_id) - 1);
        s_st.device_id[sizeof(s_st.device_id) - 1] = '\0';
    }
    snprintf(s_st.dongle_fw, sizeof(s_st.dongle_fw), "%s",
             fw_version != NULL ? fw_version : "");
    s_st.dongle_api = api_level;
    xSemaphoreGive(s_lock);
}

void espnl_status_set_last_error(const char *msg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_st.last_error, sizeof(s_st.last_error), "%s",
             msg != NULL ? msg : "");
    xSemaphoreGive(s_lock);
}

void espnl_engine_note_health(const espnl_health_t *h)
{
    if (h == NULL || !h->valid || s_lock == NULL)
    {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_health = *h;
    s_health_stamp_ms = now_ms();
    s_st.health_unsupported = false;
    xSemaphoreGive(s_lock);
}

void espnl_engine_note_carrier(bool synced, const char *note)
{
    if (s_lock == NULL)
    {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_carrier_synced = synced;
    snprintf(s_carrier_note, sizeof(s_carrier_note), "%s",
             note != NULL ? note : "");
    xSemaphoreGive(s_lock);
}

void espnl_engine_dongle_rebooting(void)
{
    /* the dongle is going down (VBUS cycle) or just came back up (its
     * USB re-enumerated): either way its AP has forgotten us. Re-join
     * and do not hammer it with doomed polls meanwhile: only when the
     * AP is what we are on (usb_ncm mode polls over USB: leave it) */
    if (s_st.uplink != ESPNETLINK_UPLINK_ESPNETLINK_AP)
    {
        return;
    }
    s_poll_hold_until_ms = now_ms() + ESPNL_POLL_HOLD_MS;
    s_kick_sta = true;
}

esp_err_t espnetlink_link_repair(void)
{
    if (s_task == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return espnl_usb_repair();
}

/* ---- link detection ------------------------------------------------ */

static bool connected_ssid(char *out, size_t len)
{
    wifi_ap_record_t ap;

    out[0] = '\0';
    if (!wifi_manager_is_sta_connected() ||
        esp_wifi_sta_get_ap_info(&ap) != ESP_OK)
    {
        return false;
    }
    strncpy(out, (const char *)ap.ssid, len - 1);
    out[len - 1] = '\0';
    return true;
}

static void sta_gateway(char *out, size_t len)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = { 0 };

    if (sta != NULL && esp_netif_get_ip_info(sta, &ip) == ESP_OK &&
        ip.gw.addr != 0)
    {
        snprintf(out, len, IPSTR, IP2STR(&ip.gw));
        return;
    }
    out[0] = '\0';
}

/* ---- HTTP polls ---------------------------------------------------- */

/* @p status_out: the HTTP status (-1 = no answer), also on failure, a
 * 404 means the dongle firmware has no such route, which the caller
 * reports instead of retrying in silence. */
static bool http_get(const char *url, int timeout_ms,
                     http_client_response_t *resp, int *status_out)
{
    http_client_request_t req =
    {
        .url          = url,
        .method       = HTTP_CLIENT_GET,
        .timeout_ms   = timeout_ms,
        .max_response = ESPNL_MAX_RESP,
    };

    memset(resp, 0, sizeof(*resp));
    esp_err_t err = http_client_manager_request(&req, resp);

    if (status_out != NULL)
    {
        *status_out = err == ESP_OK ? resp->status_code : -1;
    }
    if (err != ESP_OK || resp->status_code != 200 || resp->data == NULL)
    {
        ESP_LOGD(TAG, "GET %s: %s status %d", url, esp_err_to_name(err),
                 resp->status_code);
        http_client_manager_free(resp);
        return false;
    }
    return true;
}

static void poll_gps(const char *host)
{
    char url[64];
    http_client_response_t resp;

    if (!espnl_core_url(url, sizeof(url), host, "/api/gps"))
    {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.polls++;
    xSemaphoreGive(s_lock);

    if (!http_get(url, ESPNL_GPS_TIMEOUT_MS, &resp, NULL))
    {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_st.failures++;
        s_gps.valid = false; /* never serve a stale fix as current */
        xSemaphoreGive(s_lock);
        s_consec_fail++;
        return;
    }
    s_consec_fail = 0;

    usb_acm_gps_t fix;
    bool ok = usb_acm_gps_parse(resp.data, &fix);

    http_client_manager_free(&resp);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (ok)
    {
        s_gps = fix;
        s_gps_stamp_ms = now_ms();
    }
    else
    {
        s_gps.valid = false;
    }
    xSemaphoreGive(s_lock);

    if (ok && s_sink != NULL)
    {
        s_sink(&fix);
    }
}

static void poll_health(const char *host)
{
    char url[64];
    http_client_response_t resp;
    espnl_health_t h;

    if (!espnl_core_url(url, sizeof(url), host, "/api/wifi_modem"))
    {
        return;
    }

    int status = -1;

    if (!http_get(url, ESPNL_HEALTH_TIMEOUT_MS, &resp, &status))
    {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_health.valid = false;
        if (status == 404 && !s_st.health_unsupported)
        {
            /* a dongle build from before the WiFi-modem work (bench
             * 2026-09-08): the LTE panel would sit on "waiting for the
             * first poll" forever without this */
            s_st.health_unsupported = true;
            ESP_LOGW(TAG, "dongle health: GET /api/wifi_modem answers 404 - "
                     "the dongle firmware has no health API (update it)");
        }
        xSemaphoreGive(s_lock);
        s_consec_fail++;
        return;
    }
    s_consec_fail = 0;

    bool ok = espnl_core_parse_health(resp.data, &h);

    http_client_manager_free(&resp);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.health_unsupported = false;
    if (ok)
    {
        bool was_up = s_health.valid && s_health.lte_connected;

        s_health = h;
        s_health_stamp_ms = now_ms();
        if (!was_up && h.lte_connected)
        {
            ESP_LOGI(TAG, "dongle LTE up (%s %s, %d dBm)",
                     h.operator_name[0] ? h.operator_name : "?",
                     h.network_type[0] ? h.network_type : "", h.rssi_dbm);
        }
    }
    else
    {
        s_health.valid = false;
    }
    xSemaphoreGive(s_lock);
}

/* usb_ncm / usb_rndis mode reached over the dongle's AP with its data
 * lines cut (a runtime cut from an earlier wifi_modem pass, which lasts
 * until the dongle's next boot): restore them over WiFi, POST
 * /api/wifi_modem/usb_data {"enabled":true}. The dongle releases its mux
 * at once, so USB re-enumerates and the machine can bring the
 * USB-Ethernet uplink up without a dongle restart. Rate-limited by the
 * caller. Returns true on a 200. */
static bool ap_restore_usb_data(const char *host)
{
    char url[64];
    http_client_response_t resp;

    if (!espnl_core_url(url, sizeof(url), host, "/api/wifi_modem/usb_data"))
    {
        return false;
    }

    http_client_request_t req =
    {
        .url          = url,
        .method       = HTTP_CLIENT_POST,
        .body         = "{\"enabled\":true}",
        .body_len     = sizeof("{\"enabled\":true}") - 1,
        .content_type = "application/json",
        .timeout_ms   = ESPNL_HEALTH_TIMEOUT_MS,
        .max_response = ESPNL_MAX_RESP,
    };

    memset(&resp, 0, sizeof(resp));
    esp_err_t err = http_client_manager_request(&req, &resp);
    int status = resp.status_code;

    http_client_manager_free(&resp);
    if (err != ESP_OK || status != 200)
    {
        ESP_LOGW(TAG, "usb_data restore over AP: %s status %d (dongle "
                 "usb_mode ncm?)", esp_err_to_name(err), status);
        return false;
    }

    ESP_LOGI(TAG, "usb mode: dongle data lines restored over the AP, "
             "waiting for USB enumeration");
    return true;
}

/* ---- task ---------------------------------------------------------- */

static const char *uplink_str(espnetlink_uplink_t u)
{
    switch (u)
    {
        case ESPNETLINK_UPLINK_WIFI:           return "wifi";
        case ESPNETLINK_UPLINK_ESPNETLINK_AP:  return "espnetlink (AP)";
        case ESPNETLINK_UPLINK_ESPNETLINK_USB: return "espnetlink (USB)";
        default:                               return "none";
    }
}

/* The stored key is presumed stale when the dongle is physically there
 * (ID pin) with its data cut, we are paired for it, the STA is not
 * suspended by policy, and it still sits on NO network for a while. */
static bool ap_stale_check(const espnl_config_t *cfg, bool sta, bool on_ap,
                           bool usb_attached, uint32_t now)
{
    static uint32_t s_off_since;
    usb_host_manager_status_t usb;
    bool suspended = (dev_status_manager_get() &
                      DEV_STATUS_BIT_STA_SUSPENDED) != 0;
    bool candidate = cfg->mode == ESPNETLINK_MODE_WIFI_MODEM &&
                     cfg->auto_pair && cfg->ssid[0] != '\0' && !sta &&
                     !on_ap && !usb_attached && !suspended &&
                     usb_host_manager_status(&usb) == ESP_OK &&
                     usb.host_active && usb.device_present;

    if (!candidate)
    {
        s_off_since = 0;
        return false;
    }
    if (s_off_since == 0)
    {
        s_off_since = now;
        return false;
    }
    if ((uint32_t)(now - s_off_since) >= ESPNL_AP_STALE_MS)
    {
        s_off_since = now; /* re-arm: the machine rate-limits the cycles */
        return true;
    }
    return false;
}

static void link_task(void *arg)
{
    (void)arg;

    const espnl_config_t *cfg = espnl_config();
    uint32_t next_gps_ms = 0;
    uint32_t next_health_ms = 0;
    uint32_t next_restore_ms = 0;
    espnetlink_uplink_t uplink = ESPNETLINK_UPLINK_NONE;
    bool ap_stale = false;

    espnl_usb_init();

    while (s_run)
    {
        vTaskDelay(pdMS_TO_TICKS(ESPNL_TICK_MS));

        /* 1. the USB side (pairing machine; may block on HTTP) */
        espnl_usb_tick(ap_stale);
        ap_stale = false;

        /* 2. where is the dongle reachable right now? */
        char ssid[33];
        char host[16] = "";
        bool sta = connected_ssid(ssid, sizeof(ssid));
        bool on_ap = sta && espnl_core_ssid_match(cfg->ssid, ssid);
        espnl_usb_status_t usb;
        espnetlink_uplink_t now_up = ESPNETLINK_UPLINK_NONE;

        espnl_usb_status(&usb);

        if (usb.ncm_steady)
        {
            /* usb_ncm / usb_rndis mode (or manual mode with the link left
             * on): the dongle answers at its fixed USB address. Checked
             * BEFORE on_ap: the wire is the configured transport there,
             * and the STA may well still be parked on the dongle's AP
             * (its slot survives pairing): poll over the wire anyway. */
            now_up = ESPNETLINK_UPLINK_ESPNETLINK_USB;
            strncpy(host, espnl_usb_host(), sizeof(host) - 1);
        }
        else if (on_ap)
        {
            now_up = ESPNETLINK_UPLINK_ESPNETLINK_AP;
            if (cfg->host[0] != '\0')
            {
                strncpy(host, cfg->host, sizeof(host) - 1);
            }
            else
            {
                sta_gateway(host, sizeof(host));
            }
        }
        else if (sta)
        {
            now_up = ESPNETLINK_UPLINK_WIFI;
        }
        host[sizeof(host) - 1] = '\0';

        uint32_t t = now_ms();

        if (now_up != uplink)
        {
            ESP_LOGI(TAG, "uplink: %s -> %s%s%s", uplink_str(uplink),
                     uplink_str(now_up), host[0] ? " via " : "", host);
            uplink = now_up;
            next_gps_ms = t;     /* poll immediately */
            next_health_ms = t;

            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (uplink == ESPNETLINK_UPLINK_ESPNETLINK_AP ||
                uplink == ESPNETLINK_UPLINK_ESPNETLINK_USB)
            {
                s_st.link_ups++;
            }
            else
            {
                /* the fix is never served stale; the health reading stays
                 * with its age (2026-10-07: a WiCAN on its home WiFi shows
                 * what the dongle said last, the UI names how long ago) */
                s_gps.valid = false;
                s_st.health_unsupported = false;
            }
            xSemaphoreGive(s_lock);
        }

        ap_stale = ap_stale_check(cfg, sta, on_ap, usb.attached, t);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_st.sta_connected = sta;
        s_st.uplink = uplink;
        strncpy(s_st.host, host, sizeof(s_st.host) - 1);
        s_st.host[sizeof(s_st.host) - 1] = '\0';
        /* the GPS hint (2026-10-13): away from home on the wire with LTE
         * up and no fix anywhere, for how long (a station parked on the
         * dongle's own AP slot is not at home) */
        {
            bool away = uplink == ESPNETLINK_UPLINK_ESPNETLINK_USB &&
                        !(sta && !on_ap);
            bool lte_up = s_health.valid && s_health.lte_connected;
            bool fix = s_gps.valid || (s_health.valid && s_health.gps_fix);

            s_nofix_s = espnl_core_nofix_update(&s_nofix, away, lte_up, fix,
                                                t);
        }
        xSemaphoreGive(s_lock);

        /* 3. a dead association: the dongle rebooted (we cycled it, or
         *    polls time out on a "connected" STA) -> re-join the AP */
        if (on_ap && (s_kick_sta || s_consec_fail >= ESPNL_KICK_AFTER_FAILS))
        {
            ESP_LOGI(TAG, "dongle AP %s: re-joining",
                     s_kick_sta ? "rebooted" : "unreachable");
            (void)wifi_manager_sta_reconnect();
            s_poll_hold_until_ms = t + ESPNL_POLL_HOLD_MS;
            s_consec_fail = 0;
        }
        if (!on_ap)
        {
            s_consec_fail = 0;
        }
        s_kick_sta = false;

        /* 4. polls, only while the dongle is reachable and not known to
         *    be rebooting */
        if (host[0] == '\0' || (uplink != ESPNETLINK_UPLINK_ESPNETLINK_AP &&
                                uplink != ESPNETLINK_UPLINK_ESPNETLINK_USB) ||
            (int32_t)(t - s_poll_hold_until_ms) < 0)
        {
            continue;
        }

        if ((int32_t)(t - next_gps_ms) >= 0)
        {
            next_gps_ms = t + (uint32_t)cfg->gps_poll_s * 1000u;
            poll_gps(host);
        }
        if ((int32_t)(t - next_health_ms) >= 0)
        {
            next_health_ms = t + (uint32_t)cfg->health_poll_s * 1000u;
            poll_health(host);
        }

        /* 5. usb_ncm / usb_rndis mode but the dongle's data lines are cut
         *    (a wifi_modem pass cut them earlier in this dongle boot; the
         *    cut lasts until the dongle restarts): restore over WiFi so
         *    the switch needs no dongle restart. */
        if (cfg->mode != ESPNETLINK_MODE_WIFI_MODEM &&
            uplink == ESPNETLINK_UPLINK_ESPNETLINK_AP &&
            (int32_t)(t - next_restore_ms) >= 0)
        {
            bool cut;

            xSemaphoreTake(s_lock, portMAX_DELAY);
            cut = s_health.valid && !s_health.usb_data;
            xSemaphoreGive(s_lock);

            if (cut)
            {
                next_restore_ms = t + ESPNL_RESTORE_RETRY_MS;
                (void)ap_restore_usb_data(host);
            }
        }
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

/* ---- lifecycle ----------------------------------------------------- */

esp_err_t espnetlink_link_init(void)
{
    static const log_descriptor_t LOG_DESC = { "espnetlink", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);

    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
    s_st.pair_state = "idle";
    espnl_pair_init();

    return espnl_settings_register();
}

esp_err_t espnetlink_link_start(void)
{
    if (!espnl_config_is_configured())
    {
        return ESP_ERR_INVALID_STATE;
    }

    const espnl_config_t *cfg = espnl_config();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.enabled = cfg->enabled;
    s_st.mode = cfg->mode;
    s_st.auto_pair = cfg->auto_pair;
    s_st.paired = cfg->ssid[0] != '\0';
    /* reboot-to-apply: the AP password cannot change within this boot,
     * so the hold is a boot-time fact */
    s_st.pair_blocked_factory_pw =
        cfg->enabled && !s_st.paired &&
        cfg->mode == ESPNETLINK_MODE_WIFI_MODEM &&
        wifi_manager_ap_password_is_factory();
    s_st.last_error[0] = '\0';
    s_st.dongle_fw[0] = '\0';
    s_st.dongle_api = 0;
    s_st.health_unsupported = false;
    s_nofix.running = false;
    s_nofix_s = 0;
    strncpy(s_st.ssid, cfg->ssid, sizeof(s_st.ssid) - 1);
    strncpy(s_st.device_id, cfg->device_id, sizeof(s_st.device_id) - 1);
    xSemaphoreGive(s_lock);

    if (s_st.pair_blocked_factory_pw)
    {
        ESP_LOGW(TAG, "zero-touch pairing is ON HOLD: the access point "
                 "still uses the factory password. Set a new AP password "
                 "in the web UI; the restart completes the pairing "
                 "automatically.");
    }
    if (!cfg->enabled)
    {
        ESP_LOGI(TAG, "disabled");
        return ESP_OK;
    }
    if (cfg->mode == ESPNETLINK_MODE_WIFI_MODEM && cfg->ssid[0] != '\0' &&
        !wifi_manager_mode_has_sta())
    {
        ESP_LOGW(TAG, "WiFi mode has no STA: the dongle AP cannot be "
                 "joined (pairing will fix the mode on the next key read)");
    }
    if (s_task != NULL)
    {
        return ESP_OK;
    }

    /* §2: static stack in PSRAM (plain-HTTP client; no TLS; the settings
     * store hands flash work to the Settings Manager's writer task) */
    static StaticTask_t s_tcb;
    static StackType_t s_stack[6144] EXT_RAM_BSS_ATTR;

    s_run = true;
    s_task = xTaskCreateStaticPinnedToCore(link_task, "espnl_link", 6144,
                                           NULL, 3, s_stack, &s_tcb,
                                           tskNO_AFFINITY);
    if (s_task == NULL)
    {
        s_run = false;
        return ESP_ERR_NO_MEM;
    }

    {
        usb_host_manager_status_t us;

        usb_host_manager_status(&us);
        if (!us.enabled)
        {
            ESP_LOGW(TAG, "usb_host_manager is disabled: an ESPNetLink on "
                     "the USB connector will never be detected or paired "
                     "(enable it on the USB page)");
        }
    }

    ESP_LOGI(TAG, "started (%s, auto_pair=%d, ssid '%s', gps %d s, "
             "health %d s)",
             espnl_core_mode_str((espnl_core_mode_t)cfg->mode),
             (int)cfg->auto_pair, cfg->ssid, cfg->gps_poll_s,
             cfg->health_poll_s);
    return ESP_OK;
}

esp_err_t espnetlink_link_stop(void)
{
    s_run = false;
    return ESP_OK;
}
