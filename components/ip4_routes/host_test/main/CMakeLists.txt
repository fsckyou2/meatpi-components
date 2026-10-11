# Host test compiles ONLY the pure core: no lwIP, no RTOS, so it runs on
# the linux target.
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "${comp_dir}/ip4_routes_core.c"
    INCLUDE_DIRS "${comp_dir}/include" "${comp_dir}"
    REQUIRES unity
)
