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
 * @file restart_tracker.h
 * @brief WiCAN restart tracker (core component).
 *
 * Answers "why did the device reboot?" across warm restarts without touching
 * flash: its state lives in PSRAM `.noinit` (EXT_RAM_NOINIT_ATTR) guarded by
 * a magic/version/CRC envelope. A power cycle leaves random PSRAM: the
 * envelope detects that and starts fresh; any warm reset (esp_restart, panic,
 * watchdog, brownout-less resets) preserves the full history.
 *
 * Two jobs:
 *  1. Every boot, record one history entry: reset reason, timestamp (when
 *     wall time is valid), and (if the previous run declared it) the
 *     planned reason/source of the restart.
 *  2. Give the firmware ONE sanctioned way to reboot on purpose:
 *     restart_tracker_restart(reason, source, flags). Transports that call
 *     esp_restart() directly rob the next boot of its "why".
 *
 * Lifecycle: restart_tracker_init() must run EARLY in main (right after
 * log_manager_init) so the boot record is written before anything can crash.
 * start()/stop() exist for lifecycle uniformity (Coding Standard §3) and are
 * no-ops: the component is passive after init.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RESTART_TRACKER_HISTORY_LEN 8

typedef enum
{
    RESTART_TRACKER_PLANNED_REASON_NONE = 0,
    RESTART_TRACKER_PLANNED_REASON_USER_REQUEST,
    RESTART_TRACKER_PLANNED_REASON_CONFIG_APPLY,     /* settings submit-then-reboot */
    RESTART_TRACKER_PLANNED_REASON_CONFIG_RECOVERY,
    RESTART_TRACKER_PLANNED_REASON_OTA_APPLY,
    RESTART_TRACKER_PLANNED_REASON_FACTORY_RESET,
    RESTART_TRACKER_PLANNED_REASON_SAFE_MODE,
    RESTART_TRACKER_PLANNED_REASON_POWER_WAKE,
    RESTART_TRACKER_PLANNED_REASON_INTERNAL_RECOVERY,
    /* sleep_manager's periodic check-in wake (2026-09-07; it shared
       POWER_WAKE before). Appended: records store the numeric value. */
    RESTART_TRACKER_PLANNED_REASON_PERIODIC_WAKE,
    /* a device the crash-loop brake had parked starts again: the park's
       timer or the button (2026-10-05). Appended, as above. */
    RESTART_TRACKER_PLANNED_REASON_PARK_RETRY,
    RESTART_TRACKER_PLANNED_REASON_PARTITION_MIGRATE, /* the partition table
                                                         was rewritten with this
                                                         build's (2026-10-10)  */
    /* the boot path rolled a firmware update back to the previous slot
       after three quick crashes of the new image (fw_update_boot_check,
       2026-10-18). Appended: fw_update_record.h keys on the value 12. */
    RESTART_TRACKER_PLANNED_REASON_OTA_ROLLBACK,
} restart_tracker_planned_reason_t;

typedef enum
{
    RESTART_TRACKER_SOURCE_UNKNOWN = 0,
    RESTART_TRACKER_SOURCE_WEB_UI,
    RESTART_TRACKER_SOURCE_CMDLINE,
    RESTART_TRACKER_SOURCE_CONSOLE,
    RESTART_TRACKER_SOURCE_MQTT,
    RESTART_TRACKER_SOURCE_OTA,
    RESTART_TRACKER_SOURCE_SAFE_MODE,
    RESTART_TRACKER_SOURCE_CONFIG_SERVER,
    RESTART_TRACKER_SOURCE_SLEEP_MODE,
    RESTART_TRACKER_SOURCE_BUTTON,   /* config-mode timeout (2026-07-19) */
    RESTART_TRACKER_SOURCE_PAIRING,  /* espnetlink_link zero-touch pairing */
    RESTART_TRACKER_SOURCE_PARK,     /* the crash park's own timer         */
    RESTART_TRACKER_SOURCE_BOOT,     /* the boot path itself (partition_migrate) */
} restart_tracker_source_t;

/** How a boot ran: the whole firmware, or one of the minimal modes. */
typedef enum
{
    RESTART_TRACKER_BOOT_NORMAL = 0,
    RESTART_TRACKER_BOOT_PARK,      /**< parked asleep by the crash-loop brake */
    RESTART_TRACKER_BOOT_PARK_BARE, /**< parked without the LED: the park
                                         itself had crashed                   */
    RESTART_TRACKER_BOOT_SAFE,      /**< safe mode (the button at power-on)   */
} restart_tracker_boot_mode_t;

/** One boot in the history ring. */
typedef struct
{
    uint32_t sequence;            /**< monotonically increasing boot number   */
    int64_t  boot_timestamp;      /**< unix time at boot; 0 if clock invalid  */
    int64_t  request_timestamp;   /**< when the restart was requested (planned) */
    uint64_t request_uptime_ms;   /**< uptime at the restart request           */
    uint32_t actual_reset_reason; /**< esp_reset_reason_t of THIS boot          */
    uint32_t flags;               /**< caller-defined flags from the request    */
    uint16_t planned_reason;      /**< restart_tracker_planned_reason_t         */
    uint16_t source;              /**< restart_tracker_source_t                 */
    uint8_t  was_planned;         /**< 1 if the previous run announced it       */
    uint8_t  time_valid;          /**< 1 if boot_timestamp is trustworthy       */
    /* the two bytes below were `reserved`, zero in every record so far: the
       layout and RT_VERSION are unchanged (2026-10-05) */
    uint8_t  settled;             /**< 1 once this run was up
                                       RESTART_TRACKER_SETTLE_S                 */
    uint8_t  boot_mode;           /**< restart_tracker_boot_mode_t of this run  */
} restart_tracker_record_t;

/** Restart intent left for the NEXT boot to consume. */
typedef struct
{
    int64_t  requested_timestamp;
    uint64_t requested_uptime_ms;
    uint32_t flags;
    uint16_t planned_reason;
    uint16_t source;
    uint8_t  valid;
    uint8_t  time_valid;
    uint8_t  reserved[2];
} restart_tracker_pending_t;

/**
 * The PSRAM-resident state. `crc32` MUST stay the last member: the CRC spans
 * [magic, offsetof(crc32)), so layout changes require a `version` bump.
 *
 * `mspi_tuning_guard` MUST stay the first member and outside the CRC: on the
 * ESP32-S3, MSPI PSRAM timing tuning writes a 64-byte test pattern at PSRAM
 * physical address 0 on EVERY boot, and `.ext_ram_noinit` starts there. If
 * this object links first in the section, only the guard is sacrificed.
 */
typedef struct
{
    uint8_t  mspi_tuning_guard[64];
    uint32_t magic;
    uint32_t version;
    uint32_t history_len;
    uint32_t boot_count;
    uint32_t unexpected_reset_count; /**< panics/watchdogs, not sw/poweron/sleep */
    uint32_t record_sequence;
    uint32_t latest_history_index;
    uint32_t next_history_index;
    restart_tracker_pending_t pending_restart;
    restart_tracker_record_t  history[RESTART_TRACKER_HISTORY_LEN];
    uint32_t crc32;
} restart_tracker_state_t;

/** Validate/adopt the PSRAM state and record this boot. Call early in main. */
esp_err_t restart_tracker_init(void);

/** Lifecycle uniformity (§3); passive component: both return ESP_OK. */
esp_err_t restart_tracker_start(void);
esp_err_t restart_tracker_stop(void);

/**
 * Announce an intentional restart WITHOUT performing it (for callers that
 * must do their own teardown before esp_restart()).
 */
esp_err_t restart_tracker_mark_planned_restart(restart_tracker_planned_reason_t reason,
                                               restart_tracker_source_t source,
                                               uint32_t flags);

/**
 * A hook that runs at the start of every planned restart, in the caller's
 * context, before the intent is recorded (2026-10-18; one consumer:
 * fw_update confirms a pending image to the bootloader before a restart
 * that is not a rollback or a park retry). Must return within a second or
 * two; a flash write inside it goes through an internal-stack helper (the
 * caller may be on a PSRAM stack). NULL removes it.
 */
typedef void (*restart_tracker_pre_restart_fn)(
    restart_tracker_planned_reason_t reason, restart_tracker_source_t source,
    uint32_t flags);
esp_err_t restart_tracker_set_pre_restart_hook(restart_tracker_pre_restart_fn fn);

/** The one sanctioned reboot: mark planned, then esp_restart(). No return. */
void restart_tracker_restart(restart_tracker_planned_reason_t reason,
                             restart_tracker_source_t source,
                             uint32_t flags) __attribute__((noreturn));

/** Snapshot the whole state (history ring included). */
esp_err_t restart_tracker_get_state(restart_tracker_state_t *out_state);

/** The record for THIS boot. ESP_ERR_NOT_FOUND before init has recorded it. */
esp_err_t restart_tracker_get_latest_record(restart_tracker_record_t *out_record);

/** Register the `restart_tracker` CLI command with cmdline_manager.
 *  Called INTERNALLY on the settings boot apply when the `cli` setting is
 *  true (default): main no longer wires it. */
esp_err_t restart_tracker_register_cli(void);

/** Register the settings descriptor ({cli}). Init runs before
 *  settings_manager_init, so the composition root calls this separately
 *  (the log_manager_register_settings pattern). */
esp_err_t restart_tracker_register_settings(void);

/* human-readable names (for logs / status JSON) */
const char *restart_tracker_reset_reason_to_str(uint32_t esp_reset_reason);
const char *restart_tracker_planned_reason_to_str(restart_tracker_planned_reason_t reason);
const char *restart_tracker_source_to_str(restart_tracker_source_t source);

/* ---- the crash note -------------------------------------------------------
 * Where the previous run crashed, when it ended in IDF's panic handler: two
 * link-time hooks around that handler write a 300-byte note into RTC slow
 * memory (no flash, no PSRAM, no heap), and the next boot files it under its
 * own record. See README.md, "The crash note". */

typedef enum
{
    RESTART_TRACKER_CRASH_NONE = 0,
    RESTART_TRACKER_CRASH_EXCEPTION, /* a CPU exception nobody handled       */
    RESTART_TRACKER_CRASH_ABORT,     /* abort(), assert, stack overflow hook */
    RESTART_TRACKER_CRASH_INT_WDT,   /* interrupt watchdog                   */
    RESTART_TRACKER_CRASH_TASK_WDT,  /* task watchdog, when it panics        */
    RESTART_TRACKER_CRASH_DEBUG,     /* debug exception                      */
} restart_tracker_crash_kind_t;

#define RESTART_TRACKER_CRASH_BT_LEN  16 /* frames kept of the crashed context */
#define RESTART_TRACKER_CRASH_BT2_LEN 6  /* frames kept of the other core      */

/** A crash note as its readers get it: strings terminated and printable. */
typedef struct
{
    uint32_t sequence;    /**< the boot it is filed under (the one after)     */
    uint8_t  kind;        /**< restart_tracker_crash_kind_t                   */
    uint8_t  core;        /**< the core that crashed                          */
    bool     complete;    /**< false: only kind, core, cause, pc, excvaddr    */
    bool     pseudo;      /**< cause is one of IDF's PANIC_RSN_ numbers       */
    bool     in_isr;      /**< the crash happened inside an interrupt handler */
    bool     nested;      /**< a second panic happened inside the handler     */
    bool     early;       /**< before restart_tracker_init(): no uptime       */
    bool     bt_corrupt;  /**< IDF's sanity check ended the stack walk        */
    bool     bt_more;     /**< the stack went on after the last frame kept    */
    bool     text_cut;    /**< the abort text was longer than `text`          */
    uint32_t cause;       /**< EXCCAUSE (28 = LoadProhibited)                 */
    uint32_t pc;          /**< the register, as IDF's register dump prints it */
    uint32_t excvaddr;    /**< the address a load / store / fetch faulted on  */
    uint32_t uptime_s;    /**< seconds the run had been up                    */
    uint8_t  bt_len;
    uint8_t  bt2_len;
    uint8_t  bt2_core;
    char     elf_sha[17]; /**< first hex characters of the image's ELF SHA-256 */
    char     task[17];
    char     reason[33];  /**< IDF's name of the exception; "" for an abort   */
    char     text[97];    /**< the abort / assert / stack overflow message    */
    uint32_t bt[RESTART_TRACKER_CRASH_BT_LEN];   /**< the PCs of IDF's
                               `Backtrace:` line (bt[0] = pc - 3)             */
    uint32_t bt2[RESTART_TRACKER_CRASH_BT2_LEN]; /**< the other core, when IDF
                               saved its frame (watchdog, cache error)        */
} restart_tracker_crash_t;

/** The crash note filed under boot `sequence`. ESP_ERR_NOT_FOUND when that
 *  boot has none; ESP_ERR_NOT_SUPPORTED on a target without the hooks. */
esp_err_t restart_tracker_get_crash(uint32_t sequence,
                                    restart_tracker_crash_t *out);

/** "exception", "abort", "int_wdt", "task_wdt", "debug"; "none" otherwise. */
const char *restart_tracker_crash_kind_to_str(uint8_t kind);

/** The note in one line (the boot log, the CLI). snprintf semantics. */
int restart_tracker_crash_summary(const restart_tracker_crash_t *crash,
                                  char *buf, size_t cap);

/* ---- the crash-loop brake ---------------------------------------------------
 * Three runs in a row that crashed before they had been up
 * RESTART_TRACKER_SETTLE_S, and the next boot must not start the firmware
 * again: its verdict is "park" and the composition root puts the board to
 * sleep instead (README.md, "The crash-loop brake"). The count lives in RTC
 * memory beside the crash notes: no flash, no PSRAM, and a power cycle
 * starts it again. */

#define RESTART_TRACKER_SETTLE_S     600U /* up this long = a healthy run   */
#define RESTART_TRACKER_BRAKE_STREAK 3U   /* quick crashes in a row to park */

/** What restart_tracker_init() decided for this boot, and the count as it
 *  stands now. */
typedef struct
{
    uint8_t  verdict;       /**< this boot's: restart_tracker_boot_mode_t
                                 NORMAL, PARK or PARK_BARE                    */
    uint8_t  streak;        /**< runs in a row that crashed before settling
                                 (0 again once this run settled)              */
    uint8_t  parks;         /**< parks since that streak began, this one in   */
    uint8_t  report_budget; /**< crash reports that may still be stored       */
    bool     settled;       /**< this run was up RESTART_TRACKER_SETTLE_S     */
    uint32_t retry_after_s; /**< parked: start again after this long; 0 =
                                 stay parked until the power is cycled or
                                 the button is pressed                        */
} restart_tracker_brake_t;

/** The verdict of this boot. The composition root asks right after
 *  restart_tracker_init() and, when it is not NORMAL, parks instead of
 *  starting anything. ESP_ERR_INVALID_STATE before init. */
esp_err_t restart_tracker_get_brake(restart_tracker_brake_t *out);

/** Declare how this boot runs (park, safe mode) before the mode does
 *  anything: the next boot's verdict and the history record read it. */
esp_err_t restart_tracker_set_boot_mode(restart_tracker_boot_mode_t mode);

/** Call now and then (main's 60 s loop). Marks this run settled once it has
 *  been up RESTART_TRACKER_SETTLE_S: that ends a crash streak and gives the
 *  report budget back. `force` marks it at once (the bench's knob). Returns
 *  true when the run is settled. No flash. */
bool restart_tracker_settle(bool force);

/** Bench: the next park ends by itself after `seconds` (10..3600; 0 takes
 *  the knob back). One park only, and gone with the power. */
esp_err_t restart_tracker_set_test_retry(uint16_t seconds);

/** "normal", "park", "park_bare", "safe". */
const char *restart_tracker_boot_mode_to_str(uint8_t mode);

/* ---- the stored crash report ------------------------------------------------
 * The crash notes live in RTC memory and go with the power. The boot that
 * files a note also stores it in NVS, so that it is still there after the
 * device was unplugged: for the Status page, GET /api/restart/report, the
 * console and safe mode. One report, the newest distinct crash, behind a
 * wear guard (README.md, "The stored crash report"). */

typedef struct
{
    int64_t stored_unix;  /**< when it was stored; 0: the clock was not set */
    uint8_t streak;       /**< quick crashes in a row then, this one in     */
    bool    parked;       /**< the brake parked the device on this crash    */
    char    firmware[33]; /**< version of the image that crashed; "" when
                               another image stored the report             */
    restart_tracker_crash_t crash;
} restart_tracker_report_t;

#define RESTART_TRACKER_REPORT_TEXT_MAX 1024 /* holds any report as text */

/** The stored report. ESP_ERR_NOT_FOUND when there is none. */
esp_err_t restart_tracker_get_report(restart_tracker_report_t *out);

/** Forget the stored report (one NVS erase). Flash: a caller on an
 *  internal stack only (an HTTP handler, a console command). */
esp_err_t restart_tracker_clear_report(void);

/** Write the report this boot holds back to NVS, for a caller that erased
 *  NVS wholesale and initialised it again (safe mode's factory reset).
 *  ESP_ERR_NOT_FOUND when there is none. Same stack rule. */
esp_err_t restart_tracker_restore_report(void);

/** The report as text for a person to send on: device, firmware, what
 *  crashed, the backtrace. snprintf semantics. */
int restart_tracker_report_text(const restart_tracker_report_t *report,
                                char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
