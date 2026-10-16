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
 * @file can_autobaud_core.h
 * @brief Listen before talk: the bitrate and mode policy of the native CAN
 *        node (pure logic, host-testable).
 *
 * A node at the wrong bitrate destroys the traffic of the bus it sits on
 * (bench 2026-10-02: the sending ECU driven to bus-off again and again). So
 * the node ALWAYS starts listen-only, which the driver makes physically
 * unable to transmit (TX pin not routed), and this policy decides what
 * happens next from two counters:
 *
 *   frames  frames received (CRC-checked: they prove the bitrate)
 *   bad     receive errors (stuff / form / CRC seen while listening): what
 *           a wrong bitrate produces, thousands per second, with no frame
 *
 *   frames arrive                     -> the bitrate is proven; a node the
 *                                        settings want talking is promoted
 *                                        to normal mode
 *   errors and no frame               -> wrong bitrate: next candidate
 *                                        (auto) or MISMATCH (fixed), still
 *                                        listen-only, still harmless
 *   nothing at all for a while        -> silent bus (a gatewayed OBD port):
 *                                        a FIXED bitrate goes to its
 *                                        configured mode, as before; auto
 *                                        keeps listening (a silent bus has
 *                                        no bitrate)
 *   running in normal mode, then
 *   errors and no frame               -> demoted to listen-only at once (the
 *                                        bus woke up at another bitrate)
 *
 * "Errors and no frame" is taken at once from a node that talks (every
 * error there is a frame it destroyed). A node that listens waits for a
 * second helping CAN_AB_CONFIRM_MS later: it can afford to, and a single
 * corrupted frame must not read as a wrong bitrate.
 *
 * After a mismatch was seen once, silence no longer promotes: only frames do.
 * All times are uint32 milliseconds; comparisons are wrap-safe. The caller
 * steps the policy from one task and applies the returned action.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAN_AB_MAX_CANDIDATES 8

#define CAN_AB_OK_FRAMES   2u    /**< Frames that prove a bitrate.              */
#define CAN_AB_BAD_MIN     8u    /**< Receive errors, with no frame, that name
                                      a mismatch (measured: 1200 to 11000/s).
                                      Each one is a frame destroyed on the bus
                                      while the node talks, so the number is
                                      what the watchdog costs a sender: 16 took
                                      18 failed transmissions on the bench.     */
#define CAN_AB_SPAN_MS     1000u /**< Errors must come within one span: a rare
                                      glitch on a quiet bus never adds up.      */
#define CAN_AB_SILENT_MS   300u  /**< No frame and no error for this long: the
                                      bus is silent.                            */
#define CAN_AB_WATCH_MS    100u  /**< Watchdog window of a running node: the
                                      errors and the missing frame must fall
                                      into one window.                          */
#define CAN_AB_RETRY_MS    2000u /**< Auto: rest after a round in which no
                                      candidate read the bus, then again.       */
#define CAN_AB_CONFIRM_MS  10u   /**< Listening: a mismatch needs two helpings
                                      of errors this far apart, with no frame
                                      between them. One corrupted frame makes a
                                      listening (error-passive) controller
                                      report a cascade (bench: 41 to 48 errors
                                      for one frame); it is over within a
                                      frame time, 4 ms at 33 kbit/s.            */

typedef enum
{
    CAN_AB_LISTENING = 0, /**< Listen-only, no verdict yet.                     */
    CAN_AB_MISMATCH,      /**< The bus carries traffic that the fixed bitrate
                               cannot read (auto: that no candidate reads).
                               Listen-only.                                     */
    CAN_AB_RUNNING,       /**< Verdict reached: the configured mode applies.    */
} can_ab_state_t;

typedef struct
{
    uint16_t candidates[CAN_AB_MAX_CANDIDATES]; /**< kbit/s, in try order.      */
    uint8_t  n_candidates; /**< 1 = fixed bitrate, more = auto.                 */
    bool     want_normal;  /**< The settings ask for a node that may transmit.  */
    uint16_t retry_ms;     /**< Auto: rest after a round in which no candidate
                                read the bus; 0 = CAN_AB_RETRY_MS.              */
} can_ab_cfg_t;

/** What the caller has to do to the node (nothing when !apply). */
typedef struct
{
    bool     apply;
    uint16_t baud_kbps;
    bool     listen_only;
} can_ab_action_t;

typedef struct
{
    can_ab_cfg_t   cfg;
    can_ab_state_t state;
    uint8_t  idx;           /**< Candidate in use.                              */
    bool     normal;        /**< The node may transmit right now.               */
    bool     verified;      /**< Frames were read at the bitrate in use.        */
    bool     bitten;        /**< A mismatch was seen: silence promotes no more. */
    bool     adopted;       /**< Running at a bitrate a tester stated.          */
    uint8_t  misses;        /**< Auto: candidates that failed in this round.    */
    uint16_t detected_kbps; /**< Last bitrate proven by frames, 0 = none.       */
    uint32_t enter_ms;      /**< When the bitrate / mode in use was entered.    */
    uint32_t frames0;       /**< Frame counter at that moment.                  */
    uint32_t span_ms;       /**< Start of the current error span / window.      */
    uint32_t span_frames0;  /**< Counters at the start of the span.             */
    uint32_t span_bad0;
    bool     suspect;       /**< Listening: one helping of errors was seen.     */
    uint32_t suspect_ms;    /**< ... at this time.                              */
    uint32_t switches;      /**< Candidate changes.                             */
    uint32_t demotions;     /**< Normal -> listen-only by the watchdog.         */
} can_ab_t;

/**
 * @brief Start the policy. @p frames / @p bad are the counters right now.
 * @return The node's first configuration: candidate 0, listen-only. With an
 *         empty or NULL configuration: 500 kbit/s, listen-only, for ever.
 */
can_ab_action_t can_ab_init(can_ab_t *ab, const can_ab_cfg_t *cfg,
                            uint32_t now_ms, uint32_t frames, uint32_t bad);

/**
 * @brief Evaluate. Call at least every CAN_AB_WATCH_MS, and at once when
 *        CAN_AB_BAD_MIN more receive errors came in (the glue's error
 *        interrupt wakes the stepping task for that).
 * @return What to do to the node; after applying it call can_ab_mark().
 */
can_ab_action_t can_ab_step(can_ab_t *ab, uint32_t now_ms, uint32_t frames,
                            uint32_t bad);

/**
 * @brief The settings wish changed at run time (a tool asks for listen-only,
 *        or for a node that may talk again).
 *
 * To listen-only: at once. To normal: at once when the verdict is in
 * (CAN_AB_RUNNING), otherwise the policy promotes the node when it may talk.
 * @return What to do to the node; after applying it call can_ab_mark().
 */
can_ab_action_t can_ab_want_normal(can_ab_t *ab, bool want_normal,
                                   uint32_t now_ms, uint32_t frames,
                                   uint32_t bad);

/**
 * @brief A tester states the bus bitrate (a J2534 CONNECT, slcan's S
 *        command). Taken only when the policy has no verdict of its own:
 *        auto with nothing proven, the bus silent (no frame and no error
 *        for CAN_AB_SILENT_MS at the candidate in use) and never bitten.
 *        The node then runs at that bitrate in the configured mode,
 *        unverified, like a fixed bitrate on a silent bus; the watchdog
 *        demotes it on the first unreadable traffic and the walk goes on.
 *        A fixed bitrate is the settings' word and is not overruled.
 * @param kbps       Must be one of the candidates.
 * @param[out] taken True when the statement was taken (may be NULL).
 * @return What to do to the node; after applying it call can_ab_mark().
 */
can_ab_action_t can_ab_adopt(can_ab_t *ab, uint16_t kbps, uint32_t now_ms,
                             uint32_t frames, uint32_t bad, bool *taken);

/** The node was reconfigured: evidence is counted from here. */
void can_ab_mark(can_ab_t *ab, uint32_t now_ms, uint32_t frames,
                 uint32_t bad);

/** The bitrate in use (kbit/s). */
uint16_t can_ab_baud(const can_ab_t *ab);

/** "detecting" (auto, no bitrate proven yet), "listening" (fixed, no
 *  verdict yet), "mismatch" or "running". Never NULL. */
const char *can_ab_state_name(const can_ab_t *ab);

#ifdef __cplusplus
}
#endif
