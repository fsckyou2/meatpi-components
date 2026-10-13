# Host test compiles ONLY the pure time codec and the pure clock-step
# bookkeeping: no I2C, no RTOS.
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "${comp_dir}/rtc_manager_time.c"
         "${comp_dir}/rtc_manager_clock.c"
    INCLUDE_DIRS "${comp_dir}/include" "${comp_dir}"
    REQUIRES unity
)
