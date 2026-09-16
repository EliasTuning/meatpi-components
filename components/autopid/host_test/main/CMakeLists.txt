# Pure modules only: scheduler, response parser, config parse (+ the
# expression_parser dependency, itself pure). AUTOPID_HOST_TEST strips
# the target-only halves (file IO, cache).
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")
set(ep_dir "${CMAKE_CURRENT_LIST_DIR}/../../../expression_parser")
# the chip driver's pure parse module carries the EEPROM guard that
# ap_init_sanitize() delegates to (obd_chip_guard.h, 2026-09-16)
set(oc_dir "${CMAKE_CURRENT_LIST_DIR}/../../../obd_chip")

idf_component_register(
    SRCS "test_main.c"
         "${comp_dir}/autopid_sched.c"
         "${comp_dir}/autopid_resp.c"
         "${comp_dir}/autopid_config.c"
         "${comp_dir}/autopid_std.c"
         "${comp_dir}/autopid_dtc_codec.c"
         "${comp_dir}/autopid_dtc_db_codec.c"
         "${comp_dir}/autopid_dbc_codec.c"
         "${ep_dir}/expression_parser.c"
         "${oc_dir}/obd_chip_parse.c"
    INCLUDE_DIRS "${comp_dir}" "${comp_dir}/include" "${ep_dir}/include"
                 "${oc_dir}/include" "${oc_dir}"
    REQUIRES unity
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE AUTOPID_HOST_TEST=1)
