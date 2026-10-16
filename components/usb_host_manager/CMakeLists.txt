idf_component_register(
    SRCS "usb_host_manager.c"
         "usb_host_manager_settings.c"
         "usb_host_manager_presence.c"
         "usb_host_manager_events.c"
         "usb_host_manager_http.c"
         "usb_host_manager_cli.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # Feature/policy: the USB role mux + presence watcher; the driver
    # work lives in usb_eth_host. /api/usb and the `usb` command are
    # optional (main wires them).
    PRIV_REQUIRES usb_eth_host usb_net_device usb_cdc_device dev_status_manager event_manager
                  http_server_manager settings_manager log_manager
                  cmdline_manager esp_driver_gpio esp_netif
                  cherryusb   # usb_dc_soft_disconnect(): the restart detach
)
