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
 * @file j2534_channel.c
 * @brief J2534 channel transports (see j2534_channel.h). CAN → can_manager
 *        raw frames; ISO15765 → the registered ISO-TP provider
 *        (can_isotp.h) on the shared bus.
 */
#include "j2534_channel.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "can_isotp.h"
#include "can_manager.h"
#include "obd_gate.h"
#include "can_core.h"

#include "j2534_server.h"   /* J2534_MAX_CHANNELS */
#include "j2534_server_private.h"

static const char *TAG = "j2534_server";

#define CH_CAN_QLEN 32

/* ECU-flashing gate (see j2534_channel_set_allow_reflash). Default closed:
 * reprogramming is blocked until the operator explicitly enables it. */
static bool s_allow_reflash;

void j2534_channel_set_allow_reflash(bool allow)
{
    s_allow_reflash = allow;
}

/* UDS memory-transfer services: the ones that actually read/write ECU
 * flash. Blocking any of these makes a reprogramming sequence impossible
 * (you cannot write memory without RequestDownload + TransferData). */
static bool uds_is_reflash_sid(uint8_t sid)
{
    switch (sid)
    {
        case 0x34: /* RequestDownload      */
        case 0x35: /* RequestUpload        */
        case 0x36: /* TransferData         */
        case 0x37: /* RequestTransferExit  */
            return true;
        default:
            return false;
    }
}

/* True if this write must be rejected because ECU flashing is disabled.
 * ISO15765: the UDS SID is data[0]. Raw CAN: data = [4-byte id][frame];
 * a hand-rolled ISO-TP single frame carries the SID after the PCI nibble
 * (SF: frame[1], FF: frame[2]), checked so the CAN channel can't be used
 * to smuggle a reflash past the ISO15765 gate. */
static bool reflash_blocked(uint32_t protocol, const j2534_msg_t *msg)
{
    if (s_allow_reflash || msg->data_size == 0)
    {
        return false;
    }

    if (protocol == J2534_PROT_ISO15765)
    {
        return uds_is_reflash_sid(msg->data[0]);
    }

    if (protocol == J2534_PROT_CAN && msg->data_size >= 6)
    {
        const uint8_t *f = msg->data + 4; /* skip the 4-byte CAN id */
        uint8_t pci = (uint8_t)(f[0] >> 4);

        if (pci == 0x0)                       /* single frame */
        {
            return uds_is_reflash_sid(f[1]);
        }
        if (pci == 0x1 && msg->data_size >= 7) /* first frame */
        {
            return uds_is_reflash_sid(f[2]);
        }
    }

    return false;
}

typedef struct {
    uint32_t id;                        /* filter id (1-based)         */
    uint32_t type;                      /* PASS / BLOCK                */
    uint8_t  mask[8];
    uint8_t  pattern[8];
    uint8_t  len;
} can_filter_t;

typedef struct {
    bool     in_use;
    uint32_t protocol;
    uint32_t flags;

    /* CAN raw */
    QueueHandle_t can_q;
    int           can_sub;
    can_filter_t  filters[J2534_CH_MAX_FILTERS];
    int           n_filters;

    /* ISO15765 (session on the registered ISO-TP provider) */
    can_isotp_session_t isotp;
    bool     isotp_bound;
    uint32_t tx_id;
    uint32_t rx_id;
    bool     ext;

    uint32_t next_filter_id;
} channel_t;

static channel_t s_ch[J2534_MAX_CHANNELS];
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static bool s_refused_said;    /* the refusal line went out this connect */

/* ---- helpers ---------------------------------------------------------- */

static uint32_t now_us(void) { return (uint32_t)esp_timer_get_time(); }

/* A write the CAN node refused: it may not talk yet (listening for the
   bitrate on a silent bus with `auto`, a mismatch, listen-only). Counted
   on /api/j2534, said once per CONNECT (can_core says why, once per
   episode). 2026-10-17: a tester read the silent refusal as "no comm". */
static void say_refused(const char *what)
{
    j2534_srv_count_tx_refused();

    if (s_refused_said)
    {
        return;
    }

    s_refused_said = true;
    ESP_LOGW(TAG, "%s write refused: the CAN node may not talk yet (see "
             "the can_core line: no bitrate proven, a mismatch, or "
             "listen-only); set the bit rate, or CONNECT with the bus's",
             what);
}

uint32_t j2534_channel_bus_baud(uint32_t proto, uint32_t baud_bps)
{
    if (proto != J2534_PROT_CAN && proto != J2534_PROT_ISO15765)
    {
        return J2534_STATUS_NOERROR; /* not a CAN protocol: nothing to check */
    }

    /* the bus has its bitrate (the device's setting, what the node read
       off the wire, or what a tester stated and it runs at): a tester that
       asks for another one is told so, not served at the wrong rate */
    can_manager_status_t cst;
    uint32_t bus_kbps = 0;

    if (can_manager_status(&cst) == ESP_OK && cst.running)
    {
        bus_kbps = (cst.link.verified || cst.link.adopted)
                       ? cst.link.baud_kbps
                   : cst.baud_auto ? 0
                                   : cst.baud_kbps;
    }

    if (j2534_connect_baud_check(baud_bps, bus_kbps) != J2534_STATUS_NOERROR)
    {
        ESP_LOGW(TAG, "CONNECT at %lu bit/s refused: the CAN bus runs at "
                      "%lu kbit/s", (unsigned long)baud_bps,
                 (unsigned long)bus_kbps);
        return J2534_ERR_INVALID_BAUDRATE;
    }

    /* auto with nothing proven: the tester's baud is the best word there
       is on a silent bus (2026-10-17: a tester on a gatewayed OBD port,
       which speaks only when asked, read the refused writes as "connected,
       no comm to the CAN bus"). Not taken (the bus not silent, unreadable
       traffic seen, a rate this node does not know): the channel still
       opens and its writes are refused, with a W line per CONNECT. */
    if (bus_kbps == 0 && baud_bps >= 1000u)
    {
        esp_err_t ad = can_manager_adopt_baud(baud_bps / 1000u);

        if (ad == ESP_OK)
        {
            ESP_LOGI(TAG, "CONNECT at %lu bit/s: the bus was silent, the "
                          "node now runs at the tester's bitrate",
                     (unsigned long)baud_bps);
        }
        else if (ad == ESP_ERR_NOT_SUPPORTED)
        {
            ESP_LOGW(TAG, "CONNECT at %lu bit/s: no bitrate proven on the "
                          "bus and the tester's was not taken (the bus not "
                          "silent, unreadable traffic seen, or not a rate "
                          "this node knows): writes are refused until the "
                          "bus talks; set the bit rate",
                     (unsigned long)baud_bps);
        }
    }

    return J2534_STATUS_NOERROR;
}

static void put_id_be(uint8_t *p, uint32_t id)
{
    p[0] = (uint8_t)(id >> 24); p[1] = (uint8_t)(id >> 16);
    p[2] = (uint8_t)(id >> 8);  p[3] = (uint8_t)id;
}

static uint32_t get_id_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static bool isotp_bind(channel_t *c)
{
    if (can_isotp() == NULL)
    {
        ESP_LOGE(TAG, "ISO15765 not available in this build "
                      "(no ISO-TP provider registered)");
        return false;
    }

    if (can_manager_core_handle() == NULL)
    {
        return false;
    }

    if (c->isotp_bound)
    {
        can_isotp()->close(c->isotp);
        c->isotp_bound = false;
    }

    can_isotp_cfg_t cfg = {
        .tx_id        = c->tx_id,
        .rx_id        = c->rx_id,
        .ext_id       = c->ext,
        .block_size   = 0,
        .stmin_ms     = 0,
        .padding_byte = (c->flags & J2534_TX_ISO15765_FRAME_PAD) ? 0xAA : 0x00,
        .use_padding  = (c->flags & J2534_TX_ISO15765_FRAME_PAD) != 0,
    };

    if (can_isotp()->open(&cfg, &c->isotp) != ESP_OK)
    {
        ESP_LOGE(TAG, "isotp bind failed (tx %03lX rx %03lX)",
                 (unsigned long)c->tx_id, (unsigned long)c->rx_id);
        return false;
    }

    c->isotp_bound = true;
    ESP_LOGI(TAG, "ISO15765 channel bound tx %03lX rx %03lX",
             (unsigned long)c->tx_id, (unsigned long)c->rx_id);
    return true;
}

/* ---- lifecycle -------------------------------------------------------- */

void j2534_channel_init(void)
{
    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
}

static void teardown(channel_t *c)
{
    if (c->can_sub >= 0)
    {
        can_manager_unsubscribe_queue(c->can_sub);
        c->can_sub = -1;
    }
    if (c->can_q != NULL)
    {
        vQueueDelete(c->can_q);
        c->can_q = NULL;
    }
    if (c->isotp_bound)
    {
        can_isotp()->close(c->isotp);
        c->isotp_bound = false;
    }
    obd_gate_release(c); /* idempotent: only if this channel holds it */
    memset(c, 0, sizeof(*c));
    c->can_sub = -1;
}

void j2534_channel_reset_all(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < J2534_MAX_CHANNELS; i++)
    {
        if (s_ch[i].in_use || s_ch[i].can_q || s_ch[i].isotp_bound)
        {
            teardown(&s_ch[i]);
        }
    }
    xSemaphoreGive(s_lock);
}

uint32_t j2534_channel_connect(int slot, uint32_t protocol, uint32_t flags,
                               uint32_t tx_id, uint32_t rx_id, bool ext)
{
    if (slot < 0 || slot >= J2534_MAX_CHANNELS)
    {
        return J2534_ERR_INVALID_CHANNEL_ID;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    channel_t *c = &s_ch[slot];
    teardown(c);
    s_refused_said = false;
    c->in_use = true;
    c->protocol = protocol;
    c->flags = flags;
    c->tx_id = tx_id;
    c->rx_id = rx_id;
    c->ext = ext;
    c->next_filter_id = 1;

    uint32_t st = J2534_STATUS_NOERROR;

    if (protocol == J2534_PROT_CAN)
    {
        c->can_q = xQueueCreate(CH_CAN_QLEN, sizeof(can_core_frame_t));
        if (c->can_q == NULL ||
            can_manager_subscribe_queue(c->can_q, 0, 0, ext, true,
                                        &c->can_sub) != ESP_OK)
        {
            teardown(c);
            st = J2534_ERR_FAILED;
        }
        else
        {
            (void)can_manager_subscriber_name(c->can_sub, "j2534");
        }
    }
    else if (protocol == J2534_PROT_ISO15765)
    {
        /* bind now if the tester gave tx/rx on CONNECT; otherwise wait
         * for a FLOW_CONTROL filter to supply them */
        if (tx_id != 0 && rx_id != 0 && !isotp_bind(c))
        {
            teardown(c);
            st = J2534_ERR_FAILED;
        }
    }
    else
    {
        teardown(c);
        st = J2534_ERR_NOT_SUPPORTED;
    }

    xSemaphoreGive(s_lock);
    return st;
}

void j2534_channel_disconnect(int slot)
{
    if (slot < 0 || slot >= J2534_MAX_CHANNELS)
    {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    teardown(&s_ch[slot]);
    xSemaphoreGive(s_lock);
}

bool j2534_channel_active(int slot)
{
    return slot >= 0 && slot < J2534_MAX_CHANNELS && s_ch[slot].in_use;
}

/* ---- write ------------------------------------------------------------ */

uint32_t j2534_channel_write(int slot, const j2534_msg_t *msg)
{
    if (slot < 0 || slot >= J2534_MAX_CHANNELS || !s_ch[slot].in_use)
    {
        return J2534_ERR_INVALID_CHANNEL_ID;
    }

    channel_t *c = &s_ch[slot];
    uint32_t st = J2534_STATUS_NOERROR;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (reflash_blocked(c->protocol, msg))
    {
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "ECU flashing disabled: rejected a UDS reprogramming "
                 "service (enable j2534_server.allow_reflash to permit)");
        return J2534_ERR_NOT_SUPPORTED;
    }

    if (c->protocol == J2534_PROT_CAN)
    {
        if (msg->data_size < 4 || msg->data_size > 12)
        {
            st = J2534_ERR_INVALID_MSG;
        }
        else
        {
            uint32_t id = get_id_be(msg->data);
            bool ext = (msg->tx_flags & J2534_TX_CAN_29BIT_ID) != 0;
            uint8_t dlc = (uint8_t)(msg->data_size - 4);
            esp_err_t err = can_manager_send(id, ext, false, msg->data + 4,
                                             dlc);

            if (err != ESP_OK)
            {
                st = J2534_ERR_FAILED;

                if (err == ESP_ERR_INVALID_STATE)
                {
                    say_refused("CAN");
                }
            }
        }
    }
    else if (c->protocol == J2534_PROT_ISO15765)
    {
        if (!c->isotp_bound)
        {
            st = J2534_ERR_NO_FLOW_CONTROL;
        }
        else if (msg->data_size == 0)
        {
            st = J2534_ERR_INVALID_MSG;
        }
        else if (!can_manager_tx_ready())
        {
            say_refused("ISO15765");
            st = J2534_ERR_FAILED;
        }
        else
        {
            /* an ESP-side requester on the shared bus: hold the
               conversation gate from the request until READ_MSGS
               delivers the reply (or the hold self-expires) so the
               MIC chip (autopid) does not interleave; fail-open
               (obd_gate.h). Owner identity = the channel. */
            obd_gate_acquire(c, OBD_GATE_WAIT_MS);

            if (can_isotp()->send(c->isotp, msg->data,
                                  msg->data_size, 1000) != ESP_OK)
            {
                obd_gate_release(c);
                st = J2534_ERR_FAILED;
            }
        }
    }

    xSemaphoreGive(s_lock);
    return st;
}

/* ---- filters ---------------------------------------------------------- */

uint32_t j2534_channel_add_filter(int slot, uint32_t type,
                                  const j2534_msg_t *mask,
                                  const j2534_msg_t *pattern,
                                  const j2534_msg_t *flow_control,
                                  uint32_t *filter_id)
{
    if (slot < 0 || slot >= J2534_MAX_CHANNELS || !s_ch[slot].in_use)
    {
        return J2534_ERR_INVALID_CHANNEL_ID;
    }

    channel_t *c = &s_ch[slot];
    uint32_t st = J2534_STATUS_NOERROR;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (type == J2534_FLOW_CONTROL_FILTER)
    {
        /* ISO15765: pattern id = rx, flow-control id = tx (first 4 bytes
         * of each message's data, our convention) */
        if (c->protocol != J2534_PROT_ISO15765 ||
            pattern == NULL || flow_control == NULL ||
            pattern->data_size < 4 || flow_control->data_size < 4)
        {
            st = J2534_ERR_INVALID_MSG;
        }
        else
        {
            c->rx_id = get_id_be(pattern->data);
            c->tx_id = get_id_be(flow_control->data);
            c->ext = (pattern->tx_flags & J2534_TX_CAN_29BIT_ID) != 0;
            if (isotp_bind(c))
            {
                *filter_id = c->next_filter_id++;
            }
            else
            {
                st = J2534_ERR_FAILED;
            }
        }
    }
    else if (type == J2534_PASS_FILTER || type == J2534_BLOCK_FILTER)
    {
        if (c->n_filters >= J2534_CH_MAX_FILTERS || mask == NULL ||
            pattern == NULL)
        {
            st = J2534_ERR_FAILED; /* table full */
        }
        else
        {
            can_filter_t *f = &c->filters[c->n_filters];
            uint8_t n = (uint8_t)(mask->data_size < 8 ? mask->data_size : 8);
            f->id = c->next_filter_id++;
            f->type = type;
            f->len = n;
            memcpy(f->mask, mask->data, n);
            memcpy(f->pattern, pattern->data,
                   pattern->data_size < n ? pattern->data_size : n);
            c->n_filters++;
            *filter_id = f->id;
        }
    }
    else
    {
        st = J2534_ERR_INVALID_MSG;
    }

    xSemaphoreGive(s_lock);
    return st;
}

uint32_t j2534_channel_stop_filter(int slot, uint32_t filter_id)
{
    if (slot < 0 || slot >= J2534_MAX_CHANNELS || !s_ch[slot].in_use)
    {
        return J2534_ERR_INVALID_CHANNEL_ID;
    }

    channel_t *c = &s_ch[slot];
    uint32_t st = J2534_ERR_INVALID_FILTER_ID;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < c->n_filters; i++)
    {
        if (c->filters[i].id == filter_id)
        {
            c->filters[i] = c->filters[--c->n_filters];
            st = J2534_STATUS_NOERROR;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return st;
}

void j2534_channel_clear_filters(int slot)
{
    if (slot < 0 || slot >= J2534_MAX_CHANNELS)
    {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ch[slot].n_filters = 0;
    xSemaphoreGive(s_lock);
}

/* apply PASS/BLOCK filters to a raw CAN frame's [id][data] view */
static bool can_filters_pass(channel_t *c, const uint8_t *view, size_t len)
{
    if (c->n_filters == 0)
    {
        return true; /* no filters = pass all (matches J2534 CAN default) */
    }

    bool any_pass = false, has_pass = false;

    for (int i = 0; i < c->n_filters; i++)
    {
        can_filter_t *f = &c->filters[i];
        bool m = j2534_filter_match(f->mask, f->len, f->pattern, f->len,
                                    view, len);
        if (f->type == J2534_BLOCK_FILTER && m)
        {
            return false;
        }
        if (f->type == J2534_PASS_FILTER)
        {
            has_pass = true;
            any_pass = any_pass || m;
        }
    }

    return has_pass ? any_pass : true;
}

/* ---- rx poll ---------------------------------------------------------- */

bool j2534_channel_poll_rx(int slot, j2534_msg_t *out)
{
    if (slot < 0 || slot >= J2534_MAX_CHANNELS)
    {
        return false;
    }

    channel_t *c = &s_ch[slot];
    if (!c->in_use)
    {
        return false;
    }

    bool got = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (c->protocol == J2534_PROT_CAN && c->can_q != NULL)
    {
        can_core_frame_t fr;
        if (xQueueReceive(c->can_q, &fr, 0) == pdTRUE)
        {
            uint8_t view[12];
            put_id_be(view, fr.id);
            memcpy(view + 4, fr.data, fr.dlc);
            if (can_filters_pass(c, view, 4 + fr.dlc))
            {
                memset(out, 0, sizeof(*out));
                out->protocol_id = J2534_PROT_CAN;
                /* the frame's own RX-interrupt time, not the poll time */
                out->timestamp = fr.ts_us ? fr.ts_us : now_us();
                out->rx_status = fr.ext ? J2534_RX_CAN_29BIT_ID : 0;
                out->data_size = 4 + fr.dlc;
                memcpy(out->data, view, out->data_size);
                got = true;
            }
        }
    }
    else if (c->protocol == J2534_PROT_ISO15765 && c->isotp_bound)
    {
        size_t n = 0;
        esp_err_t r = can_isotp()->recv(c->isotp, out->data,
                                        sizeof(out->data), &n, 5);
        if (r == ESP_OK && n > 0)
        {
            out->protocol_id = J2534_PROT_ISO15765;
            out->rx_status = c->ext ? J2534_RX_CAN_29BIT_ID : 0;
            out->tx_flags = 0;
            out->timestamp = now_us();
            out->extra_data_index = 0;
            out->data_size = (uint32_t)n;
            got = true;
            obd_gate_release(c); /* the reply is in: conversation over */
        }
    }

    xSemaphoreGive(s_lock);
    return got;
}
