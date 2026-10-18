idf_component_register(
    SRCS "ota_manager.c"
         "ota_session.c"
         "ota_pipe.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # Public header exposes only esp_err/stdint -> everything private.
    # Transport-agnostic: NO dependency on any transport (http/tcp/mqtt),
    # transports call the session API (Architecture ownership inversion).
    # mbedtls: the session's SHA-256 through the PSA API (mbedtls 4.1).
    PRIV_REQUIRES app_update log_manager mbedtls
)
