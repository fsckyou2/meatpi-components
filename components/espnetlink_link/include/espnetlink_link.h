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
 * @file espnetlink_link.h
 * @brief The ESPNetLink LTE/GPS dongle as the WiCAN's internet uplink:
 *        zero-touch pairing over USB, then either plain USB-Ethernet
 *        (`usb_rndis`, the default since 2026-10-12, or `usb_ncm`) or the
 *        "WiFi modem" topology.
 *
 * Why two topologies: the WiCAN's USB host transmitter desenses the
 * dongle's GNSS while the USB-Ethernet link is active (a cold fix takes
 * longer). In `usb_rndis` / `usb_ncm` the data lines stay on and the
 * dongle is a USB-Ethernet adapter (usb_host_manager's uplink); GPS and
 * health ride HTTP over the USB link. In `wifi_modem` the WiCAN reads the
 * dongle's per-device AP key over USB, tells the dongle to cut its data
 * lines, and joins the dongle's WiFi AP (a normal wifi_manager fallback
 * network: the home network stays primary) for internet (NAPT into LTE)
 * and GPS (`GET /api/gps`). Since 2026-10-12 nothing about the cut
 * persists on the dongle (it boots with USB on every time, so a forgotten
 * AP password never locks its owner out): the WiCAN repeats identify, key
 * read and cut at every dongle boot, about 10 to 20 s of USB per boot.
 *
 * Zero-touch sequence (`auto_pair`, wifi_modem): USB attach 303A:4007 →
 * GET /api/info → GET /api/wifi_modem/credentials (served over USB only)
 * → store/verify the wifi_manager fallback slot + our ssid/device_id →
 * POST /api/wifi_modem/usb_data {enabled:false} → the device drops
 * (expected). A new/changed key means the WiCAN reboots once
 * (reboot-to-apply); an unchanged key needs nothing. Recovery (the AP
 * refuses the stored key, the cut never lands) is a VBUS cycle through
 * usb_host_manager_set_vbus(), rate-limited. In the USB modes the
 * machine ensures the dongle's class and LTE sharing instead (a fresh
 * dongle ships rndis + sharing: nothing to change, no dongle restart).
 * A dongle in usb_mode=console presents only its CDC console on USB: it
 * is reported (`dongle_console`), never paired over USB.
 *
 * Ownership split:
 *  - wifi_manager owns JOINING (the dongle's SSID is a STA candidate).
 *  - usb_host_manager/usb_eth_host own the USB host + the USB netif.
 *  - this component owns the pairing machine, knows WHICH SSID/device
 *    is the dongle, polls GPS + health over HTTP and publishes fixes
 *    through one sink (main wires it to autopid, like usb_acm_cli's).
 *
 * Settings ("espnetlink", v2): enabled (true), mode (wifi_modem|usb_ncm|
 * usb_rndis, default usb_rndis since 2026-10-12), auto_pair (true), ssid,
 * device_id, host, gps_poll_s (2), health_poll_s (10), cut_retries (2),
 * apn, apn_user, apn_password, sim_pin (2026-10-13), cli. Boot-applied.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "usb_acm_gps.h"   /* the fix struct + parser (shared with USB) */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    ESPNETLINK_MODE_WIFI_MODEM = 0,  /**< USB = power only; AP uplink  */
    ESPNETLINK_MODE_USB_NCM,         /**< USB-Ethernet uplink (CDC-NCM)  */
    ESPNETLINK_MODE_USB_RNDIS,       /**< USB-Ethernet uplink (RNDIS)    */
} espnetlink_mode_t;                 /* values mirror espnl_core_mode_t */

typedef enum
{
    ESPNETLINK_UPLINK_NONE = 0,
    ESPNETLINK_UPLINK_WIFI,          /**< STA on some other network    */
    ESPNETLINK_UPLINK_ESPNETLINK_AP, /**< STA joined to the dongle's AP */
    ESPNETLINK_UPLINK_ESPNETLINK_USB,/**< NCM link to the dongle       */
} espnetlink_uplink_t;

typedef struct
{
    bool     enabled;
    espnetlink_mode_t mode;
    bool     auto_pair;
    bool     paired;           /**< an SSID is configured                */
    char     ssid[33];         /**< configured ESPNetLink SSID           */
    char     device_id[13];    /**< paired dongle (12 hex, "" = unknown) */
    bool     sta_connected;
    espnetlink_uplink_t uplink;
    char     host[16];         /**< dongle address in use ("" = none)    */
    /* USB side */
    bool     usb_attached;     /**< NCM link up to a 303A:4007 device    */
    bool     dongle_console;   /**< 303A:4007 on the port as a CDC console
                                    alone (dongle usb_mode=console): no
                                    USB Ethernet, nothing to pair over USB */
    const char *pair_state;    /**< state machine, as text               */
    uint32_t cuts;             /**< successful usb_data cuts             */
    uint32_t vbus_cycles;      /**< recovery power cycles                */
    uint32_t pair_errors;      /**< failed identify/key/cut sequences    */
    bool     pair_blocked_factory_pw; /**< zero-touch held: the AP still
                                           has the factory password (the
                                           store would be refused)       */
    char     last_error[160];  /**< last pairing failure text, "" = none */
    char     dongle_fw[24];    /**< /api/info fw_version, "" = not seen  */
    int      dongle_api;       /**< /api/info api_level, 0 = not seen    */
    bool     health_unsupported; /**< /api/wifi_modem answered 404: the
                                      dongle firmware has no health API  */
    /* GPS (last poll) */
    bool     gps_valid;
    uint32_t gps_age_ms;       /**< since the last valid fix; 0 if none  */
    /* dongle health (last /api/wifi_modem poll) */
    bool     health_valid;
    bool     lte_connected;    /**< PPP up on the dongle                  */
    int      rssi_dbm;
    char     operator_name[24];
    char     network_type[12];
    bool     dongle_gps_fix;
    bool     dongle_usb_data;  /**< data lines routed (live state)       */
    /* 2026-10-07 (dongle api 8; "" from an older one): the SIM as the
       dongle found it, the bring-up stage, the carrier address, and how
       old this reading is (the pairing pass reads it over the cable, so
       a WiCAN on its home WiFi keeps the last one instead of nothing) */
    char     dongle_sim[12];   /**< "ready" | "missing" | "pin" | "pin_wrong"
                                    | "unknown" | "" (pin_wrong: the SIM
                                    refused the PIN the dongle holds)      */
    char     dongle_stage[20];
    char     dongle_ip[16];
    uint32_t health_age_ms;    /**< since the reading; 0 when none       */
    /* the GPS hint (2026-10-13): seconds away from home on the wire with
       LTE up and no fix anywhere (0 otherwise), and the verdict past
       ESPNL_NOFIX_HINT_S (the UI nudges toward WiFi modem) */
    uint32_t no_fix_s;
    bool     no_fix_hint;
    /* the carrier settings relayed to the dongle (espnetlink.apn, ...) */
    char     apn[64];          /**< "" = the dongle keeps its own         */
    bool     carrier_synced;   /**< the last pass found the dongle holding
                                    them (or nothing to relay)           */
    char     carrier_note[96]; /**< why not, "" = fine                    */
    /* counters */
    uint32_t polls;
    uint32_t failures;
    uint32_t link_ups;         /**< NONE/HOME -> ESPNL transitions        */
} espnetlink_link_status_t;

/** Register settings + log descriptor. No network. */
esp_err_t espnetlink_link_init(void);

/** Start the link task (no-op when !enabled). Call after wifi_manager
 *  and usb_host_manager have started. */
esp_err_t espnetlink_link_start(void);
esp_err_t espnetlink_link_stop(void);

/** One sink for parsed fixes (poll-task context, must not block).
 *  Same signature as usb_acm_cli's sink so one glue serves both. */
typedef void (*espnetlink_gps_sink_t)(const usb_acm_gps_t *fix);
void espnetlink_link_set_gps_sink(espnetlink_gps_sink_t sink);

/** Last cached fix (never touches the network). ESP_ERR_NOT_FOUND when
 *  there is no live fix: the same contract as usb_acm_cli_gps_get(),
 *  so it doubles as that component's fallback provider. */
esp_err_t espnetlink_link_gps_get(usb_acm_gps_t *out);

esp_err_t espnetlink_link_status(espnetlink_link_status_t *out);

/**
 * Manual pairing: store @p ssid/@p password as a wifi_manager fallback
 * network (first free slot, or the slot already holding that SSID) and
 * set this component's ssid + enabled. Persists immediately; reboot to
 * apply. The zero-touch path uses the same store.
 * @param[out] slot_out  the wifi_manager slot used (0 = primary sta_ssid
 *                       already matched, 1..5 = fallbackN). May be NULL.
 */
esp_err_t espnetlink_link_pair(const char *ssid, const char *password,
                               int *slot_out);

/**
 * Re-pair: cycle the dongle's VBUS so it re-enumerates and the key is
 * read again (also the recovery path the engine uses by itself).
 * ESP_ERR_INVALID_STATE when the USB host is not up.
 */
esp_err_t espnetlink_link_repair(void);

/** `GET /api/espnetlink`, `POST /api/espnetlink/pair`,
 *  `POST /api/espnetlink/repair` */
esp_err_t espnetlink_link_register_http(void);
esp_err_t espnetlink_link_register_cli(void);

#ifdef __cplusplus
}
#endif
