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
 * @file restart_tracker.c
 * @brief Target glue over the pure core: PSRAM `.noinit` state placement,
 *        spinlock, cache write-back (so the data actually reaches PSRAM
 *        before a reset), and the esp_* input sources. Settings live in
 *        restart_tracker_settings.c (standard §4.1).
 */
#include "restart_tracker.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

#include "log_manager.h"

#include "restart_tracker_private.h"

static const char *TAG = "restart_tracker";

/* Survives warm resets; random after power-on: rt_state_is_valid decides. */
static EXT_RAM_NOINIT_ATTR restart_tracker_state_t s_state;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED; /* internal: spinlock */
static bool s_recorded;

/* The crash-loop brake: this boot's verdict (the count itself is in RTC
   memory, restart_tracker_crash.c). */
static restart_tracker_brake_t s_brake EXT_RAM_BSS_ATTR;
static bool s_settled;

/* the product's own park retry: off unless the build turns it on (Kconfig) */
#define RT_PARK_RETRY_S ((uint32_t)CONFIG_WICAN_RESTART_TRACKER_PARK_RETRY_MIN * 60U)

static rt_inputs_t gather_inputs(void)
{
    rt_inputs_t in =
    {
        .now_unix = (int64_t)time(NULL),
        .uptime_ms = (uint64_t)(esp_timer_get_time() / 1000LL),
        .reset_reason = (uint32_t)esp_reset_reason(),
    };

    return in;
}

/** A boot that follows a crash says so: what and where, then the PCs as
 *  addr2line takes them (tools/crash_decode.py). W, not E: the boot's error
 *  count and the boot_errors fault are not this line's business. */
static void log_crash(const rt_crash_note_t *note)
{
    /* PSRAM: the boot task only, once per boot */
    static restart_tracker_crash_t crash EXT_RAM_BSS_ATTR;
    static char line[224] EXT_RAM_BSS_ATTR;

    rt_crash_export(note, &crash);
    restart_tracker_crash_summary(&crash, line, sizeof(line));
    ESP_LOGW(TAG, "previous run crashed: %s", line);

    size_t used = 0;

    line[0] = '\0';

    for (int i = 0; i < crash.bt_len && used + 12U < sizeof(line); i++)
    {
        used += (size_t)snprintf(line + used, sizeof(line) - used,
                                 "%s0x%08" PRIx32, (i > 0) ? " " : "",
                                 crash.bt[i]);
    }

    if (crash.bt_len > 0)
    {
        ESP_LOGW(TAG, "backtrace: %s%s", line,
                 crash.bt_corrupt ? " (corrupt)"
                                  : (crash.bt_more ? " ..." : ""));
    }

    used = 0;

    for (int i = 0; i < crash.bt2_len && used + 12U < sizeof(line); i++)
    {
        used += (size_t)snprintf(line + used, sizeof(line) - used,
                                 "%s0x%08" PRIx32, (i > 0) ? " " : "",
                                 crash.bt2[i]);
    }

    if (crash.bt2_len > 0)
    {
        ESP_LOGW(TAG, "core %u meanwhile: %s", (unsigned)crash.bt2_core,
                 line);
    }
}

/** Push the cached view out to PSRAM so it survives the next reset. */
static esp_err_t commit(void)
{
    return esp_cache_msync(&s_state, sizeof(s_state),
                           ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                           ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

esp_err_t restart_tracker_init(void)
{
    if (s_recorded)
    {
        return ESP_OK; /* one record per boot */
    }

    static const log_descriptor_t LOG_DESC =
        { "restart_tracker", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC); /* per-TAG level control (§9.2) */

    rt_inputs_t in = gather_inputs();

    /* forensics for an invalid previous state: distinguishes "memory wiped"
       (magic gone) from "write-back never landed" (CRC-only mismatch) */
    if (!rt_state_is_valid(&s_state))
    {
        ESP_LOGW(TAG, "prev state invalid: magic=%08lx ver=%lu len=%lu "
                      "crc=%08lx want=%08lx",
                 (unsigned long)s_state.magic, (unsigned long)s_state.version,
                 (unsigned long)s_state.history_len,
                 (unsigned long)s_state.crc32,
                 (unsigned long)rt_crc32(&s_state));
    }

    portENTER_CRITICAL(&s_lock);
    bool fresh = rt_record_boot(&s_state, &in);
    esp_err_t err = commit();
    uint32_t slot = s_state.latest_history_index;
    restart_tracker_record_t rec = s_state.history[slot];
    uint32_t boots = s_state.boot_count;
    uint32_t unexpected = s_state.unexpected_reset_count;
    portEXIT_CRITICAL(&s_lock);

    /* the crash note of the run that just ended, when it left one: filed
       under this boot's record. RTC memory, so outside the lock (nothing
       else writes it until the next panic) and with no cache sync. */
    const rt_crash_note_t *note =
        rt_crash_boot_collect(rec.sequence, slot, fresh);

    /* the crash-loop brake: how the run before ended, and what this boot
       may do. A crash that a restart request had announced is that
       restart going wrong, not a loop. */
    rt_brake_state_t *count = rt_crash_brake_state();

    memset(&s_brake, 0, sizeof(s_brake));

    if (count != NULL)
    {
        bool crash_reset = rec.was_planned == 0U &&
                           rt_reset_reason_is_crash(rec.actual_reset_reason);

        portENTER_CRITICAL(&s_lock);
        rt_brake_boot(count, crash_reset, RT_PARK_RETRY_S, &s_brake);
        portEXIT_CRITICAL(&s_lock);
    }

    s_recorded = true;

    ESP_LOGI(TAG, "boot #%lu (%s%s): reset=%s planned=%s/%s, unexpected=%lu",
             (unsigned long)boots,
             fresh ? "fresh state" : "history kept",
             rec.time_valid ? ", time ok" : "",
             restart_tracker_reset_reason_to_str(rec.actual_reset_reason),
             restart_tracker_planned_reason_to_str(
                 (restart_tracker_planned_reason_t)rec.planned_reason),
             restart_tracker_source_to_str(
                 (restart_tracker_source_t)rec.source),
             (unsigned long)unexpected);

    if (note != NULL)
    {
        log_crash(note);
    }

    /* the stored report: NVS is read, and written when this crash is news
       (restart_tracker_report.c). Before the verdict is obeyed: a device
       that parks now must already have its report on flash. */
    rt_report_boot(note, &rec, &s_brake);

    if (s_brake.verdict == RESTART_TRACKER_BOOT_PARK)
    {
        ESP_LOGW(TAG, "crash-loop brake: %u runs in a row crashed before "
                 "they settled; this boot parks", (unsigned)s_brake.streak);
    }
    else if (s_brake.verdict == RESTART_TRACKER_BOOT_PARK_BARE)
    {
        ESP_LOGW(TAG, "crash-loop brake: the park itself crashed; parking "
                 "without the LED");
    }
    else if (s_brake.streak >= RESTART_TRACKER_BRAKE_STREAK)
    {
        ESP_LOGW(TAG, "crash-loop brake: one try after a park (%u quick "
                 "crashes so far)", (unsigned)s_brake.streak);
    }
    else if (s_brake.streak > 0U)
    {
        ESP_LOGW(TAG, "crash-loop brake: %u of %u quick crashes in a row",
                 (unsigned)s_brake.streak,
                 (unsigned)RESTART_TRACKER_BRAKE_STREAK);
    }

    return err;
}

esp_err_t restart_tracker_start(void)
{
    return s_recorded ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t restart_tracker_stop(void)
{
    return ESP_OK; /* passive: nothing to stop */
}

static restart_tracker_pre_restart_fn s_pre_restart;

esp_err_t restart_tracker_set_pre_restart_hook(restart_tracker_pre_restart_fn fn)
{
    s_pre_restart = fn;
    return ESP_OK;
}

esp_err_t restart_tracker_mark_planned_restart(restart_tracker_planned_reason_t reason,
                                               restart_tracker_source_t source,
                                               uint32_t flags)
{
    if (s_pre_restart != NULL)
    {
        s_pre_restart(reason, source, flags); /* outside the spinlock */
    }

    rt_inputs_t in = gather_inputs();
    rt_brake_state_t *count = rt_crash_brake_state();

    portENTER_CRITICAL(&s_lock);
    rt_mark_planned(&s_state, &in, reason, source, flags);

    if (reason == RESTART_TRACKER_PLANNED_REASON_PARK_RETRY && count != NULL)
    {
        /* the next boot is the park's one try: the streak must stand */
        rt_brake_mark_retry(count);
    }

    esp_err_t err = commit();
    portEXIT_CRITICAL(&s_lock);

    return err;
}

void restart_tracker_restart(restart_tracker_planned_reason_t reason,
                             restart_tracker_source_t source, uint32_t flags)
{
    restart_tracker_mark_planned_restart(reason, source, flags);
    ESP_LOGI(TAG, "restarting: %s/%s",
             restart_tracker_planned_reason_to_str(reason),
             restart_tracker_source_to_str(source));
    esp_restart();
}

/* ---- the crash-loop brake ------------------------------------------------------- */

esp_err_t restart_tracker_get_brake(restart_tracker_brake_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_recorded)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const rt_brake_state_t *count = rt_crash_brake_state();

    /* the verdict and the retry time are this boot's; the count is live */
    *out = s_brake;

    if (count != NULL)
    {
        portENTER_CRITICAL(&s_lock);
        out->streak = count->streak;
        out->parks = count->parks;
        out->report_budget = count->budget;
        out->settled = (count->run_flags & RT_BRAKE_RF_SETTLED) != 0U;
        portEXIT_CRITICAL(&s_lock);
    }

    return ESP_OK;
}

esp_err_t restart_tracker_set_boot_mode(restart_tracker_boot_mode_t mode)
{
    if ((unsigned)mode > (unsigned)RESTART_TRACKER_BOOT_SAFE)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_recorded)
    {
        return ESP_ERR_INVALID_STATE;
    }

    rt_brake_state_t *count = rt_crash_brake_state();

    portENTER_CRITICAL(&s_lock);

    if (count != NULL)
    {
        rt_brake_set_mode(count, (uint8_t)mode);
    }

    rt_record_set_mode(&s_state, (uint8_t)mode);

    esp_err_t err = commit();

    portEXIT_CRITICAL(&s_lock);
    return err;
}

bool restart_tracker_settle(bool force)
{
    if (!s_recorded)
    {
        return false;
    }

    if (s_settled)
    {
        return true;
    }

    /* esp_timer: it counts through light sleep, the tick does not */
    if (!force && esp_timer_get_time() <
                      (int64_t)RESTART_TRACKER_SETTLE_S * 1000000LL)
    {
        return false;
    }

    rt_brake_state_t *count = rt_crash_brake_state();

    portENTER_CRITICAL(&s_lock);

    if (count != NULL)
    {
        rt_brake_settle(count);
    }

    rt_record_set_settled(&s_state);
    (void)commit();
    s_settled = true;
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "run settled (%s): a crash from here is not part of a loop",
             force ? "marked by hand" : "up long enough");
    return true;
}

esp_err_t restart_tracker_set_test_retry(uint16_t seconds)
{
    if (seconds != 0U && (seconds < 10U || seconds > 3600U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    rt_brake_state_t *count = rt_crash_brake_state();

    if (count == NULL)
    {
        return s_recorded ? ESP_ERR_NOT_SUPPORTED : ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_lock);
    rt_brake_set_test_retry(count, seconds);
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t restart_tracker_get_state(restart_tracker_state_t *out_state)
{
    if (out_state == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);

    if (!rt_state_is_valid(&s_state))
    {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    *out_state = s_state;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t restart_tracker_get_latest_record(restart_tracker_record_t *out_record)
{
    if (out_record == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);

    if (!rt_state_is_valid(&s_state) || s_state.boot_count == 0U)
    {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    *out_record = s_state.history[s_state.latest_history_index %
                                  RESTART_TRACKER_HISTORY_LEN];
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}
