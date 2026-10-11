# Host test compiles ONLY the pure config checks (plus ip4_routes' pure
# core, which the routes list check calls): no wireguard, no RTOS, no netif.
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "${comp_dir}/vpn_manager_check.c"
         "${comp_dir}/../ip4_routes/ip4_routes_core.c"
    INCLUDE_DIRS "${comp_dir}/include" "${comp_dir}"
                 "${comp_dir}/../ip4_routes/include" "${comp_dir}/../ip4_routes"
    REQUIRES unity
)
