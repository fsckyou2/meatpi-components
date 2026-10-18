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
 * @file restart_tracker_core.c
 * @brief Pure restart-tracking state machine. No IDF dependencies: compiled
 *        as-is by the host unit tests. All wall-clock/uptime/reset-reason
 *        inputs are injected (rt_inputs_t).
 */
#include "restart_tracker_private.h"

#include <stddef.h>
#include <string.h>

/* Reset-reason codes mirrored from esp_reset_reason_t, the IDF 5+ numbering
 * (UNKNOWN 0, POWERON 1, EXT 2, SW 3, PANIC 4, INT_WDT 5, TASK_WDT 6, WDT 7,
 * DEEPSLEEP 8, BROWNOUT 9, SDIO 10, USB 11, JTAG 12, EFUSE 13, PWR_GLITCH 14,
 * CPU_LOCKUP 15). Kept as plain numbers so this file stays host-buildable.
 * Until 2026-10-03 the table carried IDF 4's numbers (DEEPSLEEP 5, BROWNOUT
 * 6, the watchdogs 7..9): an interrupt-watchdog reset was recorded as a
 * deep-sleep wake and not counted as unexpected (bench, phase 5). Until
 * 2026-10-17 a USB-Serial-JTAG reset (esptool's hard reset after a flash
 * over the native USB port) and a JTAG reset counted as unexpected: both
 * are deliberate, like the EN pin (found on the ESPNetLink production
 * station, 2026-10-16). */
#define RT_RST_POWERON   1U
#define RT_RST_EXT       2U
#define RT_RST_SW        3U
#define RT_RST_DEEPSLEEP 8U
#define RT_RST_USB       11U
#define RT_RST_JTAG      12U

/* CRC-32 (IEEE 802.3, poly 0xEDB88320), same convention as the settings
 * codec; local so the core has zero deps. Shared with the crash note
 * (restart_tracker_crash_core.c). */
uint32_t rt_crc32_bytes(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;

    for (size_t i = 0; i < len; i++)
    {
        crc ^= data[i];

        for (int bit = 0; bit < 8; bit++)
        {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }

    return ~crc;
}

uint32_t rt_crc32(const restart_tracker_state_t *state)
{
    /* span [magic, crc32): the mspi_tuning_guard head is sacrificial and
       excluded, boot-time timing tuning may overwrite it (see header) */
    size_t start = offsetof(restart_tracker_state_t, magic);

    return rt_crc32_bytes((const uint8_t *)state + start,
                       offsetof(restart_tracker_state_t, crc32) - start);
}

bool rt_state_is_valid(const restart_tracker_state_t *state)
{
    return state->magic == RT_MAGIC &&
           state->version == RT_VERSION &&
           state->history_len == RESTART_TRACKER_HISTORY_LEN &&
           rt_crc32(state) == state->crc32;
}

void rt_state_reset(restart_tracker_state_t *state)
{
    memset(state, 0, sizeof(*state));
    state->magic = RT_MAGIC;
    state->version = RT_VERSION;
    state->history_len = RESTART_TRACKER_HISTORY_LEN;
}

bool rt_reset_reason_is_unexpected(uint32_t reason)
{
    switch (reason)
    {
        case RT_RST_POWERON:
        case RT_RST_EXT: /* EN-pin reset: flasher/reset button = deliberate */
        case RT_RST_SW:
        case RT_RST_DEEPSLEEP:
        case RT_RST_USB:  /* USB-Serial-JTAG reset: esptool's hard reset after
                             a flash over the native USB port */
        case RT_RST_JTAG: /* a debugger: deliberate like the EN pin */
            return false;
        default:
            return true; /* panic, watchdogs, brownout, unknown */
    }
}

static bool time_is_valid(int64_t now_unix)
{
    return now_unix >= RT_MIN_VALID_UNIX_TIME;
}

bool rt_record_boot(restart_tracker_state_t *state, const rt_inputs_t *in)
{
    bool was_reset = false;

    if (!rt_state_is_valid(state))
    {
        rt_state_reset(state); /* cold boot or corrupted PSRAM */
        was_reset = true;
    }

    uint32_t slot = state->next_history_index % RESTART_TRACKER_HISTORY_LEN;
    restart_tracker_record_t *record = &state->history[slot];

    memset(record, 0, sizeof(*record));
    record->sequence = ++state->record_sequence;
    record->boot_timestamp = time_is_valid(in->now_unix) ? in->now_unix : 0;
    record->time_valid = time_is_valid(in->now_unix) ? 1U : 0U;
    record->actual_reset_reason = in->reset_reason;

    if (state->pending_restart.valid)
    {
        record->request_timestamp = state->pending_restart.requested_timestamp;
        record->request_uptime_ms = state->pending_restart.requested_uptime_ms;
        record->planned_reason = state->pending_restart.planned_reason;
        record->source = state->pending_restart.source;
        record->flags = state->pending_restart.flags;
        record->was_planned = 1U;
    }

    state->boot_count++;

    if (!record->was_planned && rt_reset_reason_is_unexpected(in->reset_reason))
    {
        state->unexpected_reset_count++;
    }

    state->latest_history_index = slot;
    state->next_history_index = (slot + 1U) % RESTART_TRACKER_HISTORY_LEN;
    memset(&state->pending_restart, 0, sizeof(state->pending_restart));
    state->crc32 = rt_crc32(state);

    return was_reset;
}

void rt_record_set_mode(restart_tracker_state_t *state, uint8_t mode)
{
    if (!rt_state_is_valid(state) || state->boot_count == 0U)
    {
        return; /* no record of this boot to write into */
    }

    state->history[state->latest_history_index %
                   RESTART_TRACKER_HISTORY_LEN].boot_mode = mode;
    state->crc32 = rt_crc32(state);
}

void rt_record_set_settled(restart_tracker_state_t *state)
{
    if (!rt_state_is_valid(state) || state->boot_count == 0U)
    {
        return;
    }

    state->history[state->latest_history_index %
                   RESTART_TRACKER_HISTORY_LEN].settled = 1U;
    state->crc32 = rt_crc32(state);
}

void rt_mark_planned(restart_tracker_state_t *state, const rt_inputs_t *in,
                     restart_tracker_planned_reason_t reason,
                     restart_tracker_source_t source, uint32_t flags)
{
    if (!rt_state_is_valid(state))
    {
        rt_state_reset(state);
    }

    state->pending_restart.requested_timestamp =
        time_is_valid(in->now_unix) ? in->now_unix : 0;
    state->pending_restart.requested_uptime_ms = in->uptime_ms;
    state->pending_restart.flags = flags;
    state->pending_restart.planned_reason = (uint16_t)reason;
    state->pending_restart.source = (uint16_t)source;
    state->pending_restart.valid = 1U;
    state->pending_restart.time_valid = time_is_valid(in->now_unix) ? 1U : 0U;
    state->crc32 = rt_crc32(state);
}

/* ---- name tables (pure, shared by logs and status JSON) ------------------- */

const char *restart_tracker_reset_reason_to_str(uint32_t esp_reset_reason)
{
    switch (esp_reset_reason)
    {
        case 0:  return "unknown";
        case 1:  return "poweron";
        case 2:  return "external";
        case 3:  return "software";
        case 4:  return "panic";
        case 5:  return "interrupt_wdt"; /* ESP_RST_INT_WDT    */
        case 6:  return "task_wdt";      /* ESP_RST_TASK_WDT   */
        case 7:  return "wdt";           /* ESP_RST_WDT        */
        case 8:  return "deepsleep";     /* ESP_RST_DEEPSLEEP  */
        case 9:  return "brownout";      /* ESP_RST_BROWNOUT   */
        case 10: return "sdio";
        case 11: return "usb";
        case 12: return "jtag";
        case 13: return "efuse";
        case 14: return "power_glitch";
        case 15: return "cpu_lockup";
        default: return "invalid";
    }
}

const char *restart_tracker_planned_reason_to_str(restart_tracker_planned_reason_t reason)
{
    switch (reason)
    {
        case RESTART_TRACKER_PLANNED_REASON_NONE:              return "none";
        case RESTART_TRACKER_PLANNED_REASON_USER_REQUEST:      return "user_request";
        case RESTART_TRACKER_PLANNED_REASON_CONFIG_APPLY:      return "config_apply";
        case RESTART_TRACKER_PLANNED_REASON_CONFIG_RECOVERY:   return "config_recovery";
        case RESTART_TRACKER_PLANNED_REASON_OTA_APPLY:         return "ota_apply";
        case RESTART_TRACKER_PLANNED_REASON_FACTORY_RESET:     return "factory_reset";
        case RESTART_TRACKER_PLANNED_REASON_SAFE_MODE:         return "safe_mode";
        case RESTART_TRACKER_PLANNED_REASON_POWER_WAKE:        return "power_wake";
        case RESTART_TRACKER_PLANNED_REASON_INTERNAL_RECOVERY: return "internal_recovery";
        case RESTART_TRACKER_PLANNED_REASON_PERIODIC_WAKE:     return "periodic_wake";
        case RESTART_TRACKER_PLANNED_REASON_PARK_RETRY:        return "park_retry";
        case RESTART_TRACKER_PLANNED_REASON_PARTITION_MIGRATE: return "partition_migrate";
        case RESTART_TRACKER_PLANNED_REASON_OTA_ROLLBACK:      return "ota_rollback";
        default:                                               return "invalid";
    }
}

const char *restart_tracker_source_to_str(restart_tracker_source_t source)
{
    switch (source)
    {
        case RESTART_TRACKER_SOURCE_UNKNOWN:       return "unknown";
        case RESTART_TRACKER_SOURCE_WEB_UI:        return "web_ui";
        case RESTART_TRACKER_SOURCE_CMDLINE:       return "cmdline";
        case RESTART_TRACKER_SOURCE_CONSOLE:       return "console";
        case RESTART_TRACKER_SOURCE_MQTT:          return "mqtt";
        case RESTART_TRACKER_SOURCE_OTA:           return "ota";
        case RESTART_TRACKER_SOURCE_SAFE_MODE:     return "safe_mode";
        case RESTART_TRACKER_SOURCE_CONFIG_SERVER: return "config_server";
        case RESTART_TRACKER_SOURCE_SLEEP_MODE:    return "sleep_mode";
        case RESTART_TRACKER_SOURCE_BUTTON:        return "button";
        case RESTART_TRACKER_SOURCE_PAIRING:       return "pairing";
        case RESTART_TRACKER_SOURCE_PARK:          return "park";
        case RESTART_TRACKER_SOURCE_BOOT:          return "boot";
        default:                                   return "invalid";
    }
}
