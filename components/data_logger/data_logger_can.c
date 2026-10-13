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
 * @file data_logger_can.c
 * @brief The CAN frames stream's front end: a static queue subscribed
 *        into can_manager's RX fan-out (drop-oldest) and a small drain
 *        task that tags frames into the frame ring. The writer task
 *        does all storage: this file never touches a file.
 *
 * The drain task's stack is PSRAM (it only moves queue → ring, no
 * flash, no SD: §2 corollary doesn't apply). Subscription retries
 * lazily: can_manager may be disabled, or the bus may come up after
 * data_logger_start() (subscribe requires a running bus).
 *
 * Filter settings (`can_filter`/`can_mask`/`can_ext`) map 1:1 onto
 * can_manager_subscribe_queue(); empty can_filter = monitor_all.
 */
#include <string.h>
#include <sys/time.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "can_manager.h"

#include "data_logger_private.h"

static const char *TAG = "data_logger";

#define DL_CAN_Q_LEN 256

static dl_can_cfg_t s_cfg;

static QueueHandle_t s_q;
static StaticQueue_t s_q_buf;          /* internal: FreeRTOS object   */
static uint8_t s_q_store[DL_CAN_Q_LEN * sizeof(can_core_frame_t)]
    EXT_RAM_BSS_ATTR;

static TaskHandle_t s_task;
static StaticTask_t s_tcb;             /* internal: FreeRTOS object   */
static StackType_t s_stack[3072] EXT_RAM_BSS_ATTR;

static int s_sub_idx = -1;

void dl_can_configure(const dl_can_cfg_t *cfg)
{
    s_cfg = *cfg;
}

uint32_t dl_can_queued(void)
{
    return (s_q != NULL) ? (uint32_t)uxQueueMessagesWaiting(s_q) : 0;
}

static void frame_to_record(const can_core_frame_t *f,
                            dl_record_t *rec)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    memset(rec, 0, sizeof(*rec));
    rec->ts_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    rec->mono_ms = (uint32_t)(esp_timer_get_time() / 1000);
    rec->kind = DL_REC_FRAME;
    rec->u.f.id = f->id;
    rec->u.f.dlc = (f->dlc <= 8) ? f->dlc : 8;
    rec->u.f.flags = (uint8_t)((f->ext ? DL_FRAME_EXT : 0) |
                               (f->rtr ? DL_FRAME_RTR : 0));
    memcpy(rec->u.f.data, f->data, rec->u.f.dlc);
}

static void drain_task(void *arg)
{
    (void)arg;

    bool logged_wait = false;

    while (true)
    {
        if (s_sub_idx < 0)
        {
            esp_err_t err = can_manager_subscribe_queue(
                s_q, s_cfg.filter, s_cfg.mask, s_cfg.ext,
                s_cfg.monitor_all, &s_sub_idx);

            if (err != ESP_OK)
            {
                if (!logged_wait)
                {
                    ESP_LOGI(TAG, "CAN log waiting for the bus "
                             "(can_manager enabled?)");
                    logged_wait = true;
                }

                s_sub_idx = -1;
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }

            (void)can_manager_subscriber_name(s_sub_idx, "logger");
            ESP_LOGI(TAG, "CAN log subscribed (%s)",
                     s_cfg.monitor_all ? "all ids" : "filtered");
        }

        can_core_frame_t f;

        if (xQueueReceive(s_q, &f, pdMS_TO_TICKS(500)) == pdTRUE)
        {
            dl_record_t rec;

            frame_to_record(&f, &rec);
            (void)dl_frame_push(&rec);
        }
    }
}

esp_err_t dl_can_start(void)
{
    if (s_q == NULL)
    {
        s_q = xQueueCreateStatic(DL_CAN_Q_LEN,
                                 sizeof(can_core_frame_t),
                                 s_q_store, &s_q_buf);
    }

    if (s_q == NULL)
    {
        return ESP_FAIL;
    }

    if (s_task == NULL)
    {
        s_task = xTaskCreateStatic(drain_task, "dl_can",
                                   sizeof(s_stack) / sizeof(s_stack[0]),
                                   NULL, 3, s_stack, &s_tcb);
    }

    return (s_task != NULL) ? ESP_OK : ESP_FAIL;
}

/* synthetic frames straight into the ring: engine benchmarks without
 * a bus (CLI `logger frametest <n>`, bench harness) */
esp_err_t dl_can_test_push(int n)
{
    dl_record_t rec;
    struct timeval tv;
    int queued = 0;

    for (int i = 0; i < n; i++)
    {
        gettimeofday(&tv, NULL);
        memset(&rec, 0, sizeof(rec));
        rec.ts_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
        rec.mono_ms = (uint32_t)(esp_timer_get_time() / 1000);
        rec.kind = DL_REC_FRAME;
        rec.u.f.id = 0x100u + (uint32_t)(i & 0x0F);
        rec.u.f.dlc = 8;
        rec.u.f.data[0] = (uint8_t)i;
        rec.u.f.data[1] = (uint8_t)(i >> 8);
        rec.u.f.data[2] = (uint8_t)(i >> 16);
        rec.u.f.data[3] = (uint8_t)(i >> 24);
        rec.u.f.data[4] = 0xAB;
        rec.u.f.data[5] = 0xCD;
        rec.u.f.data[6] = 0xEF;
        rec.u.f.data[7] = (uint8_t)(i ^ 0xFF);

        if (dl_frame_push(&rec) == ESP_OK)
        {
            queued++;
        }
    }

    return (queued == n) ? ESP_OK : ESP_ERR_INVALID_STATE;
}
