# web_ui_v2: the v2 single-page app, embedded (gzipped) and served by the
# http_server_manager catch-all. Compiled only when CONFIG_WICAN_WEBUI_V2 is
# selected (the Kconfig choice); otherwise this is an empty component so the
# build + main's web_ui_v2_register() stub still link.
set(srcs "web_ui_v2.c")
set(embeds "")

if(CONFIG_WICAN_WEBUI_V2)
    # Minify (rjsmin/rcssmin, vendored under tools/) and pre-gzip index.html
    # into the build dir, then embed the .gz. Portable: uses the IDF Python
    # (no node, no external gzip binary needed on Windows).
    idf_build_get_property(python PYTHON)
    set(src_html "${CMAKE_CURRENT_LIST_DIR}/web/index.html")
    set(gz_html  "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz")
    add_custom_command(
        OUTPUT "${gz_html}"
        COMMAND "${python}" "${CMAKE_CURRENT_LIST_DIR}/gzip_asset.py"
                "${src_html}" "${gz_html}"
        DEPENDS "${src_html}" "${CMAKE_CURRENT_LIST_DIR}/gzip_asset.py"
                "${CMAKE_CURRENT_LIST_DIR}/tools/rjsmin.py"
                "${CMAKE_CURRENT_LIST_DIR}/tools/rcssmin.py"
        COMMENT "Minifying + gzipping web_ui_v2 index.html for embedding"
        VERBATIM)
    # On-demand page chunks (2026-09-07): the Scripts page ships as its own
    # minified + gzipped blob, fetched from /ui/scripts.js the first time
    # the page opens, so the main page stays small.
    set(src_js "${CMAKE_CURRENT_LIST_DIR}/web/scripts.js")
    set(gz_js  "${CMAKE_CURRENT_BINARY_DIR}/scripts.js.gz")
    add_custom_command(
        OUTPUT "${gz_js}"
        COMMAND "${python}" "${CMAKE_CURRENT_LIST_DIR}/gzip_asset.py"
                "${src_js}" "${gz_js}"
        DEPENDS "${src_js}" "${CMAKE_CURRENT_LIST_DIR}/gzip_asset.py"
                "${CMAKE_CURRENT_LIST_DIR}/tools/rjsmin.py"
        COMMENT "Minifying + gzipping web_ui_v2 scripts.js chunk for embedding"
        VERBATIM)
    # second chunk (2026-09-07): the File Manager page
    set(src_files_js "${CMAKE_CURRENT_LIST_DIR}/web/files.js")
    set(gz_files_js  "${CMAKE_CURRENT_BINARY_DIR}/files.js.gz")
    add_custom_command(
        OUTPUT "${gz_files_js}"
        COMMAND "${python}" "${CMAKE_CURRENT_LIST_DIR}/gzip_asset.py"
                "${src_files_js}" "${gz_files_js}"
        DEPENDS "${src_files_js}" "${CMAKE_CURRENT_LIST_DIR}/gzip_asset.py"
                "${CMAKE_CURRENT_LIST_DIR}/tools/rjsmin.py"
        COMMENT "Minifying + gzipping web_ui_v2 files.js chunk for embedding"
        VERBATIM)
    # third chunk (2026-09-07): the CAN Monitor page
    set(src_monitor_js "${CMAKE_CURRENT_LIST_DIR}/web/monitor.js")
    set(gz_monitor_js  "${CMAKE_CURRENT_BINARY_DIR}/monitor.js.gz")
    add_custom_command(
        OUTPUT "${gz_monitor_js}"
        COMMAND "${python}" "${CMAKE_CURRENT_LIST_DIR}/gzip_asset.py"
                "${src_monitor_js}" "${gz_monitor_js}"
        DEPENDS "${src_monitor_js}" "${CMAKE_CURRENT_LIST_DIR}/gzip_asset.py"
                "${CMAKE_CURRENT_LIST_DIR}/tools/rjsmin.py"
        COMMENT "Minifying + gzipping web_ui_v2 monitor.js chunk for embedding"
        VERBATIM)
    # fourth chunk (2026-10-01): the Quick Setup wizard
    set(src_setup_js "${CMAKE_CURRENT_LIST_DIR}/web/setup.js")
    set(gz_setup_js  "${CMAKE_CURRENT_BINARY_DIR}/setup.js.gz")
    add_custom_command(
        OUTPUT "${gz_setup_js}"
        COMMAND "${python}" "${CMAKE_CURRENT_LIST_DIR}/gzip_asset.py"
                "${src_setup_js}" "${gz_setup_js}"
        DEPENDS "${src_setup_js}" "${CMAKE_CURRENT_LIST_DIR}/gzip_asset.py"
                "${CMAKE_CURRENT_LIST_DIR}/tools/rjsmin.py"
        COMMENT "Minifying + gzipping web_ui_v2 setup.js chunk for embedding"
        VERBATIM)
    # fifth chunk (2026-10-11): the configuration backup cards (issue #979)
    set(src_backup_js "${CMAKE_CURRENT_LIST_DIR}/web/backup.js")
    set(gz_backup_js  "${CMAKE_CURRENT_BINARY_DIR}/backup.js.gz")
    add_custom_command(
        OUTPUT "${gz_backup_js}"
        COMMAND "${python}" "${CMAKE_CURRENT_LIST_DIR}/gzip_asset.py"
                "${src_backup_js}" "${gz_backup_js}"
        DEPENDS "${src_backup_js}" "${CMAKE_CURRENT_LIST_DIR}/gzip_asset.py"
                "${CMAKE_CURRENT_LIST_DIR}/tools/rjsmin.py"
        COMMENT "Minifying + gzipping web_ui_v2 backup.js chunk for embedding"
        VERBATIM)
    add_custom_target(web_ui_v2_gz DEPENDS "${gz_html}" "${gz_js}" "${gz_files_js}" "${gz_monitor_js}" "${gz_setup_js}" "${gz_backup_js}")
    set(embeds "${gz_html}" "${gz_js}" "${gz_files_js}" "${gz_monitor_js}" "${gz_setup_js}" "${gz_backup_js}")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES http_server_manager
    EMBED_FILES ${embeds}
)

if(CONFIG_WICAN_WEBUI_V2)
    add_dependencies(${COMPONENT_LIB} web_ui_v2_gz)
endif()
