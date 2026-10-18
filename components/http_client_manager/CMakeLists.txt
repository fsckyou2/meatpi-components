idf_component_register(
    SRCS "http_client_manager.c"
         "http_client_manager_events.c"
         "http_client_manager_auth.c"
         "http_client_manager_download.c"
         "http_client_manager_upload.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # Service: the one way components do HTTP-client work. Per-request
    # client instances on the caller's task (thread-safe by isolation) +
    # a concurrency limiter; TLS via cert_manager sets like mqtt.
    PRIV_REQUIRES event_manager esp_http_client mbedtls cert_manager filesystem
                  log_manager
)
