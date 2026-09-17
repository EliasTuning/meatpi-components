# Pure modules only: rule parse/eval + templates. EM_HOST_TEST strips the
# target-only core JSON builders from the private header.
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "${comp_dir}/event_manager_rules.c"
         "${comp_dir}/event_manager_eval.c"
         "${comp_dir}/event_manager_template.c"
    INCLUDE_DIRS "${comp_dir}" "${comp_dir}/include"
    REQUIRES unity
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE EM_HOST_TEST=1)
