idf_build_get_property(build_dir BUILD_DIR)

# this build's partition table, the layout the flash must carry: the file
# the partition_table component generates from the project's CSV
set(pm_table_bin "${build_dir}/partition_table/partition-table.bin")

idf_component_register(
    SRCS "partition_migrate.c"
         "partition_migrate_core.c"
         "partition_migrate_md5.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # spi_flash: the raw read, erase and write at the table's offset;
    # heap: the internal-RAM buffer; esp_timer: the measured erase+write
    PRIV_REQUIRES spi_flash heap esp_timer log_manager
)

target_add_binary_data(${COMPONENT_LIB} "${pm_table_bin}" BINARY
                       DEPENDS partition_table_bin)
add_dependencies(${COMPONENT_LIB} partition_table_bin)
