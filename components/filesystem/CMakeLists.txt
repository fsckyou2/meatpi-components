idf_component_register(
    SRCS "filesystem.c"
         "filesystem_read.c"
         "filesystem_settings.c"
         "filesystem_stream.c"
         "filesystem_path.c"
         "filesystem_mount.c"
         "filesystem_cli.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # public header exposes only stdio/esp_err types -> everything is
    # private; cmdline_manager only by the _cli.c (`fs` command)
    PRIV_REQUIRES littlefs vfs fatfs log_manager cmdline_manager
                  settings_manager
)
