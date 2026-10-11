# Pure module only: the JSON member scanner, the redaction rule and its
# inverse, the name checks, the key-file codec, the manifest codec, the
# ISO time. No IDF glue is compiled.
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "${comp_dir}/config_backup_core.c"
         "${comp_dir}/config_backup_codec.c"
    INCLUDE_DIRS "${comp_dir}" "${comp_dir}/include"
    REQUIRES unity
)
