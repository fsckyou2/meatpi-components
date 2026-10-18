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
 * @file ota_pipe.c
 * @brief The pipe (2026-10-18): a producer on a PSRAM stack (fw_update's
 *        download task, whose TLS handshake wants the big stack) hands
 *        image bytes to a small INTERNAL-stack writer that exists only for
 *        the session. Every esp_ota call ends in a cache-off flash write
 *        that asserts the caller's stack is internal (standard §2), so the
 *        writer runs begin, every write, end and abort; the producer only
 *        copies into two 4 KB PSRAM slots and waits when both are full.
 *        The writer's stack comes from the internal heap at begin and goes
 *        back at end or abort: nothing permanent (internal/TASK_internal_ram.md).
 */
#include "ota_manager_private.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ota_manager";

#define PIPE_SLOTS       2
#define PIPE_SLOT_LEN    4096U
#define PIPE_WRITER_STACK 5120  /* internal heap, session-scoped        */
#define PIPE_WRITER_PRIO  6
#define PIPE_MSG_END     (-1)
#define PIPE_MSG_ABORT   (-2)

static uint8_t s_slot[PIPE_SLOTS][PIPE_SLOT_LEN] EXT_RAM_BSS_ATTR;
static size_t  s_slot_len[PIPE_SLOTS];

static QueueHandle_t s_free;   /* slot indices the producer may fill    */
static QueueHandle_t s_full;   /* filled slot indices, or an END/ABORT  */
static SemaphoreHandle_t s_done; /* the writer's last word              */
static StaticQueue_t s_free_buf;
static StaticQueue_t s_full_buf;
static StaticSemaphore_t s_done_buf;
static uint8_t s_free_store[PIPE_SLOTS * sizeof(int)];
static uint8_t s_full_store[(PIPE_SLOTS + 1) * sizeof(int)];

static volatile bool s_active;
static volatile esp_err_t s_writer_err;   /* the first failure of the writer */
static volatile esp_err_t s_end_err;      /* the result of end()             */
static size_t s_total;
static bool s_have_sha;
static uint8_t s_sha[32];

static void queues_init(void)
{
    if (s_free == NULL)
    {
        s_free = xQueueCreateStatic(PIPE_SLOTS, sizeof(int), s_free_store,
                                    &s_free_buf);
        s_full = xQueueCreateStatic(PIPE_SLOTS + 1, sizeof(int), s_full_store,
                                    &s_full_buf);
        s_done = xSemaphoreCreateBinaryStatic(&s_done_buf);
    }
}

static void queues_reset(void)
{
    xQueueReset(s_free);
    xQueueReset(s_full);
    (void)xSemaphoreTake(s_done, 0);

    for (int i = 0; i < PIPE_SLOTS; i++)
    {
        xQueueSend(s_free, &i, 0);
    }
}

/** The writer: begins the session, drains the slots, ends or aborts. */
static void writer_task(void *arg)
{
    (void)arg;
    s_writer_err = ota_manager_begin(s_total);

    if (s_writer_err != ESP_OK && s_have_sha)
    {
        (void)ota_manager_expect_sha256(NULL); /* not consumed: disarm */
    }

    xSemaphoreGive(s_done); /* begin's result is in */

    bool run = true;

    while (run)
    {
        int msg;

        if (xQueueReceive(s_full, &msg, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        if (msg == PIPE_MSG_END)
        {
            s_end_err = (s_writer_err == ESP_OK) ? ota_manager_end()
                                                 : s_writer_err;
            run = false;
        }
        else if (msg == PIPE_MSG_ABORT)
        {
            (void)ota_manager_abort();
            s_end_err = ESP_ERR_INVALID_STATE;
            run = false;
        }
        else
        {
            if (s_writer_err == ESP_OK)
            {
                s_writer_err = ota_manager_write(s_slot[msg], s_slot_len[msg]);
            }

            xQueueSend(s_free, &msg, 0); /* drained even after a failure */
        }
    }

    /* standard §2: ephemeral tasks log their watermark for the audit */
    ESP_LOGI(TAG, "ota_writer done: stack headroom %u B",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    s_active = false;
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

esp_err_t ota_pipe_begin(size_t total_size, const uint8_t *expected_sha256)
{
    if (s_active)
    {
        return ESP_ERR_INVALID_STATE;
    }

    queues_init();
    queues_reset();
    s_total = total_size;
    s_have_sha = expected_sha256 != NULL;

    if (s_have_sha)
    {
        memcpy(s_sha, expected_sha256, sizeof(s_sha));
        (void)ota_manager_expect_sha256(s_sha); /* for the writer's begin */
    }

    s_writer_err = ESP_OK;
    s_end_err = ESP_OK;
    s_active = true;

    /* internal: the writer's stack must survive the cache-off flash writes */
    if (xTaskCreate(writer_task, "ota_writer", PIPE_WRITER_STACK, NULL,
                    PIPE_WRITER_PRIO, NULL) != pdPASS)
    {
        s_active = false;
        (void)ota_manager_expect_sha256(NULL);
        ESP_LOGE(TAG, "no internal RAM for the OTA writer");
        return ESP_ERR_NO_MEM;
    }

    /* the begin runs on the writer: wait for its word (an erase of the
       slot can take seconds) */
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(30000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "the OTA writer did not open the session");
        return ESP_ERR_TIMEOUT;
    }

    if (s_writer_err != ESP_OK)
    {
        int msg = PIPE_MSG_END; /* let the writer finish and go */

        xQueueSend(s_full, &msg, portMAX_DELAY);
        (void)xSemaphoreTake(s_done, pdMS_TO_TICKS(5000));
        return s_writer_err;
    }

    return ESP_OK;
}

esp_err_t ota_pipe_write(const uint8_t *data, size_t len, uint32_t wait_ms)
{
    if (!s_active)
    {
        return ESP_ERR_INVALID_STATE;
    }

    while (len > 0)
    {
        int slot;

        if (s_writer_err != ESP_OK)
        {
            return s_writer_err;
        }

        if (xQueueReceive(s_free, &slot, pdMS_TO_TICKS(wait_ms)) != pdTRUE)
        {
            return ESP_ERR_TIMEOUT;
        }

        size_t n = (len > PIPE_SLOT_LEN) ? PIPE_SLOT_LEN : len;

        memcpy(s_slot[slot], data, n);
        s_slot_len[slot] = n;
        xQueueSend(s_full, &slot, portMAX_DELAY);
        data += n;
        len -= n;
    }

    return ESP_OK;
}

esp_err_t ota_pipe_end(uint32_t wait_ms)
{
    int msg = PIPE_MSG_END;

    if (!s_active)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xQueueSend(s_full, &msg, portMAX_DELAY);

    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(wait_ms)) != pdTRUE)
    {
        ESP_LOGE(TAG, "the OTA writer did not finish within %u ms",
                 (unsigned)wait_ms);
        return ESP_ERR_TIMEOUT;
    }

    return s_end_err;
}

esp_err_t ota_pipe_abort(void)
{
    int msg = PIPE_MSG_ABORT;

    if (!s_active)
    {
        return ESP_OK;
    }

    xQueueSend(s_full, &msg, portMAX_DELAY);
    (void)xSemaphoreTake(s_done, pdMS_TO_TICKS(10000));
    return ESP_OK;
}

bool ota_pipe_active(void)
{
    return s_active;
}
