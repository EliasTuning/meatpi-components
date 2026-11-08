# Pure modules only: se_script_name_ok (script_engine_name.c), the
# script-name guard, the obd conversation core (script_engine_obd.c) over a
# fake port, and the self-description tables (script_engine_doc.c +
# script_engine_examples.c: the reference and example gallery the web UI
# renders). Split out of script_engine.c precisely so they can be
# host-tested without the Berry VM / filesystem / settings deps.
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "test_obd.c"
         "test_reflash.c"
         "test_doc.c"
         "test_gpio.c"
         "${comp_dir}/script_engine_gpio.c"
         "${comp_dir}/script_engine_name.c"
         "${comp_dir}/script_engine_obd.c"
         "${comp_dir}/script_engine_doc.c"
         "${comp_dir}/script_engine_examples.c"
    INCLUDE_DIRS "${comp_dir}" "${comp_dir}/include"
    REQUIRES unity
)
