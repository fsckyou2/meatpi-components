# Pure modules only: the ESPNetLink `gps -p -j` parser (usb_acm_gps.c has
# no FreeRTOS/USB deps), since 2026-10-07 the NMEA 0183 reader for a plain
# USB GNSS receiver (usb_acm_nmea.c), and since 2026-10-11 the settings
# migration (usb_acm_cli_migrate.c, cJSON only: v1 `enabled` -> v2
# `console`, issue #978). The GPS values ride into autopid (and thence HA /
# the logger / the dashboard).
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "${comp_dir}/usb_acm_gps.c"
         "${comp_dir}/usb_acm_nmea.c"
         "${comp_dir}/usb_acm_cli_migrate.c"
    INCLUDE_DIRS "${comp_dir}/include" "${comp_dir}"
    REQUIRES unity
)
