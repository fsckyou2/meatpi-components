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
 * @file j2534_server.h
 * @brief SAE J2534 PassThru device: session + channel server. See
 *        TASK_j2534_server.md. Groundwork phase: the transport-agnostic
 *        frame pump + a TCP listener doing the session/channel HANDSHAKE
 *        (HELLO/OPEN/CONNECT/DISCONNECT/CLOSE). Vehicle I/O (CAN +
 *        ISO15765), filters, periodics, ioctl, and the USB-CDC transport
 *        are later phases; data ops answer ERR_NOT_SUPPORTED for now.
 *
 * Settings ("j2534_server", reboot-to-apply): enabled (default false),
 * port (TCP, default 6809), cli.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define J2534_MAX_CHANNELS 4

esp_err_t j2534_server_init(void);
esp_err_t j2534_server_start(void);
esp_err_t j2534_server_stop(void);

typedef struct {
    bool     enabled;
    uint16_t port;
    bool     listening;
    bool     client_connected;   /* a tester is attached                 */
    bool     device_open;        /* PassThruOpen done                    */
    uint8_t  channel_count;      /* active PassThruConnect channels      */
    uint32_t frames_rx;          /* wire frames received                 */
    uint32_t frames_tx;          /* wire frames sent                     */
    uint32_t tx_refused;         /* writes the CAN node refused: it may  */
                                 /* not talk yet (listening, mismatch)   */
    bool     allow_reflash;      /* ECU-flashing gate open (unsafe)      */
    bool     allow_lan;          /* reachable on STA/USB-eth uplinks     */
    bool     exclusive;          /* runtime: autopid off the bus while a  */
                                 /* tester is attached (obd_gate hold)   */
    bool     autopid_paused;     /* the pollers acknowledged the hold    */
    char     transport[8];       /* "tcp" | "serial" | "ble" | "none"    */
} j2534_server_status_t;

/** The setting (`enabled`), stable across runtime stop: the truth BLE glue
 *  keys its channel registration on. */
bool j2534_server_is_enabled(void);
/** Started and enabled: sessions are being served. */
bool j2534_server_is_running(void);

esp_err_t j2534_server_status(j2534_server_status_t *out);

/** Runtime "exclusive" (boot default = the `exclusive` setting): while a
 *  tester is attached the background pollers (autopid) stay off the bus.
 *  Applies at once, also to a tester already attached. */
void j2534_server_set_exclusive(bool on);
bool j2534_server_exclusive(void);

esp_err_t j2534_server_register_http(void); /* GET /api/j2534 */

/* ---- transports ----------------------------------------------------------
 * One framed wire protocol (J2534_WIRE_PROTOCOL.md) over any reliable,
 * in-order byte link. A transport - the TCP listener in this component,
 * usb_cdc_device (CDC-ACM), ble_j2534 (a BLE stream channel) - supplies two
 * blocking byte ops + a context and runs ONE tester session per link-up
 * from its own task. Single tester across all transports: a second one is
 * answered ACK ERR_DEVICE_IN_USE (0x1A) to its first frame and its session
 * ends. */
typedef struct j2534_transport
{
    const char *name;   /* "tcp" | "serial" | "ble" - shown in status  */
    /* read up to n bytes, blocking up to timeout_ms; >0 bytes, 0 timeout,
     * <0 link down (ends the session) */
    int (*read)(void *ctx, uint8_t *buf, size_t n, uint32_t timeout_ms);
    /* write n bytes - one complete wire frame per call (header + payload
     * are handed over contiguously); n on success, <0 on link error */
    int (*write)(void *ctx, const uint8_t *buf, size_t n);
    void *ctx;
} j2534_transport_t;

/** Run one tester session on @p t. Blocks until the link drops, the server
 *  stops or the tester sends a malformed header. Returns at once when the
 *  server is not running (`enabled=false` / stopped). Refuses (ACK
 *  ERR_DEVICE_IN_USE, then returns) when a tester is attached on another
 *  transport. Callers loop to serve the next link-up. */
void j2534_server_serve_transport(const j2534_transport_t *t);

/* ---- legacy serial vtable (device_class=cdc) --------------------------
 * usb_cdc_device provides these two blocking byte ops and registers them,
 * then calls j2534_server_serve_serial() from its own task once the host
 * has opened the port. Thin wrappers over the transport API above. */
typedef struct {
    /* read up to n bytes, blocking up to timeout_ms; return bytes read
     * (>0), 0 on timeout, <0 on link error (session ends) */
    int (*read)(uint8_t *buf, size_t n, uint32_t timeout_ms);
    /* write n bytes; return n on success, <0 on error */
    int (*write)(const uint8_t *buf, size_t n);
} j2534_serial_transport_t;

void j2534_server_set_serial_transport(const j2534_serial_transport_t *t);

/* Run one serial tester session (blocks until the link drops or the server
 * stops). Refused immediately if a tester is already attached on another
 * transport. Caller loops to accept the next session. */
void j2534_server_serve_serial(void);

#ifdef __cplusplus
}
#endif
