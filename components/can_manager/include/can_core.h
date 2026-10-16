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
 * @file can_core.h
 * @brief The shared CAN bus core: one TWAI peripheral, many clients.
 *
 * The CAN manager wraps the ESP-IDF TWAI (CAN) driver and provides:
 *  - Multi-client frame dispatch: multiple AT engine instances receive frames
 *    that pass their individual filter / mask settings.
 *  - Normal TX (single frame send).
 *  - Bus monitor mode (silent: receive only, no ACK).
 *  - CAN statistics (Tx, Rx, error counts).
 *
 * Only one can_core_handle_t may exist per physical CAN peripheral.
 * Multiple AT engines register can_core_client_t instances with the handle.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "elm327_err.h"
#include "can_core_recovery.h"
#include "can_autobaud_core.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#else
typedef void *QueueHandle_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Maximum number of callback clients (AT engines, ISO-TP channels)
 * sharing one CAN bus. Default matches the historical
 * ELM327_MAX_ENGINES sizing.
 * ------------------------------------------------------------------------- */
#ifndef CAN_CORE_MAX_CLIENTS
#define CAN_CORE_MAX_CLIENTS      4
#endif

/* Task-owned RX queues. 8 until 2026-10-03, which today's consumers fill
 * but for one slot (four J2534 channels, uds_manager's ISO-TP session, the
 * bridge endpoints' shared queue, the frame logger); J1939 is the eighth.
 * The RX task walks the table once per frame: 16 flag tests. */
#ifndef CAN_CORE_MAX_QUEUE_SUBSCRIBERS
#define CAN_CORE_MAX_QUEUE_SUBSCRIBERS 16
#endif

/* -------------------------------------------------------------------------
 * CAN frame structure (platform-agnostic)
 * ------------------------------------------------------------------------- */
typedef struct
{
    uint32_t  id;          /**< Frame ID (11 or 29 bit depending on ext).    */
    bool      ext;         /**< True = 29-bit extended ID.                   */
    bool      rtr;         /**< True = Remote Transmission Request frame.    */
    uint8_t   dlc;         /**< Data Length Code (0–8).                      */
    uint8_t   data[8];     /**< Frame payload.                               */
    uint32_t  ts_us;       /**< Receive time, microseconds since boot, taken
                                in the driver's RX interrupt (wraps every
                                ~71.6 min); 0 = unknown / a frame built for
                                transmission. 2026-09-21: the timestamp that
                                the binary BLE stream, GVRET and J2534 carry. */
} can_core_frame_t;

/* -------------------------------------------------------------------------
 * Frame receive callback
 *
 * Invoked (from the CAN manager's internal task) for each frame that passes
 * the client's filter / mask.
 *
 * @param ctx    Opaque context provided at client registration.
 * @param frame  Received CAN frame (valid for the duration of the callback).
 * ------------------------------------------------------------------------- */
typedef void (*can_core_rx_cb_t)(void *ctx,
                                   const can_core_frame_t *frame);

/* -------------------------------------------------------------------------
 * Client record: one per AT engine instance
 * ------------------------------------------------------------------------- */
typedef struct
{
    can_core_rx_cb_t   rx_cb;         /**< Frame receive callback.         */
    void                *rx_ctx;        /**< Opaque context for rx_cb.       */
    uint32_t             filter;        /**< ID filter value (masked).       */
    uint32_t             mask;          /**< ID mask (1 = bit must match).   */
    bool                 ext;           /**< True = match 29-bit IDs.        */
    bool                 monitor_all;   /**< True = ignore filter / mask.    */
    bool                 active;        /**< True if slot is in use.         */
} can_core_client_t;

/* -------------------------------------------------------------------------
 * Queue subscriber record: one per task-owned RX queue
 *
 * Queue item size must be sizeof(can_core_frame_t). When the queue is full,
 * the oldest frame is discarded so the newest frame can be enqueued.
 * ------------------------------------------------------------------------- */
typedef struct
{
    QueueHandle_t         queue;        /**< Queue of can_core_frame_t.    */
    uint32_t              filter;       /**< ID filter value (masked).       */
    uint32_t              mask;         /**< ID mask (1 = bit must match).   */
    bool                  ext;          /**< True = match 29-bit IDs.        */
    bool                  monitor_all;  /**< True = ignore filter / mask.    */
    bool                  active;       /**< True if slot is in use.         */
    const char           *name;         /**< Who it is, for the status views:
                                             a string that outlives the
                                             subscription, or NULL.          */
    uint32_t              drops;        /**< Frames this queue lost (full: the
                                             oldest one made room). Zeroed at
                                             registration.                   */
} can_core_queue_subscriber_t;

/* -------------------------------------------------------------------------
 * CAN bus handle configuration
 * ------------------------------------------------------------------------- */
typedef struct
{
    int      tx_gpio;        /**< GPIO pin for TWAI TX.                      */
    int      rx_gpio;        /**< GPIO pin for TWAI RX.                      */
    uint32_t baud_kbps;      /**< Bus speed in kbit/s (e.g. 500). Ignored
                                  when n_baud_candidates > 1.                */
    bool     silent_mode;    /**< Listen-only for good: the node never gets
                                  its TX pin (no ACK, no frame, no flag).    */
    size_t   rx_queue_depth; /**< TWAI RX queue depth (frames).              */
    size_t   tx_queue_depth; /**< TWAI TX queue depth (frames).              */
    uint16_t baud_candidates[CAN_AB_MAX_CANDIDATES]; /**< Auto: the bitrates
                                  to try, kbit/s, in order.                  */
    uint8_t  n_baud_candidates; /**< 0 or 1 = fixed (baud_kbps), more = auto. */
    uint16_t link_retry_ms;  /**< Auto: rest after a round without a readable
                                  bitrate; 0 = the policy's default.         */
    bool     quiet_link;     /**< The link's verdicts go to the debug log: an
                                  internal listener, not the user's bus.     */
} can_core_config_t;

/* -------------------------------------------------------------------------
 * The node's link to the bus (listen before talk, can_autobaud_core.h)
 *
 * The node always starts listen-only and without its TX pin. It is promoted
 * to normal mode once frames prove the bitrate (or a fixed-bitrate bus stayed
 * silent), and demoted again when the bus turns unreadable.
 * ------------------------------------------------------------------------- */
typedef struct
{
    const char *state;       /**< "detecting", "listening", "mismatch" or
                                  "running" (can_ab_state_name).             */
    uint32_t baud_kbps;      /**< The bitrate the node is at.                */
    uint32_t detected_kbps;  /**< Last bitrate proven by frames, 0 = none.   */
    bool     autobaud;       /**< More than one candidate.                   */
    bool     listen_only;    /**< The node cannot transmit right now.        */
    bool     verified;       /**< Frames were read at this bitrate.          */
    bool     adopted;        /**< Running at a bitrate a tester stated.      */
    uint32_t switches;       /**< Auto: candidate changes.                   */
    uint32_t demotions;      /**< Normal -> listen-only by the watchdog.     */
} can_core_link_t;

/* -------------------------------------------------------------------------
 * CAN bus state (mapped from the TWAI driver; RECOVERING also covers the
 * post-recovery backoff window before the automatic restart)
 * ------------------------------------------------------------------------- */
typedef enum
{
    CAN_CORE_BUS_STOPPED = 0,
    CAN_CORE_BUS_RUNNING,
    CAN_CORE_BUS_OFF,
    CAN_CORE_BUS_RECOVERING,
} can_core_bus_state_t;

/* -------------------------------------------------------------------------
 * CAN bus statistics
 * ------------------------------------------------------------------------- */
typedef struct
{
    uint32_t tx_count;       /**< Frames successfully transmitted.           */
    uint32_t rx_count;       /**< Frames received.                           */
    uint32_t tx_errors;      /**< Transmit error count.                      */
    uint32_t rx_errors;      /**< Receive error / overrun count.             */
    uint32_t arb_lost;       /**< Arbitration lost count.                    */
    uint32_t bus_errors;     /**< Bus error count.                           */
    uint32_t bus_off_count;  /**< Times the controller entered bus-off.      */
    uint32_t recovery_count; /**< Successful automatic bus restarts.         */
    uint32_t rx_missed;      /**< Wire frames lost to a full TWAI RX queue.  */
    uint32_t tx_retries;     /**< Transmissions repeated after a lost one (the
                                  controller is single shot; the driver's
                                  tx_done interrupt re-queues a frame up to
                                  CAN_DRV_TX_RETRIES times, 2026-10-03).      */
    uint32_t tx_lost;        /**< Frames given up on after those retries.    */
    uint32_t tx_done;        /**< Frames the controller reported sent.       */
    uint32_t rx_overrun;     /**< Wire frames the controller's own FIFO lost
                                  (64 bytes: the receive interrupt was served
                                  too late). Before 2026-10-03 these vanished
                                  without a trace.                            */
    uint32_t dispatch_drops; /**< Drop-oldest evictions in subscriber queues.*/
    uint32_t rx_bad;         /**< Errors seen while RECEIVING (stuff / form /
                                  CRC; every error while listen-only): what
                                  a wrong bitrate produces, without a frame. */
    uint32_t tx_refused;     /**< Transmits asked while the node may not talk
                                  (listening, mismatch, being reconfigured). */
    uint32_t rx_storms;      /**< Receive-error storms (>= 20 000 errors/s, a
                                  bitrate close to the bus's) that masked the
                                  controller's interrupts until the next node
                                  start: an interrupt watchdog reset before
                                  2026-10-03                                  */
    uint32_t rx_deaf;        /**< Looks at the RX line that found the bus busy
                                  while the listening controller reported
                                  neither a frame nor an error: traffic it
                                  cannot even fail on (a saturated bus at a
                                  higher bitrate).                            */
    uint32_t err_stuff;      /**< bus_errors by kind, as the controller names
                                  them. A receiver's errors are stuff, form
                                  and other (CRC); bit and ack belong to our
                                  own transmissions.                          */
    uint32_t err_form;
    uint32_t err_bit;
    uint32_t err_ack;
    uint32_t err_other;
    uint8_t  bus_state;      /**< can_core_bus_state_t snapshot.           */
} can_core_stats_t;

/* -------------------------------------------------------------------------
 * CAN bus handle (one per physical CAN peripheral)
 * ------------------------------------------------------------------------- */
typedef struct can_core_handle_s
{
    can_core_config_t  config;                          /**< Init config.  */
    can_core_client_t  clients[CAN_CORE_MAX_CLIENTS]; /**< Client list.  */
    can_core_queue_subscriber_t queue_subscribers[CAN_CORE_MAX_QUEUE_SUBSCRIBERS]; /**< Task RX queues. */
    can_core_stats_t   stats;                           /**< Statistics.   */
    can_core_recovery_t recovery;      /**< Bus-off restart policy state.  */
    can_ab_t             ab;            /**< Listen-before-talk policy state. */
    volatile uint32_t    link_frames;   /**< Frames received: the policy's
                                             evidence. Never zeroed (stats can
                                             be, from the CLI). Its receive
                                             errors come straight from the
                                             driver's ISR counters.            */
    uint32_t             isr_base_rx_overrun; /**< (see isr_base_rx_missed)  */
    uint32_t             isr_base_rx_missed; /**< ISR counters at the last   */
    uint32_t             isr_base_rx_bad;    /**< stats reset: stats report  */
    uint32_t             isr_base_arb_lost;  /**< the difference.            */
    uint32_t             isr_base_err[5];    /**< Same, the errors by kind.  */
    uint32_t             isr_base_tx[3];     /**< Same: retries, lost, done. */
    uint32_t             isr_base_storms;    /**< Same: the storms.          */
    uint32_t             deaf;          /**< Evidence the controller did not
                                             give: the RX line was busy while
                                             it reported neither a frame nor
                                             an error (can_core_link.c). Added
                                             to its receive errors for the
                                             policy. Never zeroed.             */
    uint32_t             line_ms;       /**< When the RX line was last looked
                                             at, and the counters then.        */
    uint32_t             line_frames;
    uint32_t             line_bad;
    uint32_t             node_baud_kbps; /**< What the node runs at right now. */
    volatile bool        node_listen;   /**< Node is listen-only (no TX pin).  */
    volatile bool        tx_open;       /**< Transmit allowed: normal mode, up. */
    volatile int         tx_users;      /**< Transmits in flight (the gate).   */
    volatile bool        tx_refused_said; /**< The refusal was logged once this
                                               listen-only episode.          */
    volatile bool        initialised;                     /**< Init flag.    */
    volatile bool        reconfiguring;                   /**< Driver reconfigure in progress. */
    volatile bool        rx_parked;   /**< RX task ack: parked outside twai_receive. */
    volatile bool        rx_exited;   /**< RX task ack: loop left, about to delete.  */
    void                *rx_task_handle; /**< Internal RX task handle.       */
} can_core_handle_t;

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/**
 * @brief Initialise the CAN bus and start the receive task.
 *
 * @param handle  Pre-allocated (may be static) handle.
 * @param config  Bus configuration.
 * @return ELM327_OK on success, ELM327_ERR_CAN on driver error.
 */
elm327_err_t can_core_init(can_core_handle_t *handle,
                              const can_core_config_t *config);

/**
 * @brief Deinitialise the CAN bus and stop the receive task.
 *
 * @param handle  CAN handle.
 */
void can_core_deinit(can_core_handle_t *handle);

/**
 * @brief Take the controller off the bus at once, without teardown.
 *
 * For the restart path: nothing is waited for, the RX task and every
 * registration stay as they are. The controller neither receives nor
 * transmits afterwards; only a deinit + init brings it back.
 *
 * @param handle  CAN handle.
 */
void can_core_quiesce(can_core_handle_t *handle);

/**
 * @brief Register an AT engine as a CAN client.
 *
 * Returns a client slot index (0 to CAN_CORE_MAX_CLIENTS-1).
 * The client immediately begins receiving matching frames via its callback.
 *
 * @param handle  CAN handle.
 * @param client  Populated client descriptor.
 * @return Client index (>=0) on success, ELM327_ERR_NO_MEM if full.
 */
int can_core_register_client(can_core_handle_t *handle,
                                const can_core_client_t *client);

/**
 * @brief Register a task-owned FreeRTOS queue for CAN RX delivery.
 *
 * Queue item size must be sizeof(can_core_frame_t). If the queue is full,
 * the oldest item is removed so the newest frame is retained.
 *
 * @param handle      CAN handle.
 * @param subscriber  Populated queue subscriber descriptor.
 * @return Subscriber index (>=0) on success, ELM327_ERR_NO_MEM if full.
 */
int can_core_register_rx_queue(can_core_handle_t *handle,
                                 const can_core_queue_subscriber_t *subscriber);

/** Name a queue subscriber for the status views. @p name must outlive the
 *  subscription (a string literal). */
void can_core_set_rx_queue_name(can_core_handle_t *handle, int idx,
                                const char *name);

/** Queue subscriber slots in use. */
int can_core_rx_queue_count(const can_core_handle_t *handle);

/** A copy of the subscriber record in slot @p idx; false when the slot is
 *  free or out of range. */
bool can_core_get_rx_queue(const can_core_handle_t *handle, int idx,
                           can_core_queue_subscriber_t *out);

/**
 * @brief Unregister a CAN client.
 *
 * @param handle  CAN handle.
 * @param idx     Client index returned by can_core_register_client().
 */
void can_core_unregister_client(can_core_handle_t *handle, int idx);

/**
 * @brief Unregister a task-owned CAN RX queue subscriber.
 *
 * @param handle  CAN handle.
 * @param idx     Queue subscriber index.
 */
void can_core_unregister_rx_queue(can_core_handle_t *handle, int idx);

/**
 * @brief Update a client's filter and mask settings.
 *
 * @param handle  CAN handle.
 * @param idx     Client index.
 * @param filter  New filter value.
 * @param mask    New mask value.
 * @param ext     True for 29-bit IDs.
 */
void can_core_set_filter(can_core_handle_t *handle,
                            int idx,
                            uint32_t filter,
                            uint32_t mask,
                            bool ext);

/**
 * @brief Update a queue subscriber's filter and mask settings.
 *
 * @param handle  CAN handle.
 * @param idx     Queue subscriber index.
 * @param filter  New filter value.
 * @param mask    New mask value.
 * @param ext     True for 29-bit IDs.
 */
void can_core_set_rx_queue_filter(can_core_handle_t *handle,
                                    int idx,
                                    uint32_t filter,
                                    uint32_t mask,
                                    bool ext);

/**
 * @brief Set or clear the monitor-all flag for a client.
 *
 * When true the client's filter / mask is ignored and every received frame
 * is delivered to the callback.
 *
 * @param handle       CAN handle.
 * @param idx          Client index.
 * @param monitor_all  New value.
 */
void can_core_set_monitor_all(can_core_handle_t *handle,
                                 int idx,
                                 bool monitor_all);

/**
 * @brief Set or clear the monitor-all flag for a queue subscriber.
 *
 * @param handle       CAN handle.
 * @param idx          Queue subscriber index.
 * @param monitor_all  New value.
 */
void can_core_set_rx_queue_monitor_all(can_core_handle_t *handle,
                                         int idx,
                                         bool monitor_all);

/**
 * @brief Transmit a single CAN frame from any caller task.
 *
 * @param handle   CAN handle.
 * @param frame    Frame to transmit.
 * @param timeout_ms  Timeout in milliseconds (0 = non-blocking).
 * @return ELM327_OK on success, ELM327_ERR_BUSY if the TX queue stays full
 *         until timeout or the node may not talk within it (listen-only,
 *         bitrate not proven, mismatch: counted in tx_refused), or
 *         ELM327_ERR_CAN on driver error.
 */
elm327_err_t can_core_transmit(can_core_handle_t *handle,
                                   const can_core_frame_t *frame,
                                   uint32_t timeout_ms);

/**
 * @brief Read the current CAN bus statistics.
 *
 * @param handle  CAN handle.
 * @param stats   Output structure.
 */
void can_core_get_stats(can_core_handle_t *handle,
                           can_core_stats_t *stats);

/**
 * @brief Reset (clear) CAN bus statistics.
 *
 * @param handle  CAN handle.
 */
void can_core_reset_stats(can_core_handle_t *handle);

/**
 * @brief Read the node's link to the bus: policy state, bitrate, mode.
 *
 * @param handle  CAN handle.
 * @param out     Output structure.
 */
void can_core_get_link(can_core_handle_t *handle, can_core_link_t *out);

/**
 * @brief Change CAN silent/listen-only mode without losing client registrations.
 *
 * To listen-only: at once. To normal: at once when the bitrate in use is
 * proven or the node already reached its verdict; otherwise the node keeps
 * listening and the listen-before-talk policy promotes it when it may talk
 * (frames at this bitrate, or a fixed-bitrate bus that stays silent).
 *
 * @param handle       CAN handle.
 * @param silent_mode  True for listen-only mode, false for normal mode.
 * @return ELM327_OK on success.
 */
elm327_err_t can_core_set_silent_mode(can_core_handle_t *handle,
                                        bool silent_mode);

/**
 * @brief Change the bus speed (requires reinitialisation).
 *
 * Stops the bus and restarts it at the new, FIXED rate, listen-only: the
 * listen-before-talk policy starts over and promotes the node when it may
 * talk. A transmit before that is refused (ELM327_ERR_BUSY).
 *
 * @param handle     CAN handle.
 * @param baud_kbps  New baud rate in kbit/s.
 * @return ELM327_OK on success.
 */
elm327_err_t can_core_set_baud(can_core_handle_t *handle,
                                  uint32_t baud_kbps);

/**
 * @brief A tester states the bus bitrate (a J2534 CONNECT, slcan's S).
 *
 * Taken only when the link policy has no verdict of its own: more than one
 * candidate (`auto`) with nothing proven, the bus silent for
 * CAN_AB_SILENT_MS at the candidate in use, never bitten
 * (can_ab_adopt). The node then runs at that bitrate in the configured
 * mode, `adopted` and unverified; the watchdog demotes it on the first
 * unreadable traffic. A fixed bitrate is never overruled.
 *
 * @param[out] adopted  True when the statement was taken (may be NULL).
 * @return ELM327_OK (look at @p adopted), ELM327_ERR_NOT_INIT, or the
 *         node restart's error.
 */
elm327_err_t can_core_adopt_baud(can_core_handle_t *handle,
                                 uint32_t baud_kbps, bool *adopted);

#ifdef __cplusplus
}
#endif
