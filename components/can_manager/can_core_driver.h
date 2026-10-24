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
 * @file can_core_driver.h
 * @brief The TWAI node driver half of can_core (can_core_driver.c): node
 *        bring-up / teardown and the ISR-fed raw RX queue. Private to
 *        can_core.c, which owns the RX task, dispatch, TX and stats.
 *        Split out 2026-10-02 (700-line rule).
 */
#pragma once

#ifdef ESP_PLATFORM

#include <stdbool.h>
#include <stdint.h>

#include "esp_twai.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "can_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAN_RX_RAW_EXT     0x1
#define CAN_RX_RAW_RTR     0x2
#define CAN_RX_RAW_EVT     0x80 /* not a frame: wake the RX task (the link
                                   policy has receive errors to look at)  */

typedef struct
{
    uint32_t id;
    uint32_t ts_us;     /* esp_timer at the RX interrupt (the bus time) */
    uint8_t  flags;
    uint8_t  dlc;
    uint8_t  data[8];
} can_rx_raw_t;

/** What the ISR counts, since boot (internal RAM, never zeroed: the core
 *  keeps the offsets for its own stats). */
typedef struct
{
    uint32_t rx_missed; /* frames lost to a full raw queue                  */
    uint32_t rx_overrun; /* frames the controller's own FIFO lost: it was
                            full when they arrived (the interrupt came late) */
    uint32_t rx_bad;    /* receive errors (every error while listen-only)   */
    uint32_t arb_lost;  /* arbitrations lost                                */
    uint32_t err_stuff; /* bus errors by kind, as the controller names them */
    uint32_t err_form;
    uint32_t err_bit;
    uint32_t err_ack;
    uint32_t err_other; /* CRC and whatever else the capture calls "other"  */
    uint32_t tx_retries; /* transmissions repeated after a lost one (the
                            controller is single shot; the tx_done interrupt
                            re-queues the frame: CAN_DRV_TX_RETRIES_ARB times
                            after a lost arbitration, CAN_DRV_TX_RETRIES after
                            an error)                                        */
    uint32_t tx_lost;   /* frames given up on after the retries              */
    uint32_t tx_done;   /* frames the controller reported sent               */
    uint32_t rx_storms; /* receive-error storms that masked the controller's
                           interrupts (can_on_error); each ends with a bounce */
} can_drv_counters_t;

void can_drv_counters(can_drv_counters_t *out);

/* Driver state, written by can_core_driver.c only. The queue is STATIC
 * and never deleted (see can_core_driver.c). */
extern twai_node_handle_t can_drv_node;      /* NULL while the node is down */
extern QueueHandle_t      can_drv_rx_q;      /* can_rx_raw_t, ISR -> RX task */
extern volatile bool      can_drv_evt_bus_off;
/* a receive-error storm masked the controller's interrupts (RX task: log it;
   the link policy bounces the deaf node) */
extern volatile bool can_drv_evt_storm;
/** True while a storm has the controller's interrupts masked (until the
 *  next node start). */
bool can_drv_storm_masked(void);
extern volatile bool      can_drv_evt_recovered;

/** Create + enable the node at @p baud_kbps, on the configured ISR core.
 *  @p listen_only: no TX pin is routed (the node cannot drive the bus).
 *  Pins and queue depth come from handle->config; the raw RX queue keeps
 *  its frames. Holds the node lock (can_core_node.c). */
elm327_err_t can_drv_start_on_core(can_core_handle_t *handle,
                                   uint32_t baud_kbps, bool listen_only);

/** The creation itself, on the calling task's core (can_core_driver.c).
 *  Only can_drv_start_on_core() calls it, with the node lock held. */
elm327_err_t can_drv_create(can_core_handle_t *handle, uint32_t baud_kbps,
                            bool listen_only);

/** The node's status and error record, read under the node lock: false
 *  while the node is down or being bounced. THE way to read them from a
 *  task that is not bouncing the node: twai_node_get_info() on a node that
 *  is being deleted loads through a NULL register pointer (the panic of
 *  2026-10-05, can_core_node.c). May wait for a bounce (milliseconds). */
bool can_drv_node_info(twai_node_status_t *info, twai_node_record_t *rec);

/** Create (once) and empty the raw RX queue: at init, never on a bounce. */
void can_drv_rx_reset(void);

/** Disable + delete the node (no-op while it is down), under the node
 *  lock. The caller parks the RX task first. */
void can_drv_stop(can_core_handle_t *handle);

/** Hold the TX pad recessive as a plain GPIO (output, high): what the pad
 *  is whenever the controller's TX signal is not routed to it (node down,
 *  or listen-only mode). */
void can_drv_tx_recessive(int tx_gpio);

/** Queue @p frame for transmission from a slot the driver owns until the
 *  frame's tx_done interrupt (the IDF node driver keeps the caller's
 *  POINTER until the hardware is free: a frame on the caller's stack was a
 *  use-after-return whenever the hardware was busy, 2026-10-03). A frame
 *  the single-shot controller loses is re-queued from the interrupt:
 *  CAN_DRV_TX_RETRIES_ARB times after a lost arbitration (no error, a busy
 *  bus), CAN_DRV_TX_RETRIES times after an error (no acknowledge: each one
 *  costs the node 8 error-counter points).
 *  @return ELM327_OK; ELM327_ERR_BUSY: no slot free or the driver's queue
 *  full within @p timeout_ms; ELM327_ERR_CAN: the driver refused. */
elm327_err_t can_drv_transmit(const can_core_frame_t *frame,
                              uint32_t timeout_ms);

#define CAN_DRV_TX_SLOTS       16 /* frames in flight (internal RAM, the ISR
                                     and the HAL read them)               */
#define CAN_DRV_TX_RETRIES     3  /* attempts after the first, on an error  */
#define CAN_DRV_TX_RETRIES_ARB 250 /* ... after a lost arbitration, which
                                      costs the bus nothing. A car bus sends
                                      in bursts, and a low-priority frame
                                      loses to every frame in one: 16 was
                                      too few on M-CAN. 250 is ~60 ms of
                                      back-to-back frames at 500 kbit/s    */

/** Take the controller off the bus NOW, without tearing the node down
 *  (no-op while it is down): for the restart path, where the RX task is
 *  not parked and nothing may block (a bounce in flight is given 50 ms,
 *  then the restart goes on). */
void can_drv_quiesce(void);

#ifdef __cplusplus
}
#endif

#endif /* ESP_PLATFORM */
