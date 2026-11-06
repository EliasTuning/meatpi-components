# Host test compiles ONLY the pure helpers: the file-name rules and row
# encoders, the recovery decisions and the clock rules. No sqlite, no
# RTOS, no VFS.
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "test_recover.c"
         "test_clock.c"
         "${comp_dir}/data_logger_files.c"
         "${comp_dir}/data_logger_cursor.c"
         "${comp_dir}/data_logger_recover.c"
         "${comp_dir}/data_logger_clock.c"
    INCLUDE_DIRS "${comp_dir}/include" "${comp_dir}"
    REQUIRES unity
)
