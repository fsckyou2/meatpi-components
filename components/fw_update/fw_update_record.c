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
 * @file fw_update_record.c
 * @brief The job record's codec and the boot decision table. Pure.
 */
#include "fw_update_record.h"

#include <stddef.h>
#include <string.h>

uint32_t fwu_crc32(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;

    for (size_t i = 0; i < n; i++)
    {
        crc ^= p[i];

        for (int b = 0; b < 8; b++)
        {
            crc = (crc & 1U) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
        }
    }

    return ~crc;
}

bool fwu_record_valid(const fwu_record_t *r)
{
    return r != NULL && r->magic == FWU_RECORD_MAGIC &&
           r->version == FWU_RECORD_VERSION && r->len == FWU_RECORD_LEN &&
           sizeof(*r) == FWU_RECORD_LEN &&
           fwu_crc32((const uint8_t *)r, offsetof(fwu_record_t, crc32)) ==
               r->crc32;
}

void fwu_record_seal(fwu_record_t *r)
{
    r->magic = FWU_RECORD_MAGIC;
    r->version = FWU_RECORD_VERSION;
    r->len = FWU_RECORD_LEN;
    r->from_version[sizeof(r->from_version) - 1] = '\0';
    r->to_version[sizeof(r->to_version) - 1] = '\0';
    r->error[sizeof(r->error) - 1] = '\0';
    r->crc32 = fwu_crc32((const uint8_t *)r, offsetof(fwu_record_t, crc32));
}

/* ---- the boot decision ------------------------------------------------------------ */

static bool milestone_reason(uint16_t reason)
{
    /* the restart INTO the image, a rollback, a park's retry, safe mode and
       the partition rewrite say nothing about the image having run well */
    return reason != FWU_REASON_OTA_APPLY && reason != FWU_REASON_OTA_ROLLBACK &&
           reason != FWU_REASON_PARK_RETRY && reason != FWU_REASON_SAFE_MODE &&
           reason != FWU_REASON_PARTITION_MIGRATE;
}

fwu_boot_action_t fwu_boot_decide(const fwu_boot_in_t *in)
{
    if (in == NULL || !in->have_record || in->rec_state != FW_JOB_REBOOTING)
    {
        return FWU_BOOT_NOTHING;
    }

    if (in->running_slot != in->rec_slot)
    {
        /* the new image is not the one running: the bootloader refused it
           (its state INVALID / ABORTED) or never got to it */
        return (in->last_invalid_known && in->last_invalid_slot == in->rec_slot)
                   ? FWU_BOOT_WRITE_ROLLED_BACK_BL
                   : FWU_BOOT_WRITE_BOOT_FAILED;
    }

    if (in->rec_flags & FWU_FLAG_LAYOUT_CHANGED)
    {
        /* the table was rewritten for this image: the old one under the
           new table would rewrite and format again, so it is confirmed */
        return FWU_BOOT_WRITE_VERIFIED;
    }

    if (in->latest_planned && milestone_reason(in->latest_reason))
    {
        return FWU_BOOT_WRITE_VERIFIED; /* the previous run was operated */
    }

    if (in->brake_parks)
    {
        return FWU_BOOT_ROLLBACK_APP;
    }

    return FWU_BOOT_KEEP_PENDING;
}

const char *fwu_job_state_str(fw_update_job_state_t s)
{
    switch (s)
    {
        case FW_JOB_IDLE:           return "idle";
        case FW_JOB_CHECKING:       return "checking";
        case FW_JOB_DOWNLOADING:    return "downloading";
        case FW_JOB_VERIFYING:      return "verifying";
        case FW_JOB_PUSHING:        return "pushing";
        case FW_JOB_REBOOTING:      return "rebooting";
        case FW_JOB_PENDING_VERIFY: return "pending_verify";
        case FW_JOB_VERIFIED:       return "verified";
        case FW_JOB_ROLLED_BACK:    return "rolled_back";
        case FW_JOB_BOOT_FAILED:    return "boot_failed";
        case FW_JOB_FAILED:         return "failed";
        case FW_JOB_ABORTED:        return "aborted";
        default:                    return "?";
    }
}

const char *fwu_by_str(uint8_t by)
{
    switch (by)
    {
        case FWU_BY_APP:        return "app";
        case FWU_BY_BOOTLOADER: return "bootloader";
        default:                return "";
    }
}
