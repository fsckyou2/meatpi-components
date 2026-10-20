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
 * @file autopid_dialect.h
 * @brief The OBD dialect part of autopid_private.h (pure, host-tested:
 *        autopid_dialect.c). A FRAGMENT: included by autopid_private.h
 *        only, after autopid_vehicle.h (it needs ap_dialect_t,
 *        ap_veh_ecu_t and ap_bus_t).
 *
 * The legislated diagnostics of a vehicle come in dialects: the same data
 * behind other requests.
 *
 *   obd2   SAE J1979 / ISO 15031-5, services 01..0A: `0100` support
 *          bitmaps, `01xx` PIDs, the VIN by `0902`.
 *   uds    ISO 27145 (WWH-OBD) and SAE J1979-2 (OBD on UDS): the same PIDs
 *          as data identifiers F4xx of service 22 (`22F40C` answers
 *          `62 F4 0C A B`), the VIN by `22F802`, eight bitmap ranges. Such
 *          a vehicle does not answer `0100` at all.
 *   j1939  SAE J1939: broadcast data, no request rows (TASK_j1939_wwh.md
 *          phase 5).
 *
 * Which dialect a car speaks is found by the detection job and kept per
 * car in the vehicle store.
 */
#pragma once

#ifndef AP_NAME_LEN
#error "include autopid_private.h, not this fragment"
#endif

#define AP_DIALECT_RANGES_MAX 8     /* support bitmaps 00, 20, .. E0        */
#define AP_DIALECT_CAND_LEN   5     /* up to four protocol chars + NUL      */
#define AP_DIALECT_HDR_CMD_LEN 16   /* "ATSH18DA58F1" + NUL                 */

/** "obd2" / "uds" / "j1939". */
const char *ap_dialect_name(ap_dialect_t dialect);

/** The dialect of a stored name. Anything else, "" and NULL are obd2: a
 *  store written before dialects existed holds OBD-II cars only. */
ap_dialect_t ap_dialect_from_name(const char *name);

/** Standard PIDs are asked for one request at a time (obd2, uds). */
bool ap_dialect_has_requests(ap_dialect_t dialect);

/** The request for standard PID @p pid: "010C" / "22F40C".
 *  @return its length, 0 = the dialect has none or @p cap is too small. */
size_t ap_dialect_pid_cmd(ap_dialect_t dialect, uint8_t pid, char *out,
                          size_t cap);

/** The VIN request: "0902" / "22F802" ("" when the dialect has none). */
const char *ap_dialect_vin_cmd(ap_dialect_t dialect);

/** Support-bitmap ranges a scan walks: 6 (as before dialects) / 8 / 0. */
int ap_dialect_ranges(ap_dialect_t dialect);

/** How many bytes further into the payload the data of a PID sits than
 *  in obd2 (`41 0C A B` against `62 F4 0C A B`): 0 / 1. Added to the byte
 *  indexes of the standard table's expressions. */
int ap_dialect_data_shift(ap_dialect_t dialect);

/**
 * Responders and their support bitmaps from the reply to the bitmap
 * request of range @p base (0x00, 0x20, ..), as the chip prints it:
 *
 *   41 00 BE 3F A8 13                      headers off: one entry with id
 *                                          UINT32_MAX, every line OR-merged
 *   7E8 06 41 00 BE 3F A8 13               11-bit, headers on
 *   18 DA F1 58 07 62 F4 00 98 18 A0 13    29-bit, headers on: the chip
 *                                          prints the id as four byte tokens
 *   18DAF158 07 62 F4 00 98 18 A0 13       29-bit, the id in one token
 *
 * One entry per responder id in order of appearance (an id seen twice is
 * OR-merged). Lines of anything else (NO DATA, SEARCHING..., `7F 22 31`,
 * the answer to another range) are skipped.
 * @return entry count, at most @p max.
 */
int ap_dialect_bitmaps(ap_dialect_t dialect, const char *resp, uint8_t base,
                       ap_veh_ecu_t *out, size_t max);

/** The VIN from the reply to ap_dialect_vin_cmd(), any shape
 *  ap_resp_to_payload() reads. */
bool ap_dialect_vin(ap_dialect_t dialect, const char *resp,
                    char vin[AP_VIN_LEN]);

/** `62 F8 10 <id>`: the protocol identification of ISO 27145-4, from any
 *  line of @p resp. Informational: detection never depends on it. */
bool ap_dialect_uds_protocol_id(const char *resp, uint8_t *id);

/* ---- one responder's support bitmaps, every range (a scan's table) -------- */

typedef struct
{
    uint32_t id;                                /* responder CAN id          */
    uint32_t bitmap[AP_DIALECT_RANGES_MAX];     /* MSB = PID base + 1        */
} ap_dialect_ecu_t;

/** Fold the responders of one range (ap_dialect_bitmaps) into the table.
 *  @return the new row count (rows beyond @p max are dropped). */
int ap_dialect_table_add(ap_dialect_ecu_t *tab, int n, int max, int range,
                         const ap_veh_ecu_t *row, int n_row);

/** Does any responder announce the range after @p range? */
bool ap_dialect_table_more(const ap_dialect_ecu_t *tab, int n, int range);

/** Does anybody have @p pid? (never true for the bitmap PIDs 00, 20, ..) */
bool ap_dialect_table_has(const ap_dialect_ecu_t *tab, int n, uint8_t pid);

/** Which responder serves @p pid: the LOWEST id that has it. A second unit
 *  that repeats a PID in its own scaling loses (the Sprinter VS30's `5A`
 *  answers the engine speed unscaled; `58` is the engine).
 *  @return the id, UINT32_MAX when nobody has it. */
uint32_t ap_dialect_pid_owner(const ap_dialect_ecu_t *tab, int n,
                              uint8_t pid);

/* ---- addressing ---------------------------------------------------------- */

/** The physical request id of a legislated responder: 7E8..7EF ->
 *  7E0..7E7, 18DAF1xx -> 18DAxxF1. False for any other id. */
bool ap_dialect_request_id(uint32_t responder, uint32_t *request);

/** The chip command that addresses @p responder alone: "ATSH7E0" /
 *  "ATSH18DA58F1". @return its length, 0 = not a legislated responder. */
size_t ap_dialect_header_cmd(uint32_t responder, char *out, size_t cap);

/** Does a ';'-separated init chain set the request header (ATSH)?
 *  Case-insensitive, spaces ignored ("at sh 7e0"): the AP_SETS_HEADER bit
 *  of ap_chain_sets() (autopid_chain.c). */
bool ap_dialect_init_sets_header(const char *init);

/**
 * The ISO 15765-4 protocols a contact probe may be sent on, as a string of
 * protocol chars in the order a tester tries them (500 kbit/s before 250,
 * 11-bit before 29-bit). The probes are pinned requests (`0100`, then
 * `22F400`, on each): the chip's own search asks `0100` only, so it never
 * finds a UDS-dialect vehicle, and it needs 9.3 s to give up where a
 * pinned probe needs 0.6 (bench 2026-10-03). A pinned request at the wrong
 * bitrate destroys the traffic of a live bus, so the bus guard strikes
 * what this bus cannot be:
 *
 *   bus live at 500 kbit/s    "67"
 *   bus live at 250 kbit/s    "89"
 *   bus silent / not probed   "6789" a gatewayed OBD port answers only
 *                                    when asked
 *   unreadable / another rate ""     nothing is transmitted
 *
 * @return the count.
 */
int ap_dialect_can_candidates(const ap_bus_t *bus,
                              char out[AP_DIALECT_CAND_LEN]);

#ifndef AUTOPID_HOST_TEST
/* ---- target half ---------------------------------------------------------- */

/** One row of the SAE table as a config row in @p dialect, appended to
 *  @p arr: `pid`, `cmd`, `name`, the parameters with their expressions.
 *  A uds row whose @p owner is a legislated responder also carries the
 *  `init` that addresses that ECU alone (ap_dialect_header_cmd): the
 *  poller gives the functional header back by itself (autopid_runner.c).
 *  False for a PID the table does not know, or knows by name only (no
 *  parameter to decode: such a row would be polled for nothing).
 *  (autopid_std.c) */
bool ap_std_entry_to_json(cJSON *arr, uint8_t pid, ap_dialect_t dialect,
                          uint32_t owner);

/** Who is this car (autopid_identify.c): the responder set and the VIN,
 *  asked in @p dialect on the protocol in effect, @p proto (it names the
 *  functional header to put back). Fills `dialect`, `ecus`, `n_ecus` and
 *  `vin` of @p seen and leaves the rest alone. @p vin_fallback: also try
 *  the manufacturer's `22F190` (the detection job). The chip is left with
 *  headers off and the functional header. @p resp is the caller's reply
 *  buffer. @return true when a VIN or a responder was read. */
bool ap_identify(ap_dialect_t dialect, char proto, bool vin_fallback,
                 ap_veh_seen_t *seen, char *resp, size_t resp_len);

/** The detection job's PID walk (autopid_scan_walk.c, scan task): the
 *  support bitmaps of every range of @p dialect, then one row per
 *  supported PID into @p supported (a uds row with its owner's address).
 *  @return true when at least one range answered. */
bool ap_scan_walk(ap_dialect_t dialect, cJSON *supported, uint16_t *found,
                  char *resp, size_t resp_len);

/** First contact found the car on @p proto in @p dialect: the next
 *  detection job tries that before its own search. Any task. */
void ap_std_scan_hint(char proto, ap_dialect_t dialect);

/* the bus guard for callers that pin a protocol themselves (contact
   probes, the detection job): autopid_guard.c */
/** The bus as the guard saw it last. */
void ap_guard_bus(ap_bus_t *out);
/** May one request go out on the pinned CAN protocol @p proto now? Looks
 *  at the bus again unless its bitrate is already known. */
bool ap_guard_candidate_ok(char proto);
#endif /* !AUTOPID_HOST_TEST */

/** The walk's rows as a config.json document (a new car's tables:
 *  standard PIDs only, the `default` group, every row `"enabled": false`
 *  so nothing is read until the user ticks it, 2026-10-09;
 *  autopid_scan_rows.c, PURE, host-tested). Caller frees. */
char *ap_scan_rows_config(const cJSON *supported);
