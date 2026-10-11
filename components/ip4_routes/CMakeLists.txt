idf_component_register(
    SRCS "ip4_routes.c"
         "ip4_routes_core.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # lwip: the netif type and the hook's address types; log_manager: the
    # log descriptor
    PRIV_REQUIRES lwip log_manager
)

# IDF's lwIP port defines LWIP_HOOK_IP4_ROUTE_SRC as its own
# ip4_route_src_hook (source-address routing; a strong symbol with no
# Kconfig choice). The destination route table gets in front of it by
# wrapping the symbol at link time (the restart_tracker / can_manager
# pattern): lwIP's calls land in __wrap_ip4_route_src_hook, which falls
# through to __real_ip4_route_src_hook when the table has no answer.
target_link_libraries(${COMPONENT_LIB} INTERFACE
                      "-Wl,--wrap=ip4_route_src_hook")
