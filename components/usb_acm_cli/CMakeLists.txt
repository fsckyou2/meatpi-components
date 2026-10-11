idf_component_register(
    SRCS "usb_acm_cli.c"
         "usb_acm_cli_poll.c"
         "usb_acm_gps.c"
         "usb_acm_nmea.c"
         "usb_acm_cli_settings.c"
         "usb_acm_cli_migrate.c"
         "usb_acm_cli_http.c"
         "usb_acm_cli_cli.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # Feature: USB serial devices on the host port: a plain USB GPS receiver
    # (NMEA, read by default) and the espnetlink's CDC-ACM management console
    # (talked to when its switch is on). The USB host is owned by
    # usb_host_manager/usb_eth_host; this only registers CherryUSB's CDC-ACM
    # class callbacks + surfaces.
    PRIV_REQUIRES cherryusb bridge_manager settings_manager log_manager
                  esp_mm http_server_manager esp_http_server espressif__cjson
                  cmdline_manager console
)
