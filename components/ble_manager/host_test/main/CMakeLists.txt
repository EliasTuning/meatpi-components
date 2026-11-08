# Host test compiles ONLY the pure layer (TX packing math, identity
# derivations, TX-power clamp): no BT stack, so it runs on the linux target.
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "test_pack.c"
         "test_ram.c"
         "test_diag.c"
         "${comp_dir}/ble_manager_pack.c"
         "${comp_dir}/ble_manager_diag.c"
    INCLUDE_DIRS "${comp_dir}/include" "${comp_dir}"
    REQUIRES unity
)
