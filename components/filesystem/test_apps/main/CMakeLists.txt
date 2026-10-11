idf_component_register(
    SRCS "test_fs_main.c"
    # fatfs: the prototype of the wrapped esp_vfs_fat_info (the 32 GB card leg)
    REQUIRES filesystem fatfs
)

# The 32 GB card leg (issue #982): this app runs without a card, so the
# FATFS capacity query is answered by the app itself for /sd
# (test_fs_main.c); the real function stays reachable as
# __real_esp_vfs_fat_info for every other call.
target_link_libraries(${COMPONENT_LIB} INTERFACE
                      "-Wl,--wrap=esp_vfs_fat_info")
