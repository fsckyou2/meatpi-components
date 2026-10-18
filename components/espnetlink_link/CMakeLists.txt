idf_component_register(
    SRCS "espnetlink_link.c"
         "espnetlink_link_core.c"
         "espnetlink_link_usb.c"
         "espnetlink_link_pair.c"
         "espnetlink_link_settings.c"
         "espnetlink_link_migrate.c"
         "espnetlink_link_http.c"
         "espnetlink_link_cli.c"
         "espnetlink_link_ensure.c"
         "espnetlink_link_update.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # Feature: the ESPNetLink LTE/GPS dongle as the internet uplink,
    # zero-touch pairing over USB, then its WiFi AP (USB = power only) or
    # plain USB-Ethernet. usb_acm_cli is public: the fix struct/parser/sink
    # type live in its header and are shared with the USB-console GPS path.
    REQUIRES usb_acm_cli
    PRIV_REQUIRES settings_manager log_manager wifi_manager api_http
                  http_client_manager http_server_manager esp_http_server
                  usb_host_manager restart_tracker dev_status_manager
                  cmdline_manager console espressif__cjson esp_wifi esp_netif
                  esp_timer freertos
)
