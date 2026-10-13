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
 * @file vpn_manager_wg.c
 * @brief esp_wireguard glue (legacy vpn_wireguard.c semantics kept):
 *        endpoint resolved to IPv4 once per up (the port doesn't
 *        resolve internally), peer-up = the wireguardif poll. Called
 *        from the state task ONLY.
 *
 *        esp_wireguard's allowed_ip/allowed_ip_mask configure the lwIP
 *        netif, NOT a WireGuard AllowedIPs route: allowed_ip becomes
 *        the netif's LOCAL address (netif_add uses it directly). The
 *        netif IP is ALWAYS Interface.Address with a /32 netmask;
 *        passing the AllowedIPs NETWORK address through instead
 *        brought the netif up as e.g. 10.8.0.0 and silently killed the
 *        whole data plane (BUG_WG_NETIF_ADDR.md). Which destinations
 *        enter the tunnel is the `routes` list (issue #975,
 *        2026-10-11): every CIDR goes into ip4_routes' table for this
 *        netif once the tunnel is up (lwIP's route lookup consults it
 *        before its netif walk; the tunnel's own network is one of the
 *        routes) and leaves it before the netif is removed;
 *        `default_route` still makes the netif the default one.
 */
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wireguard.h"
#include "ip4_routes.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"

#include "vpn_manager_private.h"

static const char *TAG = "vpn_manager";

static wireguard_ctx_t s_ctx;
/* PSRAM; esp_wireguard keeps a pointer to it: static for life */
static wireguard_config_t s_cfg EXT_RAM_BSS_ATTR;
/* esp_wireguard keeps POINTERS into its config: these back them */
static char s_addr[32];
static char s_endpoint[64];
static char s_priv[64];
static char s_pub[64];
static char s_psk[64];

static bool resolve_ipv4(const char *host, char *out, size_t len)
{
    struct in_addr a;

    if (inet_pton(AF_INET, host, &a) == 1)
    {
        strlcpy(out, host, len);
        return true;
    }

    struct addrinfo hints = { .ai_family = AF_INET,
                              .ai_socktype = SOCK_DGRAM };
    struct addrinfo *res = NULL;

    if (getaddrinfo(host, NULL, &hints, &res) != 0 || res == NULL)
    {
        return false;
    }

    bool ok = false;

    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next)
    {
        if (ai->ai_family == AF_INET && ai->ai_addr != NULL)
        {
            struct sockaddr_in *sa = (struct sockaddr_in *)ai->ai_addr;

            ok = inet_ntop(AF_INET, &sa->sin_addr, out, len) != NULL;
            break;
        }
    }

    freeaddrinfo(res);
    return ok;
}

esp_err_t vpn_wg_up(const vpn_config_t *cfg)
{
    if (s_ctx.config != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_cfg, 0, sizeof(s_cfg));
    strlcpy(s_priv, cfg->private_key, sizeof(s_priv));
    strlcpy(s_pub, cfg->peer_public_key, sizeof(s_pub));
    s_cfg.private_key = s_priv;
    s_cfg.public_key = s_pub;

    if (cfg->preshared_key[0] != '\0')
    {
        strlcpy(s_psk, cfg->preshared_key, sizeof(s_psk));
        s_cfg.preshared_key = s_psk;
    }

    /* Netif IP = ALWAYS Interface.Address (see the file header); the
     * user's AllowedIPs contributes only the netmask */
    strlcpy(s_addr, cfg->address, sizeof(s_addr));

    char *slash = strchr(s_addr, '/');

    if (slash != NULL)
    {
        *slash = '\0';
    }

    s_cfg.allowed_ip = s_addr;
    s_cfg.allowed_ip_mask = "255.255.255.255"; /* the routes list steers */

    ESP_LOGI(TAG, "WG netif %s/32, routes: %s", s_addr,
             cfg->routes[0] ? cfg->routes : "(none beyond default_route)");

    if (!resolve_ipv4(cfg->endpoint, s_endpoint, sizeof(s_endpoint)))
    {
        ESP_LOGW(TAG, "endpoint '%s' did not resolve; using as-is",
                 cfg->endpoint);
        strlcpy(s_endpoint, cfg->endpoint, sizeof(s_endpoint));
    }

    s_cfg.endpoint = s_endpoint;
    s_cfg.port = cfg->port;
    s_cfg.persistent_keepalive = cfg->keepalive_s;
    s_cfg.listen_port = 0;
    s_cfg.fw_mark = 0;

    esp_err_t err = esp_wireguard_init(&s_cfg, &s_ctx);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "wireguard init: %s", esp_err_to_name(err));
        memset(&s_ctx, 0, sizeof(s_ctx));
        return err;
    }

    err = esp_wireguard_connect(&s_ctx);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "wireguard connect: %s", esp_err_to_name(err));
        vpn_wg_down();
        return err;
    }

    /* the split tunnel (issue #975): every listed network routes into
       this netif; the list was validated at the settings PUT */
    ip4_routes_entry_t routes[VPN_MAX_ROUTES];
    int n = ip4_routes_parse_list(cfg->routes, routes, VPN_MAX_ROUTES,
                                  NULL, 0);

    if (n > 0 && s_ctx.netif != NULL &&
        ip4_routes_set(s_ctx.netif, routes, (size_t)n) != ESP_OK)
    {
        ESP_LOGW(TAG, "routes not installed: the route table is full");
    }

    ESP_LOGI(TAG, "tunnel up to %s:%d (handshake pending)", s_endpoint,
             cfg->port);
    return ESP_OK;
}

void vpn_wg_down(void)
{
    if (s_ctx.config == NULL)
    {
        return;
    }

    if (s_ctx.netif != NULL)
    {
        ip4_routes_clear(s_ctx.netif); /* before the netif goes */

        esp_err_t err = esp_wireguard_disconnect(&s_ctx);

        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "disconnect: %s", esp_err_to_name(err));
        }
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    memset(&s_cfg, 0, sizeof(s_cfg));
    memset(s_priv, 0, sizeof(s_priv)); /* don't leave key material */
    memset(s_psk, 0, sizeof(s_psk));
    ESP_LOGI(TAG, "tunnel down");
}

bool vpn_wg_peer_up(void)
{
    if (s_ctx.config == NULL || s_ctx.netif == NULL)
    {
        return false;
    }

    return esp_wireguardif_peer_is_up(&s_ctx) == ESP_OK;
}

esp_err_t vpn_wg_set_default_route(void)
{
    if (s_ctx.config == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    return esp_wireguard_set_default(&s_ctx);
}
