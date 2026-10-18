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
 * @file fw_update_record.h
 * @brief The job record (an NVS blob that outlives the restart and a power
 *        cycle) and the boot decision table: what the boot after an update
 *        does with it. Pure: host-tested.
 *
 * The record is written at REBOOTING (right before the planned restart into
 * the new image) and then exactly once more with the outcome: VERIFIED,
 * ROLLED_BACK or BOOT_FAILED (FAILED / ABORTED for a job that failed after
 * the download started). 112 bytes, CRC-32, at most 5 NVS writes per job
 * and none in steady state.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fw_update.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FWU_RECORD_MAGIC   0x424A5746u /* "FWJB" */
#define FWU_RECORD_VERSION 1U
#define FWU_RECORD_LEN     112U

#define FWU_FLAG_LAYOUT_CHANGED 0x01U /* the table changes: no rollback */

#define FWU_BY_NONE       0U
#define FWU_BY_APP        1U
#define FWU_BY_BOOTLOADER 2U

typedef struct __attribute__((packed))
{
    uint32_t magic;
    uint16_t version;
    uint16_t len;
    uint32_t job_id;
    uint8_t  state;        /**< fw_update_job_state_t                      */
    uint8_t  target;       /**< fw_update_target_t                         */
    uint8_t  slot_subtype; /**< the app partition subtype written           */
    uint8_t  flags;        /**< FWU_FLAG_*                                  */
    uint8_t  by;           /**< FWU_BY_*: who rolled back                   */
    uint8_t  reserved[3];
    char     from_version[32];
    char     to_version[32];
    char     error[16];
    int64_t  time;         /**< unix seconds of the last write, 0 no clock */
    uint32_t crc32;
} fwu_record_t;

uint32_t fwu_crc32(const uint8_t *p, size_t n);

/** Magic, version, length and CRC all right. */
bool fwu_record_valid(const fwu_record_t *r);

/** Set magic, version, length and the CRC over everything before it. */
void fwu_record_seal(fwu_record_t *r);

/* ---- the boot decision ------------------------------------------------------------ */

typedef struct
{
    bool     have_record;        /**< a valid record was read               */
    uint8_t  rec_state;
    uint8_t  rec_slot;
    uint8_t  rec_flags;
    uint8_t  running_slot;       /**< subtype of the running partition       */
    bool     last_invalid_known; /**< esp_ota_get_last_invalid_partition()  */
    uint8_t  last_invalid_slot;
    bool     latest_planned;     /**< this boot followed a planned restart   */
    uint16_t latest_reason;      /**< restart_tracker_planned_reason_t       */
    bool     brake_parks;        /**< the crash-loop brake's verdict: park   */
} fwu_boot_in_t;

typedef enum
{
    FWU_BOOT_NOTHING = 0,      /**< no pending job                           */
    FWU_BOOT_WRITE_ROLLED_BACK_BL, /**< the bootloader fell back: record it  */
    FWU_BOOT_WRITE_BOOT_FAILED,/**< the new image never ran: record it       */
    FWU_BOOT_WRITE_VERIFIED,   /**< the milestone was reached: record it     */
    FWU_BOOT_ROLLBACK_APP,     /**< three quick crashes: mark invalid, restart */
    FWU_BOOT_KEEP_PENDING,     /**< the new image runs, not yet verified     */
} fwu_boot_action_t;

/** Planned reasons that are NOT the milestone (numeric values of
 *  restart_tracker_planned_reason_t, kept here so the core stays pure). */
#define FWU_REASON_OTA_APPLY         4U
#define FWU_REASON_SAFE_MODE         6U
#define FWU_REASON_PARK_RETRY        10U
#define FWU_REASON_PARTITION_MIGRATE 11U
#define FWU_REASON_OTA_ROLLBACK      12U

fwu_boot_action_t fwu_boot_decide(const fwu_boot_in_t *in);

/** The state words GET /api/fw_update and the console print. */
const char *fwu_job_state_str(fw_update_job_state_t s);
const char *fwu_by_str(uint8_t by);

#ifdef __cplusplus
}
#endif
