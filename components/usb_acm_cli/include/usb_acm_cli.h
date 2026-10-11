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
 * @file usb_acm_cli.h
 * @brief USB serial (CDC-ACM) host: a plain USB GPS receiver, and the
 *        ESPNetLink dongle's management console.
 *
 * Whatever CDC-ACM device the USB host binds is read by one task. A device
 * that streams NMEA 0183 (a u-blox receiver) becomes a GPS source: its fix
 * feeds `GET /api/gps`, the autopid `gps_*` parameters and so Home
 * Assistant, the destinations, the dashboard and the logger. The ESPNetLink
 * (a composite device: RNDIS/NCM = the LTE data path, CDC-ACM = its own
 * console with an `esp>` prompt) is talked to only when the console switch
 * is on: `POST /api/usb/acm/cmd`, the `acm` CLI, a bridge_manager endpoint
 * `acm` (raw passthrough), and a `gps -p -j` poll every 5 s.
 *
 * The USB host itself is owned by usb_host_manager/usb_eth_host; this
 * component only registers the CDC-ACM class callbacks CherryUSB invokes
 * on attach/detach. Settings ("usb_acm_cli", version 2): `gps` (read a USB
 * GPS receiver, default true), `console` (the ESPNetLink console over USB,
 * default false; version 1's `enabled`), `cli`.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "usb_acm_gps.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t usb_acm_cli_init(void);
esp_err_t usb_acm_cli_start(void);
esp_err_t usb_acm_cli_stop(void);

/** True while a CDC-ACM device is attached and bound. */
bool usb_acm_cli_connected(void);

/** True while a device is bound AND the RX task reads it (either switch on):
 *  a bound device nobody reads is not "reading". */
bool usb_acm_cli_reading(void);

/** The two switches as applied at boot (settings v2, 2026-10-11). */
bool usb_acm_cli_gps_enabled(void);     /* a USB GPS receiver is read        */
bool usb_acm_cli_console_enabled(void); /* the ESPNetLink console is talked to */

/** "console" (a prompt-driven console such as the ESPNetLink's, polled with
 *  `gps -p -j` while the console switch is on) or "nmea" (a GNSS receiver
 *  streaming NMEA 0183 sentences, parsed as they arrive, nothing sent to it;
 *  2026-10-07). Decided by the first well-formed sentence after the device
 *  attached. */
const char *usb_acm_cli_mode(void);

/** Well-formed NMEA sentences read from the device since it attached. */
uint32_t usb_acm_cli_nmea_sentences(void);

/**
 * The last GPS fix (a receiver's NMEA, or the dongle's periodic `gps -p -j`
 * poll; cache read: never issues USB, never blocks). @p out->valid is false
 * when no device is attached or there is no live fix; @p out->age_ms is the
 * age of a valid fix. ESP_ERR_INVALID_ARG on NULL.
 */
esp_err_t usb_acm_cli_gps_get(usb_acm_gps_t *out);

/**
 * Sink invoked after each fix refresh (RX or poll task context) with the
 * freshly parsed fix: main wires it to the autopid publisher so GPS
 * becomes first-class autopid parameters. The sink MUST NOT block or
 * touch storage. NULL unregisters. ONE sink.
 */
typedef void (*usb_acm_gps_sink_t)(const usb_acm_gps_t *fix);
void usb_acm_cli_set_gps_sink(usb_acm_gps_sink_t sink);

/**
 * Secondary fix provider consulted by usb_acm_cli_gps_get() (and so by
 * `GET /api/gps`) whenever the cache has no live fix: main wires
 * espnetlink_link's HTTP-polled cache here so the WiFi-modem topology
 * (USB data cut, no console) serves the same route. Same getter contract
 * as usb_acm_cli_gps_get(). NULL unregisters.
 */
typedef esp_err_t (*usb_acm_gps_source_t)(usb_acm_gps_t *out);
void usb_acm_cli_set_gps_fallback(usb_acm_gps_source_t source);

/**
 * Send @p line (a trailing CR is appended if missing) to the ACM console
 * and collect the response until the dongle's `esp>` prompt, or 2 s of
 * quiet (prompt-less output), or @p timeout_ms elapses. Serialized.
 * ESP_ERR_NOT_SUPPORTED while the console switch is off (nothing is sent),
 * ESP_ERR_INVALID_STATE if no device or busy.
 */
esp_err_t usb_acm_cli_command(const char *line, char *resp, size_t resp_cap,
                              size_t *resp_len, uint32_t timeout_ms);

esp_err_t usb_acm_cli_register_http(void); /* /api/usb/acm, /api/usb/acm/cmd, /api/gps */

#ifdef __cplusplus
}
#endif
