set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c" "${comp_dir}/src/ml_disco_pong.c"
    INCLUDE_DIRS "stubs" "${comp_dir}/src"
    REQUIRES unity
)
