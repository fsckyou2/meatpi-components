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
 * @file dev_status_usb.h
 * @brief The USB connector summary (2026-10-13): what is on the connector
 *        and its one-line state, for the web UI's header pill and
 *        ESPNetLink bar on the status poll they make anyway (`usb` on
 *        GET /api/status). IDF-free: the kind's spelling is host-tested
 *        with the other formatting helpers. The composition root fills it
 *        (it knows the USB components; dev_status_manager must not).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    DEV_STATUS_USB_NONE = 0,   /**< nothing enumerated (no `usb` object) */
    DEV_STATUS_USB_ESPNETLINK, /**< the ESPNetLink dongle                */
    DEV_STATUS_USB_GPS,        /**< a USB GPS receiver                   */
    DEV_STATUS_USB_ETH,        /**< a USB Ethernet adapter               */
    DEV_STATUS_USB_OTHER,      /**< something WiCAN does not drive       */
} dev_status_usb_kind_t;

typedef struct
{
    dev_status_usb_kind_t kind;
    /* espnetlink */
    char     device_id[13];    /**< the identified dongle (12 hex, or "") */
    char     sim[12];          /**< "ready" | "missing" | "pin" | "pin_wrong"
                                    | "unknown" | ""                       */
    bool     lte;              /**< the dongle's PPP is up                */
    char     operator_name[24];
    uint32_t no_fix_s;         /**< the GPS hint's count (espnetlink_link) */
    bool     no_fix_hint;      /**< its verdict: switch to WiFi modem     */
    /* espnetlink + gps */
    bool     fix;
    int      satellites;
    /* eth */
    bool     link;             /**< the adapter has an address            */
    char     ip[16];
    /* gps, eth, other */
    char     product[48];      /**< the enumerated product string, or ""  */
} dev_status_usb_t;

typedef bool (*dev_status_usb_provider_t)(dev_status_usb_t *out);

/** The wire spelling of a kind ("espnetlink" | "gps" | "eth" | "other" |
 *  "none"); pure. */
const char *dev_status_manager_usb_kind_str(dev_status_usb_kind_t k);

#ifdef __cplusplus
}
#endif
