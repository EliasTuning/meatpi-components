# Pure modules only: settings-item parse/normalize, scheduler + backoff,
# percent-encoding / URL composition, topic expansion (core) and the ABRP
# mapping/form/response rules (abrp). No IDF glue is compiled.
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "${comp_dir}/data_destinations_core.c"
         "${comp_dir}/data_destinations_abrp.c"
    INCLUDE_DIRS "${comp_dir}" "${comp_dir}/include"
    REQUIRES unity
)
