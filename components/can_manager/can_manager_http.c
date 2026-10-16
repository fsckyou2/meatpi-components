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
 * @file can_manager_http.c
 * @brief GET /api/can: bus status, the link (listen before talk) and
 *        stats (§9.1 own-routes).
 */
#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "http_server_manager.h"

#include "can_manager.h"

static const char *TAG = "can_manager";

static const char *bus_state_str(uint8_t state)
{
    switch (state)
    {
        case CAN_CORE_BUS_RUNNING:    return "running";
        case CAN_CORE_BUS_OFF:        return "bus_off";
        case CAN_CORE_BUS_RECOVERING: return "recovering";
        default:                        return "stopped";
    }
}

static esp_err_t can_get_handler(httpd_req_t *req)
{
    can_manager_status_t st;
    can_manager_probe_t probe;
    uint32_t probe_age_ms = 0;
    char body[640]; /* the reply goes out in pieces: the frame stays small
                       (the first piece is 500 characters at most) */

    (void)can_manager_status(&st);
    can_manager_last_probe(&probe, &probe_age_ms);

    /* state: where the link is (detecting / listening / mismatch) until
       its verdict, then the bus state as before (running / bus_off /
       recovering); stopped while the bus is down */
    const char *state = "stopped";

    if (st.running)
    {
        state = (strcmp(st.link.state, "running") == 0)
                    ? bus_state_str(st.stats.bus_state) : st.link.state;
    }

    snprintf(body, sizeof(body),
             "{\"enabled\":%s,\"running\":%s,\"silent\":%s,"
             "\"baud_auto\":%s,\"baud_kbps\":%lu,\"baud_detected\":%lu,"
             "\"state\":\"%s\",\"listen_only\":%s,\"verified\":%s,"
             "\"adopted\":%s,"
             "\"tx\":%lu,\"rx\":%lu,\"tx_errors\":%lu,\"rx_errors\":%lu,"
             "\"arb_lost\":%lu,\"bus_errors\":%lu,"
             "\"rx_missed\":%lu,\"rx_overrun\":%lu,"
             "\"tx_done\":%lu,\"tx_retries\":%lu,\"tx_lost\":%lu,"
             "\"dispatch_drops\":%lu,"
             "\"rx_bad\":%lu,\"rx_storms\":%lu,\"rx_deaf\":%lu,\"tx_refused\":%lu,"
             "\"link_switches\":%lu,\"link_demotions\":%lu,"
             "\"bus_off\":%lu,\"recoveries\":%lu,",
             st.enabled ? "true" : "false",
             st.running ? "true" : "false",
             st.silent ? "true" : "false",
             st.baud_auto ? "true" : "false",
             (unsigned long)st.baud_kbps,
             (unsigned long)st.link.detected_kbps,
             state,
             st.link.listen_only ? "true" : "false",
             st.link.verified ? "true" : "false",
             st.link.adopted ? "true" : "false",
             (unsigned long)st.stats.tx_count,
             (unsigned long)st.stats.rx_count,
             (unsigned long)st.stats.tx_errors,
             (unsigned long)st.stats.rx_errors,
             (unsigned long)st.stats.arb_lost,
             (unsigned long)st.stats.bus_errors,
             (unsigned long)st.stats.rx_missed,
             (unsigned long)st.stats.rx_overrun,
             (unsigned long)st.stats.tx_done,
             (unsigned long)st.stats.tx_retries,
             (unsigned long)st.stats.tx_lost,
             (unsigned long)st.stats.dispatch_drops,
             (unsigned long)st.stats.rx_bad,
             (unsigned long)st.stats.rx_storms,
             (unsigned long)st.stats.rx_deaf,
             (unsigned long)st.stats.tx_refused,
             (unsigned long)st.link.switches,
             (unsigned long)st.link.demotions,
             (unsigned long)st.stats.bus_off_count,
             (unsigned long)st.stats.recovery_count);
    httpd_resp_set_type(req, "application/json");

    esp_err_t err = httpd_resp_send_chunk(req, body, HTTPD_RESP_USE_STRLEN);

    if (err == ESP_OK)
    {
        snprintf(body, sizeof(body),
                 "\"err\":{\"stuff\":%lu,\"form\":%lu,\"bit\":%lu,"
                 "\"ack\":%lu,\"other\":%lu},"
                 "\"probe\":{\"result\":\"%s\",\"baud_kbps\":%lu,"
                 "\"frames\":%lu,\"age_ms\":%lu},"
                 "\"subscribers\":[",
                 (unsigned long)st.stats.err_stuff,
                 (unsigned long)st.stats.err_form,
                 (unsigned long)st.stats.err_bit,
                 (unsigned long)st.stats.err_ack,
                 (unsigned long)st.stats.err_other,
                 can_manager_probe_name(probe.result),
                 (unsigned long)probe.baud_kbps,
                 (unsigned long)probe.frames,
                 (unsigned long)probe_age_ms);
        err = httpd_resp_send_chunk(req, body, HTTPD_RESP_USE_STRLEN);
    }

    /* the subscribers follow one by one: who reads the bus, and what each
       one's queue lost (dispatch_drops is their sum) */
    size_t used = 0;
    size_t cap = 0;
    bool first = true;

    can_manager_capacity(&used, &cap);

    for (int i = 0; err == ESP_OK && i < (int)cap; i++)
    {
        can_manager_subscriber_t sub;

        if (!can_manager_subscriber_get(i, &sub))
        {
            continue;
        }

        snprintf(body, sizeof(body),
                 "%s{\"idx\":%d,\"name\":\"%.24s\",\"drops\":%lu}",
                 first ? "" : ",", i, sub.name, (unsigned long)sub.drops);
        first = false;
        err = httpd_resp_send_chunk(req, body, HTTPD_RESP_USE_STRLEN);
    }

    if (err == ESP_OK)
    {
        snprintf(body, sizeof(body), "],\"subscribers_max\":%u}",
                 (unsigned)cap);
        err = httpd_resp_send_chunk(req, body, HTTPD_RESP_USE_STRLEN);
    }

    if (err == ESP_OK)
    {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }

    return err;
}

esp_err_t can_manager_register_http(void)
{
    static const httpd_uri_t URIS[] =
    {
        { .uri = "/api/can", .method = HTTP_GET,
          .handler = can_get_handler },
    };

    esp_err_t err = http_server_manager_register_handlers(
        URIS, sizeof(URIS) / sizeof(URIS[0]));

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "/api/can registered");
    }

    return err;
}
