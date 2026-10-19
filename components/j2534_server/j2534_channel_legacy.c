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
 * @file j2534_channel_legacy.c
 * @brief The J1850 VPW side of a J2534 channel: the GM Class 2 serial bus
 *        reached through the OBD chip (MIC3624, ELM/STN dialect), modelled
 *        on the nightjoker7 fork's working path but rebuilt on the v6
 *        obd_chip public API (claim/transaction model) and the chip's own
 *        VTFullyRequestCk USDT instead of a software SF/FF/CF framer.
 *
 * A J1850 message is [3-byte header][payload], carried whole in a
 * PASSTHRU_MSG's data (no CAN-id prefix). CONNECT runs the VPW init on the
 * chip; WRITE_MSGS sets the header (cached) and sends the payload, then
 * parses the chip's reply lines into this slot's RX ring, which the RX
 * pump drains through j2534_legacy_poll. The chip does the multi-frame
 * segmentation and reassembly, rides out UDS 7F xx 78 responsePending, and
 * (with the j2534_server "exclusive" default) autopid is off the bus while
 * a tester is attached, so these chip transactions never interleave.
 *
 * Request/response only: unsolicited bus traffic between requests is not
 * captured (no monitor-mode task). GM 4x programming speed (service $A1 at
 * 41.6 kbit/s) is not implemented; SET_CONFIG DATA_RATE is accepted but the
 * bus stays at 10.4 kbit/s. Every function runs with the channel lock held
 * by j2534_channel.c.
 */
#include "j2534_channel_private.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "obd_chip.h"

static const char *TAG = "j2534_server";

/* a reply longer than this is truncated (a W line): J1850 diagnostic reads
 * and flash responses are short; a bigger block read wants the native CAN
 * path. The RX ring holds a handful of replies for one request. */
#define LEG_MSG_MAX   520
#define LEG_RING      8
/* the chip transaction: ATST bounds the per-response wait on the chip; this
 * outer cap sits under the DLL's 8 s ACK wait for a slow programming op */
#define LEG_TXN_MS    2000
#define LEG_WRITE_MS  6000
#define LEG_CMD_MS    800

/* the chip's reply text for one request (echo + prompt stripped): PSRAM,
 * one tester, one transaction at a time under the channel lock */
EXT_RAM_BSS_ATTR static char s_resp[4096];

typedef struct
{
    bool     used;
    int      slot;
    uint32_t seq;        /* FIFO order across the shared ring */
    uint32_t rx_status;  /* 0 = received; TX_MSG_TYPE|TX_INDICATION = TxDone */
    uint32_t ts_us;
    uint16_t len;
    uint8_t  data[LEG_MSG_MAX];
} leg_entry_t;

/* one ring shared by the (normally single) legacy channel, each entry
 * tagged with its slot; guarded by the channel lock like every function
 * here. PSRAM: no ISR or flash path touches it. */
EXT_RAM_BSS_ATTR static leg_entry_t s_ring[LEG_RING];
static uint32_t s_seq;

static uint32_t now_us(void)
{
    return (uint32_t)esp_timer_get_time();
}

/* ---- the RX ring ------------------------------------------------------ */

static leg_entry_t *ring_alloc(int slot)
{
    leg_entry_t *free_e = NULL;
    leg_entry_t *oldest = NULL;

    for (int i = 0; i < LEG_RING; i++)
    {
        if (!s_ring[i].used)
        {
            free_e = &s_ring[i];
            break;
        }

        if (oldest == NULL || s_ring[i].seq < oldest->seq)
        {
            oldest = &s_ring[i];
        }
    }

    leg_entry_t *e = (free_e != NULL) ? free_e : oldest;

    e->used = true;
    e->slot = slot;
    e->seq = ++s_seq;
    return e;
}

static void ring_push(int slot, uint32_t rx_status, const uint8_t *data,
                      size_t len)
{
    if (len > LEG_MSG_MAX)
    {
        ESP_LOGW(TAG, "J1850 reply of %u B truncated to %d (the native CAN "
                      "path carries larger reads)", (unsigned)len, LEG_MSG_MAX);
        len = LEG_MSG_MAX;
    }

    leg_entry_t *e = ring_alloc(slot);

    e->rx_status = rx_status;
    e->ts_us = now_us();
    e->len = (uint16_t)len;

    if (len > 0 && data != NULL)
    {
        memcpy(e->data, data, len);
    }
}

/* the oldest entry for @p slot, or NULL */
static leg_entry_t *ring_oldest(int slot)
{
    leg_entry_t *best = NULL;

    for (int i = 0; i < LEG_RING; i++)
    {
        if (s_ring[i].used && s_ring[i].slot == slot &&
            (best == NULL || s_ring[i].seq < best->seq))
        {
            best = &s_ring[i];
        }
    }

    return best;
}

void j2534_legacy_drop(int slot)
{
    for (int i = 0; i < LEG_RING; i++)
    {
        if (s_ring[i].used && s_ring[i].slot == slot)
        {
            s_ring[i].used = false;
        }
    }
}

/* ---- the chip -------------------------------------------------------- */

bool j2534_legacy_provider_ready(void)
{
    return obd_chip_ready();
}

/* one ELM/STN command on the chip; the reply lands in s_resp */
static esp_err_t chip_cmd(const char *cmd, uint32_t timeout_ms)
{
    return obd_chip_request(cmd, s_resp, sizeof(s_resp),
                            pdMS_TO_TICKS(timeout_ms));
}

/* the J1850 VPW init: the nightjoker7 fork's proven GM Class 2 sequence,
 * minus the EEPROM-unsafe ATSP (ATTP keeps it in RAM; the obd_chip guard
 * would rewrite ATSP anyway). All bench-probed on the MIC3624 2026-10-19.
 * Only ATTP2 is load-bearing; the rest are best-effort. */
static uint32_t run_vpw_init(void)
{
    static const char *SEQ[] = {
        "ATD",                   /* all to defaults                        */
        "ATH1",                  /* headers on: J2534 needs the 3 hdr bytes */
        "ATS0",                  /* no spaces in replies (clean parse)     */
        "ATE0",                  /* echo off (obd_chip strips it anyway)   */
        "ATAL",                  /* allow long messages                    */
        "ATAT0",                 /* adaptive timing off (J2534 fixed)      */
        "ATST19",               /* per-response wait ~100 ms              */
        "AT IFR0",               /* no in-frame-response display           */
        "ATSR F0",               /* receive to the tester address F0       */
        "ATAR",                  /* auto receive                           */
        "STFCP",                 /* clear the chip's programmable filters  */
        "STFAP 00F000,00FF00",   /* pass replies to F0 (tester)            */
        "STFAP 00FE00,00FF00",   /* pass functional/broadcast FE           */
        "STFAP 006C00,00FF00",   /* pass ECUs that answer on 6C            */
    };

    /* ATTP2: try J1850 VPW (protocol 2), RAM-only. The one that must take. */
    if (chip_cmd("ATTP2", LEG_CMD_MS) != ESP_OK)
    {
        ESP_LOGW(TAG, "J1850 VPW: the OBD chip did not answer ATTP2");
        return J2534_ERR_FAILED;
    }

    if (strchr(s_resp, '?') != NULL)
    {
        ESP_LOGW(TAG, "J1850 VPW: the OBD chip rejected ATTP2 (%s)", s_resp);
        return J2534_ERR_FAILED;
    }

    for (size_t i = 0; i < sizeof(SEQ) / sizeof(SEQ[0]); i++)
    {
        (void)chip_cmd(SEQ[i], LEG_CMD_MS);
    }

    ESP_LOGI(TAG, "J1850 VPW channel up (GM Class 2 over the OBD chip)");
    return J2534_STATUS_NOERROR;
}

uint32_t j2534_legacy_connect(j2534_channel_t *c)
{
    if (c->protocol != J2534_PROT_J1850VPW)
    {
        /* PWM / ISO9141 / ISO14230 share this file when built */
        ESP_LOGW(TAG, "legacy protocol %lu is not built yet",
                 (unsigned long)c->protocol);
        return J2534_ERR_NOT_SUPPORTED;
    }

    if (!obd_chip_ready())
    {
        ESP_LOGW(TAG, "J1850 VPW needs the OBD chip; it is not ready");
        return J2534_ERR_FAILED;
    }

    c->leg_have_hdr = false; /* the slot's RX ring was cleared by teardown */

    if (obd_chip_txn_begin(pdMS_TO_TICKS(LEG_TXN_MS)) != ESP_OK)
    {
        ESP_LOGW(TAG, "J1850 VPW: the OBD chip was busy at CONNECT");
        return J2534_ERR_FAILED;
    }

    uint32_t st = run_vpw_init();

    obd_chip_txn_end();
    return st;
}

/* ---- write ------------------------------------------------------------ */

/* parse the chip's reply text into the slot's RX ring: one J1850 message
 * per line (header + data with ATH1), 7F xx 78 responsePending interims
 * counted and dropped, status words skipped */
static void parse_replies(int slot, const char *resp)
{
    const char *p = resp;

    while (*p != '\0')
    {
        const char *eol = p;

        while (*eol != '\0' && *eol != '\r' && *eol != '\n')
        {
            eol++;
        }

        size_t len = (size_t)(eol - p);

        if (len > 0 && !j2534_legacy_line_is_status(p, len))
        {
            uint8_t msg[LEG_MSG_MAX];
            size_t n = j2534_legacy_decode_line(p, len, msg, sizeof(msg));

            /* a real J1850 message is the 3 header bytes plus >= 1 data
               byte; drop responsePending interims, keep the rest */
            if (n >= (size_t)(J2534_J1850_HDR + 1) &&
                !j2534_legacy_is_pending(msg, n))
            {
                ring_push(slot, 0, msg, n);
            }
        }

        p = (*eol == '\0') ? eol : eol + 1;
    }
}

uint32_t j2534_legacy_write(j2534_channel_t *c, int slot,
                            const j2534_msg_t *msg, bool indicate)
{
    const uint8_t *hdr;
    const uint8_t *payload;
    size_t n;

    if (!j2534_legacy_split(msg, &hdr, &payload, &n))
    {
        return J2534_ERR_INVALID_MSG;
    }

    if (j2534_ch_reflash_sid_blocked(payload[0]))
    {
        ESP_LOGW(TAG, "ECU flashing disabled: rejected a UDS reprogramming "
                      "service (enable j2534_server.allow_reflash to permit)");
        return J2534_ERR_NOT_SUPPORTED;
    }

    if (!obd_chip_ready())
    {
        return J2534_ERR_FAILED;
    }

    char cmd[2 * LEG_MSG_MAX + 32];

    if (j2534_legacy_build_request(payload, n, J2534_J1850_SF_MAX, cmd,
                                   sizeof(cmd)) == 0)
    {
        return J2534_ERR_INVALID_MSG;
    }

    if (obd_chip_txn_begin(pdMS_TO_TICKS(LEG_TXN_MS)) != ESP_OK)
    {
        return J2534_ERR_TIMEOUT;
    }

    /* set the header only when it changed (the chip keeps the last ATSH) */
    if (!c->leg_have_hdr || memcmp(c->leg_hdr, hdr, J2534_J1850_HDR) != 0)
    {
        char at[16];

        if (j2534_legacy_header_cmd(hdr, at, sizeof(at)) > 0 &&
            chip_cmd(at, LEG_CMD_MS) == ESP_OK)
        {
            memcpy(c->leg_hdr, hdr, J2534_J1850_HDR);
            c->leg_have_hdr = true;
        }
    }

    esp_err_t err = chip_cmd(cmd, LEG_WRITE_MS);

    obd_chip_txn_end();

    if (err != ESP_OK)
    {
        ESP_LOGD(TAG, "J1850 VPW send: %s", esp_err_to_name(err));
        return (err == ESP_ERR_TIMEOUT) ? J2534_ERR_TIMEOUT : J2534_ERR_FAILED;
    }

    /* a TxDone echo first (SAE J2534-1 loopback: the header only, never the
       whole payload: a big echo overread the DLL's buffer in the fork) */
    if (indicate)
    {
        ring_push(slot, J2534_RX_TX_MSG_TYPE | J2534_RX_TX_INDICATION, hdr,
                  J2534_J1850_HDR);
    }

    parse_replies(slot, s_resp);
    return J2534_STATUS_NOERROR;
}

/* ---- poll ------------------------------------------------------------- */

bool j2534_legacy_poll(j2534_channel_t *c, int slot, j2534_msg_t *out)
{
    leg_entry_t *e = ring_oldest(slot);

    if (e == NULL)
    {
        return false;
    }

    /* a received message passes the channel's PASS/BLOCK filters (over the
       whole J1850 message, header first); a TxDone echo is delivered as-is */
    if (e->rx_status == 0 &&
        !j2534_ch_filters_pass(c, e->data, e->len))
    {
        e->used = false;
        return false; /* dropped by a filter; the pump polls again */
    }

    memset(out, 0, sizeof(*out));
    out->protocol_id = J2534_PROT_J1850VPW;
    out->rx_status = e->rx_status;
    out->timestamp = e->ts_us;
    out->data_size = e->len;
    memcpy(out->data, e->data, e->len);

    e->used = false;
    return true;
}
