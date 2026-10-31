idf_component_register(
    SRCS "test_ble_main.c"
    REQUIRES ble_manager bridge_manager settings_manager log_manager
             dev_status_manager nvs_flash flash_jobs
)
