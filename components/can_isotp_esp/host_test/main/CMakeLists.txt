# Host test compiles ONLY the pure mailbox ring — no RTOS, no esp_isotp
# (which REQUIRES esp_driver_twai and cannot build for linux).
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "${comp_dir}/can_isotp_esp_mbox.c"
    INCLUDE_DIRS "${comp_dir}/include" "${comp_dir}"
    REQUIRES unity
)
