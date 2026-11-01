# Host test compiles ONLY the pure parse/framing and fw-version modules: no UART/GPIO deps.
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "test_parse.c"
         "test_stslcs.c"
         "${comp_dir}/obd_chip_parse.c"
         "${comp_dir}/obd_chip_fw_version.c"
         "${comp_dir}/obd_chip_stslcs.c"
    INCLUDE_DIRS "${comp_dir}/include" "${comp_dir}"
    REQUIRES unity
)
