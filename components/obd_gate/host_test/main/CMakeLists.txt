# Host test compiles ONLY the pure gate core — no RTOS, time injected.
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "${comp_dir}/obd_gate_core.c"
         "${comp_dir}/obd_gate_diag.c"
    INCLUDE_DIRS "${comp_dir}/include" "${comp_dir}"
    REQUIRES unity
)
