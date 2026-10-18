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
 * @file fw_update_dongle.h
 * @brief Shared between the relay's two files: the state (the dongle's
 *        manifest view and selection, the last endpoint seen, the upload
 *        route probe, the running job's release, the last result) and the
 *        helpers the job needs.
 */
#pragma once

#include "fw_update_private.h"

#define FWD_STAGE_MAX         (4u * 1024u * 1024u)
#define FWD_STAGE_RESERVE     (512u * 1024u)
#define FWD_PUSH_CHUNK        8192
#define FWD_PUSH_TIMEOUT_MS   30000   /* a socket write, and the dongle's digest pass */
#define FWD_PROBE_TIMEOUT_MS  4000
#define FWD_STEADY_WAIT_S     20
#define FWD_DROP_WAIT_S       20
#define FWD_RETURN_WAIT_USB_S 90
#define FWD_RETURN_WAIT_AP_S  150
#define FWD_VERIFY_WAIT_S     45
#define FWD_HEAD_LEN          288
#define FWD_MIN_API_LEVEL     7

typedef struct
{
    bool     valid;
    uint32_t id;
    char     from[24];
    char     to[FW_UPDATE_VER_LEN];
    fw_update_job_state_t state;
    char     error[FW_UPDATE_CODE_LEN];
    char     detail[FW_UPDATE_DETAIL_LEN];
    int64_t  time;
    uint32_t push_s;
} fwd_last_t;

/** Under the engine lock, like the engine's own state. */
typedef struct
{
    fwu_manifest_t  view;
    bool            view_valid;
    fwu_selection_t sel;
    char            etag[64];
    fwu_check_t     check;
    fw_update_dongle_endpoint_t ep;   /**< the last endpoint seen          */
    bool            ep_valid;
    bool            ota_probed;       /**< GET /api/ota/status asked        */
    bool            ota_capable;      /**< ... and it answered (not 404)    */
    char            ota_probed_fw[24];
    fwd_last_t      last;
    fwu_release_t   rel;              /**< the running job's release        */
    uint32_t        stage_cap;        /**< a bench knob                     */
} fwd_state_t;

fwd_state_t *fwd_state(void);
#define FWD (fwd_state())

/** The endpoint now; false without a relay. */
bool fwd_endpoint_now(fw_update_dongle_endpoint_t *ep);
bool fwd_version_equal(const char *a, const char *b);
/** GET http://<host>/api/ota/status: 1 idle/ready/failed, 2 receiving,
 *  0 no route (404), -1 no answer. */
int fwd_probe_ota(const char *host);
void fwd_note_probe(const fw_update_dongle_endpoint_t *ep, int rc);
/** The selection against the dongle's view. Under the lock. */
void fwd_select(void);

/** The staging buffer for an image of @p size fits: a trial allocation
 *  answers the contiguous question (nothing else can), and the heap's
 *  total free stays above the reserve afterwards; the bench cap applies.
 *  @p detail (nullable, FW_UPDATE_DETAIL_LEN) receives the numbers. */
bool fwd_stage_fits(size_t size, char *detail);
