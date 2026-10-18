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
 * @file ota_manager.h
 * @brief WiCAN firmware update manager (core service).
 *
 * Owns the device's OWN firmware update: the OTA session over the
 * `ota_0`/`ota_1` app partitions (esp_ota). TRANSPORT-AGNOSTIC by design:
 * the manager exposes one byte-stream session (`begin` → `write`× →
 * `end`/`abort`) and transports feed it:
 *
 *   - today:  HTTP multipart upload from the web UI (`api_http` glue,
 *             `POST /api/ota/upload`, the HTML form path)
 *   - later:  raw HTTP body, TCP, MQTT, …, same four calls, no changes
 *             here (ownership inversion, Architecture §2)
 *
 * One session at a time. `end()` validates the image (esp_ota magic/digest)
 * and sets the boot partition; the TRANSPORT then reboots via
 * `restart_tracker_restart(OTA_APPLY, <source>)`: never from here, and
 * never a raw esp_restart(). A failed/aborted session leaves the running
 * firmware untouched (the inactive partition is scratch space).
 *
 * Naming note: "OTA" = this device's application image. The OBD chip's own
 * firmware update is a different thing (`obd_chip_firmware_update`).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    OTA_MANAGER_IDLE = 0,       /* no session                               */
    OTA_MANAGER_RECEIVING,      /* begin() done, bytes flowing              */
    OTA_MANAGER_READY,          /* end() ok: boot partition set, reboot due */
    OTA_MANAGER_FAILED,         /* session failed; error says why           */
} ota_manager_state_t;

typedef struct
{
    ota_manager_state_t state;
    uint32_t received;          /* bytes written this session               */
    uint32_t total;             /* announced size; 0 = unknown (multipart)  */
    char     error[64];         /* last failure reason ("" when none)       */
    char     target[8];         /* partition being written ("ota_1", …)     */
    char     project_name[33];  /* the image's descriptor, once its head is
                                   in ("" before; 2026-10-18)               */
    char     version[33];
    bool     piped;             /* the session is the pipe's (fw_update)    */
} ota_manager_status_t;

/** Register the log descriptor. No flash access. */
esp_err_t ota_manager_init(void);

/** Ready for sessions. (The confirm of a pending image moved to fw_update's
 *  verify milestone on 2026-10-18: a run must earn it.) */
esp_err_t ota_manager_start(void);
esp_err_t ota_manager_stop(void);

/**
 * Open the ONE update session against the next OTA partition.
 * @param total_size announced image size, or 0 when unknown (multipart).
 * ESP_ERR_INVALID_STATE while another session is RECEIVING.
 */
esp_err_t ota_manager_begin(size_t total_size);

/** Stream image bytes (any chunking). Errors latch FAILED + abort flash. */
esp_err_t ota_manager_write(const uint8_t *data, size_t len);

/** Finish: validate the image, set the boot partition → READY. The caller
 *  (transport) reboots via restart_tracker_restart(OTA_APPLY, source). */
esp_err_t ota_manager_end(void);

/** Drop the session; the running firmware is untouched. */
esp_err_t ota_manager_abort(void);

/** Snapshot for progress/diagnostics (GET /api/ota/status). */
esp_err_t ota_manager_status(ota_manager_status_t *out);

/** State-change notification (RECEIVING at begin, READY/FAILED/IDLE at
 *  end/abort). Runs in the TRANSPORT's context, outside the session lock:
 *  keep it short. One consumer: the composition root (e.g. wiring the
 *  "update in progress" LED indication). Register before start(). */
typedef void (*ota_manager_event_cb_t)(ota_manager_state_t state);
esp_err_t ota_manager_set_event_cb(ota_manager_event_cb_t cb);

/* ---- the image's identity and digest (2026-10-18) ------------------------------ */

/**
 * Name the product (main, after init, before any session): an image whose
 * app descriptor's project name starts with none of @p prefixes, or that
 * is not an ESP32-S3 application image, is refused at its first 288 bytes
 * with "not a <product> image: <name>" and never reaches the flash. The
 * pointers must stay valid for the life of the firmware. Without this
 * call every image passes (the pre-2026-10-18 contract).
 */
esp_err_t ota_manager_set_product(const char *product,
                                  const char *const *prefixes, size_t count);

/** Arm an expected SHA-256 for the NEXT begin (the upload route's
 *  `?sha256=`): `end()` fails with "sha256 mismatch" and nothing is
 *  activated when the bytes written differ. NULL disarms. */
esp_err_t ota_manager_expect_sha256(const uint8_t sha256[32]);

/** Confirm the running image to the bootloader when it is NEW or
 *  PENDING_VERIFY (esp_ota_mark_app_valid_cancel_rollback). Callers:
 *  fw_update at its verify milestone and before a planned restart. One
 *  otadata write; the caller's stack must be internal. */
esp_err_t ota_manager_confirm_running(void);

/** "new" / "pending_verify" / "valid" / "invalid" / "aborted" /
 *  "undefined" for an esp_ota_img_states_t value. */
const char *ota_manager_img_state_str(int esp_ota_img_state);

/* ---- the pipe (ota_pipe.c) ---------------------------------------------------- */

/**
 * The session for a producer on a PSRAM stack (fw_update): begin, write and
 * end run on a 5 KB internal-stack writer created for the session (its
 * stack goes back to the heap at end or abort); the producer copies into
 * two 4 KB PSRAM slots and blocks while both are full. ESP_ERR_INVALID_STATE
 * while another session (a pipe or an upload) is receiving. @p wait_ms
 * bounds a write's wait for a free slot and end's wait for the writer
 * (end validates the image: allow the flash the time).
 */
esp_err_t ota_manager_pipe_begin(size_t total_size,
                                 const uint8_t *expected_sha256);
esp_err_t ota_manager_pipe_write(const uint8_t *data, size_t len,
                                 uint32_t wait_ms);
esp_err_t ota_manager_pipe_end(uint32_t wait_ms);
esp_err_t ota_manager_pipe_abort(void);

#ifdef __cplusplus
}
#endif
