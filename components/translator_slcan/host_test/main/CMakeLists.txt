# Pure codec only (translator_slcan_codec.c). Includes bridge_manager.h +
# can_frame_wire.h + can_core.h (headers pull FreeRTOS, available on the
# linux target, same as bridge_manager's own host test).
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")
set(root "${comp_dir}/..")

idf_component_register(
    SRCS "test_main.c"
         "${comp_dir}/translator_slcan_codec.c"
    INCLUDE_DIRS
        "${comp_dir}"
        "${root}/bridge_manager/include"
        "${root}/can_manager/include"
    REQUIRES unity
)
