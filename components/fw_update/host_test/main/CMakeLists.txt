# Pure modules only: the version grammar and order, the manifest parse, the
# selection, the layout verdict, the URL rules, the job record's codec and
# the boot decision table. No IDF glue is compiled.
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "test_image.c"
         "${comp_dir}/fw_update_core.c"
         "${comp_dir}/fw_update_version.c"
         "${comp_dir}/fw_update_record.c"
    INCLUDE_DIRS "${comp_dir}" "${comp_dir}/include"
    REQUIRES unity
)
