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
 * @file usb_host_manager.c
 * @brief Lifecycle, the presence task, and the role mux
 *        (see include/usb_host_manager.h for the model). Settings live
 *        in usb_host_manager_settings.c (standard §4.1).
 */
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dev_status_manager.h"
#include "log_manager.h"
#include "usb_dc.h"
#include "usb_eth_host.h"
#include "usb_net_device.h"
#include "usb_cdc_device.h"

#include "usb_host_manager_private.h"

static const char *TAG = "usb_host_manager";

#define UHM_ID_GPIO       CONFIG_WICAN_USB_ID_GPIO
#define UHM_MODE_GPIO     CONFIG_WICAN_USB_MODE_GPIO
#define UHM_VBUS_GPIO     CONFIG_WICAN_SLEEP_USB_PWR_GPIO /* shared rail */
#define UHM_POLL_MS       200
#define UHM_VBUS_DELAY_MS 250

static usb_host_manager_status_t s_status;
static uhm_presence_t s_presence;
static volatile bool s_run;
static TaskHandle_t s_task;
static StaticTask_t s_tcb;                    /* internal: FreeRTOS */
static StackType_t s_stack[3072] EXT_RAM_BSS_ATTR;

void uhm_status_set_enabled(bool enabled)
{
    s_status.enabled = enabled;
}

/* ---- usb_eth_host callbacks (esp_event task context) ---------------------- */

static void on_eth_ip_up(void)
{
    usb_eth_host_driver_t drv;
    char ifkey[8];
    esp_netif_t *netif = NULL;
    esp_netif_ip_info_t info = { 0 };

    s_status.eth_connected = true;
    snprintf(s_status.driver, sizeof(s_status.driver), "%s",
             usb_eth_host_get_active_driver(&drv)
                 ? usb_eth_host_driver_to_str(drv) : "?");

    if (usb_eth_host_get_active_ifkey(ifkey, sizeof(ifkey)))
    {
        netif = esp_netif_get_handle_from_ifkey(ifkey);
    }

    if (netif != NULL && esp_netif_get_ip_info(netif, &info) == ESP_OK)
    {
        snprintf(s_status.ip, sizeof(s_status.ip), IPSTR,
                 IP2STR(&info.ip));
    }

    if (!usb_eth_host_get_active_device_ids(&s_status.vid, &s_status.pid))
    {
        s_status.vid = 0;
        s_status.pid = 0;
    }

    dev_status_manager_set(DEV_STATUS_BIT_ETH_CONNECTED);
    uhm_events_eth(true, s_status.ip);
    ESP_LOGI(TAG, "wired uplink up: %s via %s (%04x:%04x)", s_status.ip,
             s_status.driver, s_status.vid, s_status.pid);
}

static void on_eth_ip_lost(void)
{
    s_status.eth_connected = false;
    s_status.ip[0] = '\0';
    s_status.vid = 0;
    s_status.pid = 0;
    dev_status_manager_clear(DEV_STATUS_BIT_ETH_CONNECTED);
    uhm_events_eth(false, "");
    ESP_LOGW(TAG, "wired uplink lost");
}

/* ---- host bring-up / teardown --------------------------------------------- */

static void host_up(void)
{
    usb_eth_host_config_t cfg = { 0 };

    gpio_set_level(UHM_MODE_GPIO, 1); /* connector -> ESP OTG */

    cfg.enable = true;
    cfg.allowed_driver_mask = USB_ETH_HOST_DRIVER_MASK_ALL;
    cfg.bus_id = 0;
    cfg.gpio.vbus_en_gpio = UHM_VBUS_GPIO;
    cfg.gpio.vbus_en_active_high = true;
    cfg.gpio.vbus_on_delay_ms = UHM_VBUS_DELAY_MS;
    cfg.netif.mode = uhm_settings_ip_static() ? USB_ETH_HOST_IP_MODE_STATIC
                                              : USB_ETH_HOST_IP_MODE_DHCP;
    cfg.netif.static_ip = *uhm_settings_static_ip();
    cfg.netif.prefer_as_default_route = uhm_settings_prefer_usb_route();
    cfg.on_eth_ip_up = on_eth_ip_up;
    cfg.on_eth_ip_lost = on_eth_ip_lost;

    if (usb_eth_host_start(&cfg) != ESP_OK)
    {
        ESP_LOGE(TAG, "host start failed; mux back to CH342");
        gpio_set_level(UHM_MODE_GPIO, 0);
        return;
    }

    s_status.host_active = true;
    s_status.vbus_on = true; /* usb_eth_host_start() ensured the rail on */
    s_status.attaches++;
    uhm_events_device(true);
    ESP_LOGI(TAG, "device attached; host mode on");
}

static void host_down(void)
{
    if (!s_status.host_active)
    {
        return;
    }

    usb_eth_host_stop();

    if (s_status.eth_connected)
    {
        on_eth_ip_lost();
    }

    gpio_set_level(UHM_MODE_GPIO, 0); /* connector -> CH342 */
    s_status.host_active = false;
    s_status.vbus_on = false;
    s_status.driver[0] = '\0';
    uhm_events_device(false);
    ESP_LOGI(TAG, "device detached; mux back to CH342");
}

static void presence_task(void *arg)
{
    (void)arg;

    uhm_presence_init(&s_presence, gpio_get_level(UHM_ID_GPIO));
    s_status.device_present = s_presence.present;

    if (s_presence.present)
    {
        host_up(); /* already plugged at boot */
    }

    while (true)
    {
        if (!s_run)
        {
            host_down();
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        uhm_edge_t edge = uhm_presence_sample(
            &s_presence, gpio_get_level(UHM_ID_GPIO));

        s_status.device_present = s_presence.present;

        if (edge == UHM_EDGE_ATTACH)
        {
            host_up();
        }
        else if (edge == UHM_EDGE_DETACH)
        {
            host_down();
        }

        vTaskDelay(pdMS_TO_TICKS(UHM_POLL_MS));
    }
}

/* ---- the warm restart in the device role ------------------------------------- */

/* esp_restart runs the shutdown handlers before the reset: drop the device
 * off the bus first, so the PC sees an unplug and the next image enumerates
 * with no transfer in flight. Without it the host is never told the device
 * went away, a transfer stays in flight across the reset, and the next
 * image can take the stale OUT completion before its class armed a read
 * (the ESPNetLink crashed that way in ISR context about 5 s after a warm
 * reboot, 2026-10-16). Soft-disconnect only: the controller stays clocked,
 * so a task that still sends in the restart window cannot hang on a gated
 * bus. Hooked once the device core came up (the host role never
 * initialises it). IDF keeps five shutdown-handler slots: WiFi, esp_timer,
 * can_manager and data_logger hold four; this is the fifth. */
static bool s_detach_hooked;

static void uhm_on_shutdown(void)
{
    (void)usb_dc_soft_disconnect(0);
}

static void uhm_hook_restart_detach(void)
{
    if (s_detach_hooked)
    {
        return;
    }

    esp_err_t err = esp_register_shutdown_handler(uhm_on_shutdown);

    if (err == ESP_OK)
    {
        s_detach_hooked = true;
    }
    else
    {
        /* no slot left (standard section 12: loud, the boot_errors net sees
         * it): a warm restart then keeps the PC attached across the reset */
        ESP_LOGE(TAG, "restart detach not hooked: %s", esp_err_to_name(err));
    }
}

/* ---- lifecycle -------------------------------------------------------------- */

esp_err_t usb_host_manager_init(void)
{
    static const log_descriptor_t LOG_DESC =
        { "usb_host_manager", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);
    uhm_events_register();
    return uhm_settings_register();
}

esp_err_t usb_host_manager_start(void)
{
    if (!uhm_settings_is_configured())
    {
        ESP_LOGE(TAG, "unconfigured; not starting");
        return ESP_ERR_INVALID_STATE; /* standard §4.3 step 5 */
    }

    if (uhm_settings_role_device())
    {
        /* Phase 3 (TASK_j2534_server.md §5): route the connector to the
         * ESP-OTG and bring up the CherryUSB DEVICE stack:
         *   ncm/rndis → usb_net_device: WiCAN is a USB NIC to the PC →
         *               IP-over-USB carries the J2534 TCP server + web UI
         *               + bridges (NCM = what Windows 10/11 binds natively;
         *               24H2 removed the RNDIS driver);
         *   cdc       → CDC-ACM serial, not implemented yet.
         * No VBUS drive: the PC powers the bus in device role. */
        usb_net_device_config_t dev_cfg = { 0 };

        /* CDC-ACM serial J2534 transport (usb_cdc_device): a virtual COM
         * port instead of the NCM/RNDIS network device. */
        if (strcmp(uhm_settings_device_class(), "cdc") == 0)
        {
            gpio_reset_pin(UHM_MODE_GPIO);
            gpio_set_direction(UHM_MODE_GPIO, GPIO_MODE_OUTPUT);
            gpio_set_level(UHM_MODE_GPIO, 1); /* connector -> ESP OTG */

            if (usb_cdc_device_start() != ESP_OK)
            {
                ESP_LOGE(TAG, "usb_cdc_device start failed; mux back to "
                         "CH342");
                gpio_set_level(UHM_MODE_GPIO, 0);
                return ESP_FAIL;
            }

            uhm_hook_restart_detach();
            snprintf(s_status.driver, sizeof(s_status.driver), "cdc-dev");
            ESP_LOGI(TAG, "role=device up (class=cdc, io%d -> ESP OTG)",
                     UHM_MODE_GPIO);
            return ESP_OK;
        }

        dev_cfg.device_class =
            (strcmp(uhm_settings_device_class(), "rndis") == 0)
                ? USB_NET_DEVICE_CLASS_RNDIS
                : USB_NET_DEVICE_CLASS_NCM;

        if (uhm_settings_ip_static())
        {
            dev_cfg.ip = uhm_settings_static_ip()->ip;
            dev_cfg.netmask = uhm_settings_static_ip()->netmask;
        }

        gpio_reset_pin(UHM_MODE_GPIO);
        gpio_set_direction(UHM_MODE_GPIO, GPIO_MODE_OUTPUT);
        gpio_set_level(UHM_MODE_GPIO, 1); /* connector -> ESP OTG */

        if (usb_net_device_start(&dev_cfg) != ESP_OK)
        {
            ESP_LOGE(TAG, "usb_net_device start failed; mux back to CH342");
            gpio_set_level(UHM_MODE_GPIO, 0);
            return ESP_FAIL;
        }

        uhm_hook_restart_detach();

        usb_net_device_status_t dev_st;

        if (usb_net_device_get_status(&dev_st) == ESP_OK)
        {
            snprintf(s_status.driver, sizeof(s_status.driver), "%s-dev",
                     dev_st.device_class);
            snprintf(s_status.ip, sizeof(s_status.ip), "%s", dev_st.ip);
        }

        ESP_LOGI(TAG, "role=device up (class=%s, io%d -> ESP OTG)",
                 uhm_settings_device_class(), UHM_MODE_GPIO);
        return ESP_OK;
    }

    if (!uhm_settings_enabled())
    {
        ESP_LOGI(TAG, "disabled in settings (connector stays on the "
                 "CH342)");
        return ESP_OK;
    }

    gpio_config_t id_cfg =
    {
        .pin_bit_mask = 1ULL << UHM_ID_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, /* floating = no device */
    };

    (void)gpio_config(&id_cfg);
    gpio_reset_pin(UHM_MODE_GPIO);
    gpio_set_direction(UHM_MODE_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(UHM_MODE_GPIO, 0);

    s_run = true;

    if (s_task == NULL)
    {
        s_task = xTaskCreateStatic(presence_task, "usb_presence",
                                   sizeof(s_stack) / sizeof(s_stack[0]),
                                   NULL, 4, s_stack, &s_tcb);

        if (s_task == NULL)
        {
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "started (ID io%d, mux io%d, vbus io%d, %s, %s)",
             UHM_ID_GPIO, UHM_MODE_GPIO, UHM_VBUS_GPIO,
             uhm_settings_ip_static() ? "static" : "dhcp",
             uhm_settings_prefer_usb_route() ? "usb-first route"
                                             : "wifi-first");
    return ESP_OK;
}

esp_err_t usb_host_manager_stop(void)
{
    s_run = false; /* the presence task tears down on its next lap */

    /* Bounded wait for that lap (2026-10-01): the sleep prepare sequence
     * calls this and then powers the USB rail down and light-sleeps; an
     * async teardown raced that (the CherryUSB deinit, its 200 ms settle
     * and the interrupt free over IPC). The presence task polls every
     * UHM_POLL_MS, host_down() takes ~0.3 s; 3 s is generous. Only waits
     * when a host was actually up. */
    for (int i = 0; i < 60 && s_task != NULL && s_status.host_active; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (s_status.host_active)
    {
        ESP_LOGW(TAG, "stop: host teardown still running after 3 s");
    }

    return ESP_OK;
}

esp_err_t usb_host_manager_status(usb_host_manager_status_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out = s_status;

    /* the enumerated device, from the host's mount hook (2026-10-07) */
    usb_eth_host_device_t d;

    out->dev_present = s_status.host_active &&
                       usb_eth_host_get_attached_device(&d);
    if (out->dev_present)
    {
        out->dev_vid = d.vid;
        out->dev_pid = d.pid;
        out->dev_class = d.intf_class != 0 ? d.intf_class : d.dev_class;
        strncpy(out->dev_product, d.product, sizeof(out->dev_product) - 1);
        out->dev_product[sizeof(out->dev_product) - 1] = '\0';
    }
    else
    {
        out->dev_vid = 0;
        out->dev_pid = 0;
        out->dev_class = 0;
        out->dev_product[0] = '\0';
    }
    return ESP_OK;
}

esp_err_t usb_host_manager_set_vbus(bool on)
{
    if (!s_status.host_active)
    {
        return ESP_ERR_INVALID_STATE;
    }

    /* same pin mode usb_eth_host configured it with (open-drain, board
     * pull-up): 1 = released = rail ON, 0 = pulled low = rail OFF */
    gpio_set_direction(UHM_VBUS_GPIO, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(UHM_VBUS_GPIO, on ? 1 : 0);
    s_status.vbus_on = on;
    ESP_LOGI(TAG, "vbus %s", on ? "on" : "off");
    return ESP_OK;
}
