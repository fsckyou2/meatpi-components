idf_component_register(
    SRCS "config_backup.c"
         "config_backup_core.c"
         "config_backup_codec.c"
         "config_backup_crypto.c"
         "config_backup_files.c"
         "config_backup_doc.c"
         "config_backup_copies.c"
         "config_backup_copies_read.c"
         "config_backup_restore.c"
         "config_backup_apply.c"
         "config_backup_http.c"
         "config_backup_cli.c"
         "config_backup_settings.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # The settings export and the per-component restore (settings_manager),
    # the files of /data and /sd read and written by path (filesystem), the
    # identity, the clock and the SD bit (dev_status_manager), the routes
    # (http_server_manager), the console command (cmdline_manager), the
    # planned restart (restart_tracker), the key in NVS (nvs_flash), the
    # random source (esp_hw_support), AES-GCM / HMAC / base64 (mbedtls,
    # the PSA API), the card's free space (fatfs). cJSON comes from the
    # managed espressif/cjson dependency (idf_component.yml).
    PRIV_REQUIRES settings_manager filesystem dev_status_manager
                  http_server_manager cmdline_manager restart_tracker
                  log_manager nvs_flash esp_hw_support mbedtls fatfs
                  esp_http_server console
)
