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
 * @file can_core_link.c
 * @brief The node's link to the bus: applies the listen-before-talk policy
 *        (can_autobaud_core.c) to the TWAI node from the RX task, gates
 *        transmission while the node may not talk, and holds the two
 *        runtime reconfiguration entry points.
 *
 * Why: a node at the wrong bitrate destroys the traffic of the bus it sits
 * on, in normal AND in listen-only mode (bench 2026-10-02, can_core_driver.c).
 * The node therefore starts without its TX pin and gets it only once frames
 * prove the bitrate, or a fixed-bitrate bus stayed silent.
 */
#include <string.h>

#include "can_core.h"
#include "can_core_private.h"

#ifdef ESP_PLATFORM
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "can_core_driver.h"

static const char *TAG = "can_core";

/* guards tx_open + tx_users: transmits come from any task on either core */
static portMUX_TYPE s_tx_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t link_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ---- the transmit gate ------------------------------------------------------ */

/** Once per listen-only episode: why a transmit was refused. A tester on a
 *  gatewayed OBD port with `auto` read the silent refusal as "connected,
 *  no comm to the CAN bus" (2026-10-17); the line names the way out. */
static void link_say_refused(can_core_handle_t *handle)
{
    if (handle->tx_refused_said || handle->config.quiet_link)
    {
        return;
    }

    handle->tx_refused_said = true;

    const can_ab_t *ab = &handle->ab;
    const char *why;

    if (ab->state == CAN_AB_MISMATCH)
    {
        why = "the bus runs at a bitrate this node cannot read";
    }
    else if (ab->cfg.n_candidates > 1)
    {
        why = "no bitrate proven on a silent bus (auto): set the bit rate, "
              "or let the tester state it on connect";
    }
    else if (ab->cfg.want_normal)
    {
        why = "still listening before it talks";
    }
    else
    {
        why = "listen-only by the settings";
    }

    ESP_LOGW(TAG, "transmit refused: the node may not talk, %s (%s at %lu "
                  "kbit/s)", why, can_ab_state_name(ab),
             (unsigned long)can_ab_baud(ab));
}

bool can_link_tx_enter(can_core_handle_t *handle, uint32_t timeout_ms)
{
    uint32_t waited = 0;

    while (true)
    {
        bool in = false;

        taskENTER_CRITICAL(&s_tx_mux);

        if (handle->tx_open)
        {
            handle->tx_users++;
            in = true;
        }

        taskEXIT_CRITICAL(&s_tx_mux);

        if (in)
        {
            return true;
        }

        if (waited >= timeout_ms)
        {
            handle->stats.tx_refused++;
            link_say_refused(handle);
            return false;
        }

        /* a promotion may be a few ms away (the bitrate just proven) */
        vTaskDelay(pdMS_TO_TICKS(5));
        waited += 5;
    }
}

void can_link_tx_exit(can_core_handle_t *handle)
{
    taskENTER_CRITICAL(&s_tx_mux);
    handle->tx_users--;
    taskEXIT_CRITICAL(&s_tx_mux);
}

void can_link_tx_close(can_core_handle_t *handle)
{
    taskENTER_CRITICAL(&s_tx_mux);
    handle->tx_open = false;
    taskEXIT_CRITICAL(&s_tx_mux);

    /* a transmit inside the driver waits for TX queue room at most its
       own timeout; 2 s is a hang */
    for (int i = 0; i < 400 && handle->tx_users > 0; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    if (handle->tx_users > 0)
    {
        ESP_LOGE(TAG, "a transmit did not leave the driver; "
                      "reconfiguring anyway");
    }
}

static void link_tx_open(can_core_handle_t *handle)
{
    taskENTER_CRITICAL(&s_tx_mux);
    handle->tx_open = true;
    taskEXIT_CRITICAL(&s_tx_mux);
    handle->tx_refused_said = false; /* a new episode gets its own line */
}

/* ---- applying what the policy decides -------------------------------------- */

/** Bounce the node to @p baud_kbps / @p listen_only. RX-task context, or a
 *  caller that parked the RX task. The raw RX queue keeps its frames. */
static elm327_err_t link_apply(can_core_handle_t *handle, uint32_t baud_kbps,
                               bool listen_only)
{
    can_link_tx_close(handle);
    can_drv_stop(handle);

    elm327_err_t err = can_drv_start_on_core(handle, baud_kbps, listen_only);

    if (err != ELM327_OK)
    {
        ESP_LOGE(TAG, "node restart failed (%lu kbit/s, %s)",
                 (unsigned long)baud_kbps,
                 listen_only ? "listen-only" : "normal");
        return err;
    }

    if (!listen_only)
    {
        link_tx_open(handle);
    }

    return ELM327_OK;
}

/** The policy's receive-error evidence: the driver's ISR counter (since
 *  boot, never zeroed, internal RAM) plus what the RX line itself said
 *  (link_look_at_the_line). */
static uint32_t link_bad(const can_core_handle_t *handle)
{
    can_drv_counters_t isr;

    can_drv_counters(&isr);
    return isr.rx_bad + handle->deaf;
}

static void link_mark(can_core_handle_t *handle)
{
    can_ab_mark(&handle->ab, link_now_ms(), handle->link_frames,
                link_bad(handle));
}

/* ---- a bus the controller is deaf to ---------------------------------------- */

#define LINK_LINE_PERIOD_MS 20u /* how often the RX line is looked at         */
#define LINK_LINE_SAMPLES   32  /* ... this many times, 3 us apart: 100 us,
                                   a whole frame at 1 Mbit/s                  */

/** Dominant on the RX line right now? (RXD is low while the bus is.) */
static bool link_line_busy(int rx_gpio)
{
    for (int i = 0; i < LINK_LINE_SAMPLES; i++)
    {
        if (gpio_get_level((gpio_num_t)rx_gpio) == 0)
        {
            return true;
        }

        esp_rom_delay_us(3);
    }

    return false;
}

/**
 * A listening controller reports a busy bus it cannot read as receive
 * errors, unless the bus is saturated at a HIGHER bitrate: then it never
 * sees the idle time it waits for after its first error and reports nothing
 * at all. Bench 2026-10-03: at 250 kbit/s under an 800 kbit/s flood, no
 * frame and no error in 14 s. To the policy that is a silent bus: the
 * candidate walk of auto stopped there, a probe would have said "silent",
 * and a bus guard would have let the OBD chip talk.
 *
 * So while the node listens, and only when the controller has had nothing
 * to say since the last look, the RX line is sampled directly. Busy = one
 * helping of mismatch evidence (the policy wants two, 10 ms apart, with no
 * frame between them: a frame that was merely in flight during a look ends
 * and clears the first).
 */
static void link_look_at_the_line(can_core_handle_t *handle, uint32_t now_ms)
{
    if (!handle->node_listen ||
        now_ms - handle->line_ms < LINK_LINE_PERIOD_MS)
    {
        return;
    }

    can_drv_counters_t isr;

    can_drv_counters(&isr);

    bool nothing_said = (handle->link_frames == handle->line_frames) &&
                        (isr.rx_bad == handle->line_bad);

    handle->line_ms = now_ms;
    handle->line_frames = handle->link_frames;
    handle->line_bad = isr.rx_bad;

    if (nothing_said && link_line_busy(handle->config.rx_gpio))
    {
        handle->deaf += CAN_AB_BAD_MIN;
        handle->stats.rx_deaf++;
    }
}

static void link_cfg(const can_core_config_t *config, can_ab_cfg_t *out)
{
    memset(out, 0, sizeof(*out));
    out->want_normal = !config->silent_mode;
    out->retry_ms = config->link_retry_ms;

    if (config->n_baud_candidates > 1)
    {
        uint8_t n = config->n_baud_candidates;

        if (n > CAN_AB_MAX_CANDIDATES)
        {
            n = CAN_AB_MAX_CANDIDATES;
        }

        memcpy(out->candidates, config->baud_candidates,
               n * sizeof(out->candidates[0]));
        out->n_candidates = n;
    }
    else
    {
        out->candidates[0] = (uint16_t)config->baud_kbps;
        out->n_candidates = 1;
    }
}

/** One line per verdict: what the link is now and why. */
static void link_log(const can_core_handle_t *handle)
{
    const can_ab_t *ab = &handle->ab;
    unsigned long baud = (unsigned long)can_ab_baud(ab);

    if (handle->config.quiet_link)
    {
        /* an internal listener (a bus guard's watch): its verdicts are the
           guard's to report, and what it hears may be the OBD chip's own
           unanswered requests */
        ESP_LOGD(TAG, "listener: %s at %lu kbit/s", can_ab_state_name(ab),
                 baud);
        return;
    }

    switch (ab->state)
    {
    case CAN_AB_RUNNING:
        ESP_LOGI(TAG, "link up: %lu kbit/s, %s (%s)", baud,
                 ab->normal ? "normal" : "listen-only",
                 ab->verified ? "bitrate proven by frames"
                 : ab->adopted ? "silent bus, bitrate stated by the tester"
                               : "silent bus, bitrate as configured");
        break;

    case CAN_AB_MISMATCH:
        ESP_LOGW(TAG, "link: the bus carries traffic that %s; listening "
                      "only, nothing is transmitted",
                 (ab->cfg.n_candidates > 1)
                     ? "none of the candidate bitrates can read"
                     : "the configured bitrate cannot read");
        break;

    default:
        ESP_LOGI(TAG, "link: listening at %lu kbit/s", baud);
        break;
    }
}

elm327_err_t can_link_start(can_core_handle_t *handle)
{
    can_ab_cfg_t cfg;

    link_cfg(&handle->config, &cfg);
    handle->tx_open = false;
    handle->tx_users = 0;

    handle->line_ms = link_now_ms();
    handle->line_frames = handle->link_frames;

    can_ab_action_t a = can_ab_init(&handle->ab, &cfg, link_now_ms(),
                                    handle->link_frames, link_bad(handle));

    /* always listen-only, always without the TX pin */
    return can_drv_start_on_core(handle, a.baud_kbps, a.listen_only);
}

void can_link_service(can_core_handle_t *handle)
{
    can_ab_state_t before = handle->ab.state;
    uint32_t now_ms = link_now_ms();

    link_look_at_the_line(handle, now_ms);

    can_ab_action_t a = can_ab_step(&handle->ab, now_ms,
                                    handle->link_frames, link_bad(handle));

    if (a.apply)
    {
        (void)link_apply(handle, a.baud_kbps, a.listen_only);
        link_mark(handle);
    }

    /* verdicts are logged, the candidate walk of auto is not (it can go
       on for as long as an unreadable bus is alive) */
    if (handle->ab.state != before)
    {
        link_log(handle);
    }
}

uint32_t can_link_poll_ms(const can_core_handle_t *handle)
{
    return (handle->ab.state == CAN_AB_RUNNING) ? CAN_AB_WATCH_MS : 25u;
}

void can_core_get_link(can_core_handle_t *handle, can_core_link_t *out)
{
    if (!handle || !out)
    {
        return;
    }

    const can_ab_t *ab = &handle->ab;

    memset(out, 0, sizeof(*out));
    out->state = can_ab_state_name(ab);
    out->baud_kbps = can_ab_baud(ab);
    out->detected_kbps = ab->detected_kbps;
    out->autobaud = ab->cfg.n_candidates > 1;
    out->listen_only = !ab->normal;
    out->verified = ab->verified;
    out->adopted = ab->adopted;
    out->switches = ab->switches;
    out->demotions = ab->demotions;
}

/* ---- runtime reconfiguration (another task: the RX task is parked) ----------- */

elm327_err_t can_core_adopt_baud(can_core_handle_t *handle,
                                 uint32_t baud_kbps, bool *adopted)
{
    if (adopted != NULL)
    {
        *adopted = false;
    }

    if (!handle || !handle->initialised)
    {
        return ELM327_ERR_NOT_INIT;
    }

    if (baud_kbps == 0 || baud_kbps > 0xFFFFu)
    {
        return ELM327_OK; /* not a bitrate: the policy would refuse it too */
    }

    handle->reconfiguring = true;
    can_core_rx_wait_parked(handle);

    bool taken = false;
    can_ab_action_t a = can_ab_adopt(&handle->ab, (uint16_t)baud_kbps,
                                     link_now_ms(), handle->link_frames,
                                     link_bad(handle), &taken);
    elm327_err_t err = ELM327_OK;

    if (a.apply)
    {
        err = link_apply(handle, a.baud_kbps, a.listen_only);
        link_mark(handle);
    }

    if (taken)
    {
        link_log(handle);
    }

    handle->reconfiguring = false;

    if (adopted != NULL)
    {
        *adopted = taken && (err == ELM327_OK);
    }

    return err;
}

elm327_err_t can_core_set_silent_mode(can_core_handle_t *handle,
                                        bool silent_mode)
{
    if (!handle || !handle->initialised)
    {
        return ELM327_ERR_NOT_INIT;
    }

    if (handle->config.silent_mode == silent_mode)
    {
        return ELM327_OK;
    }

    handle->config.silent_mode = silent_mode;
    handle->reconfiguring = true;
    can_core_rx_wait_parked(handle);

    /* to listen-only: at once. To normal: at once when the verdict is in,
       otherwise the policy promotes the node when it may talk. */
    can_ab_action_t a = can_ab_want_normal(&handle->ab, !silent_mode,
                                           link_now_ms(),
                                           handle->link_frames,
                                           link_bad(handle));
    elm327_err_t err = ELM327_OK;

    if (a.apply)
    {
        err = link_apply(handle, a.baud_kbps, a.listen_only);
        link_mark(handle);
    }

    handle->reconfiguring = false;
    return err;
}

elm327_err_t can_core_set_baud(can_core_handle_t *handle,
                                  uint32_t baud_kbps)
{
    if (!handle || !handle->initialised)
    {
        return ELM327_ERR_NOT_INIT;
    }

    handle->config.baud_kbps = baud_kbps;
    handle->config.n_baud_candidates = 0; /* an explicit bitrate is fixed */
    handle->reconfiguring = true;
    can_core_rx_wait_parked(handle);

    /* the policy starts over: listen-only at the new bitrate */
    can_link_tx_close(handle);
    can_drv_stop(handle);

    elm327_err_t err = can_link_start(handle);

    handle->reconfiguring = false;
    return err;
}

#else /* host stub: no node, nothing to gate */

void can_core_get_link(can_core_handle_t *handle, can_core_link_t *out)
{
    if (!handle || !out)
    {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->state = handle->initialised ? "running" : "listening";
    out->baud_kbps = handle->config.baud_kbps;
}

elm327_err_t can_core_set_silent_mode(can_core_handle_t *handle,
                                        bool silent_mode)
{
    if (!handle || !handle->initialised)
    {
        return ELM327_ERR_NOT_INIT;
    }

    handle->config.silent_mode = silent_mode;
    return ELM327_OK;
}

elm327_err_t can_core_set_baud(can_core_handle_t *handle,
                                  uint32_t baud_kbps)
{
    if (!handle || !handle->initialised)
    {
        return ELM327_ERR_NOT_INIT;
    }

    handle->config.baud_kbps = baud_kbps;
    return ELM327_OK;
}

elm327_err_t can_core_adopt_baud(can_core_handle_t *handle,
                                 uint32_t baud_kbps, bool *adopted)
{
    (void)baud_kbps;

    if (adopted != NULL)
    {
        *adopted = false;
    }

    return (handle && handle->initialised) ? ELM327_OK : ELM327_ERR_NOT_INIT;
}

#endif /* ESP_PLATFORM */
