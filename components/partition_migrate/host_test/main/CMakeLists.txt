# Host test compiles ONLY the pure core: no flash driver, so it runs on
# the linux target.
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "test_core.c"
         "${comp_dir}/partition_migrate_core.c"
         "${comp_dir}/partition_migrate_md5.c"
    INCLUDE_DIRS "${comp_dir}/include" "${comp_dir}"
    REQUIRES unity
)
