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
 * @file j2534_channel.h
 * @brief J2534 channel transport bindings: a CAN channel onto
 *        can_manager (raw frames), an ISO15765 channel onto the
 *        registered ISO-TP provider (can_isotp.h; without one,
 *        ISO15765 connect fails with a log line). Owned by
 *        j2534_server; the server pumps RX and pushes RX_MSG frames.
 *
 * Message data conventions (this wire protocol, TASK §4):
 *   CAN       msg.data = [4-byte big-endian CAN id][0..8 frame bytes]
 *   ISO15765  msg.data = the UDS payload (the CAN id is the channel's
 *             tx/rx id set at CONNECT or by a FLOW_CONTROL filter)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "j2534_proto.h"

#define J2534_CH_MAX_FILTERS 8

void j2534_channel_init(void);          /* one-time: mutex etc. */
void j2534_channel_reset_all(void);     /* tester disconnected: tear down */

/**
 * ECU-flashing safety gate. When @p allow is false (the default), a write
 * carrying a UDS memory-transfer service (RequestDownload/Upload,
 * TransferData, RequestTransferExit) is rejected, reprogramming is
 * impossible without them. Set from the j2534_server `allow_reflash`
 * setting. Applies to ISO15765 and to raw-CAN frames that carry an
 * ISO-TP single/first frame with one of those SIDs.
 */
void j2534_channel_set_allow_reflash(bool allow);

/**
 * The bus bitrate at CONNECT. The bus runs at the device's setting, at
 * what the listen-before-talk policy read off the wire, or at a bitrate a
 * tester stated; a CAN/ISO15765 CONNECT at another @p baud_bps is answered
 * ERR_INVALID_BAUDRATE. With `can_manager.baud = auto` and nothing proven
 * yet the tester's rate is stated to the node (can_manager_adopt_baud,
 * 2026-10-17): taken when the bus has been silent, otherwise the channel
 * still opens and its writes are refused with a log line. Returns a J2534
 * status code (NOERROR = go on and open the channel).
 */
uint32_t j2534_channel_bus_baud(uint32_t proto, uint32_t baud_bps);

/**
 * Bind a channel slot to a protocol. For ISO15765, @p tx_id/@p rx_id set
 * the ISO-TP addressing (0 = defer to a FLOW_CONTROL filter). Returns a
 * J2534 status code.
 */
uint32_t j2534_channel_connect(int slot, uint32_t protocol, uint32_t flags,
                               uint32_t tx_id, uint32_t rx_id, bool ext);

void j2534_channel_disconnect(int slot);
bool j2534_channel_active(int slot);

/** Send one PASSTHRU_MSG on the channel. J2534 status code. */
uint32_t j2534_channel_write(int slot, const j2534_msg_t *msg);

/**
 * Add a filter. For CAN: PASS/BLOCK matched in the RX pump. For
 * ISO15765: FLOW_CONTROL sets/rebinds the ISO-TP tx/rx ids (pattern =
 * rx id, flow-control msg = tx id). Returns status; *filter_id set on OK.
 */
uint32_t j2534_channel_add_filter(int slot, uint32_t type,
                                  const j2534_msg_t *mask,
                                  const j2534_msg_t *pattern,
                                  const j2534_msg_t *flow_control,
                                  uint32_t *filter_id);

uint32_t j2534_channel_stop_filter(int slot, uint32_t filter_id);
void j2534_channel_clear_filters(int slot);

/**
 * Poll one channel for a received message (non-blocking-ish; ISO15765
 * uses a short internal timeout). Returns true and fills @p out when a
 * message passed the channel's filters. The server's RX pump calls this
 * across all active channels and pushes RX_MSG frames.
 */
bool j2534_channel_poll_rx(int slot, j2534_msg_t *out);
