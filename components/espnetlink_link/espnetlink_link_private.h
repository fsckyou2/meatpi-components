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
 * @file espnetlink_link_private.h
 * @brief Shared between the engine (espnetlink_link.c), the USB pairing
 *        driver (espnetlink_link_usb.c), the store (espnetlink_link_pair.c)
 *        and the settings file.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "cJSON.h"

#include "espnetlink_link.h"
#include "espnetlink_link_core.h"
#include "http_client_manager.h"

/* ---- settings (boot-applied) -------------------------------------------- */

typedef struct
{
    bool enabled;
    espnetlink_mode_t mode;
    bool auto_pair;
    char ssid[33];
    char device_id[13];
    char host[16];
    int  gps_poll_s;
    int  health_poll_s;
    int  cut_retries;
    /* the carrier settings relayed to the dongle ("" = its own) */
    char apn[64];
    char apn_user[33];
    char apn_password[33];
    /* the SIM's PIN: relayed once per boot while the dongle reports its SIM
       locked ("" = none; 2026-10-13) */
    char sim_pin[9];
    bool cli;
} espnl_config_t;

esp_err_t espnl_settings_register(void);
const espnl_config_t *espnl_config(void);
bool espnl_config_is_configured(void);

/* ---- store (espnetlink_link_pair.c) ------------------------------------- */

/** Create the worker's semaphores (from espnetlink_link_init()). */
void espnl_pair_init(void);

/**
 * Persist the dongle as a STA candidate + our ssid/device_id. Reads the
 * stored objects first and writes only what differs (§11: no
 * unconditional rewrites). @p changed reports whether anything was
 * written: the engine reboots only then. device_id may be "".
 */
esp_err_t espnl_pair_store(const char *ssid, const char *password,
                           const char *device_id, int *slot_out,
                           bool *changed);

/* ---- USB pairing driver (espnetlink_link_usb.c) ------------------------- */

typedef struct
{
    bool     attached;        /**< NCM up to a 303A:4007 device          */
    bool     dongle_console;  /**< 303A:4007 as a CDC console alone      */
    const char *state;        /**< espnl_sm_state_str()                  */
    bool     ncm_steady;      /**< usb_ncm mode: dongle usable over USB  */
    uint32_t cuts;
    uint32_t vbus_cycles;
    uint32_t errors;
} espnl_usb_status_t;

/** Reset the machine for the configured mode. Call before the first tick. */
void espnl_usb_init(void);

/** One heartbeat (link-task context; may block on HTTP for seconds).
 *  @p ap_stale: the engine's verdict that the stored key no longer joins
 *  the dongle's AP (drives the recovery cycle). */
void espnl_usb_tick(bool ap_stale);

void espnl_usb_status(espnl_usb_status_t *out);

/** Operator re-pair request (VBUS cycle via the machine). */
esp_err_t espnl_usb_repair(void);

/** The dongle's address on the USB link when it is attached ("" else). */
const char *espnl_usb_host(void);

/* ---- engine hooks used by the USB driver -------------------------------- */

/** The identified dongle (status document): device id, firmware version
 *  and API level from /api/info, a stale dongle build is visible in the
 *  status instead of surfacing as a "foreign device" (bench 2026-09-08). */
void espnl_engine_note_info(const char *device_id, const char *fw_version,
                            int api_level);

/** The USB side is power-cycling the dongle, or saw it re-enumerate:
 *  hold the polls and re-join its AP (the association is dead either
 *  way). No-op unless the AP is the current uplink. */
void espnl_engine_dongle_rebooting(void);

/** Record the last pairing failure for the status/HTTP/UI surface
 *  ("" clears). Any store/identify failure should name itself here:
 *  the pair_state label alone proved misleading (field 2026-09-07). */
void espnl_status_set_last_error(const char *msg);

/** A health document read over the cable during the pairing pass
 *  (2026-10-07): the same cache the link polls fill, stamped now. */
void espnl_engine_note_health(const espnl_health_t *h);

/** The carrier settings' state after a pass: synced (the dongle holds
 *  them, or there is nothing to relay) or not, with the reason. */
void espnl_engine_note_carrier(bool synced, const char *note);

/** What the preference step found and did (2026-10-19): the words of
 *  espnetlink_link_status_t.prefs_agnss / prefs_ap; NULL leaves one as it is. */
void espnl_engine_note_prefs(const char *agnss, const char *ap);

/* ---- the USB driver's HTTP helpers, lent to the other files ------------- */

/** One request to the dongle over the USB link (3 s, 2 KB answer). False
 *  on a transport failure; the HTTP status is in @p resp. */
bool espnl_usb_request(const char *path, http_client_method_t method,
                       const char *body, http_client_response_t *resp);

/** POST /api/settings/submit on the dongle (it restarts to apply). */
bool espnl_usb_submit(void);

/** Read the dongle's settings document at @p path and stage @p key =
 *  @p want when it differs (or sits staged with pending_reboot): 1 staged
 *  (a submit is owed), 0 already so, -1 failure, -2 no such API (404).
 *  Takes ownership of @p want. */
int espnl_usb_ensure_setting(const char *path, const char *key,
                             cJSON *want);

/* ---- the dongle preferences (espnetlink_link_ensure.c, 2026-10-19) ------ */

/** AGNSS on, and with @p want_ap the dongle's WiFi access point on
 *  (wifi_modem mode). 1 = something was staged (and submitted when
 *  @p submit: the dongle restarts), 0 = nothing to do, -1 = failure. */
int espnl_ensure_prefs(bool want_ap, bool submit);

/* ---- the update hold (espnetlink_link_update.c, 2026-10-19) ------------- */

/** While on, the link task stops its polls, the AP-path restore and the
 *  stale-AP recovery (an image is being pushed into the dongle's single
 *  httpd worker). Released by the caller, or by itself after 15 min. */
void espnl_engine_set_update_hold(bool hold);
bool espnl_engine_update_hold(void);
