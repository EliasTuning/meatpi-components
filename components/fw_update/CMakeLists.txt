idf_component_register(
    SRCS "fw_update.c"
         "fw_update_job.c"
         "fw_update_api.c"
         "fw_update_dongle.c"
         "fw_update_dongle_job.c"
         "fw_update_fetch_ram.c"
         "fw_update_core.c"
         "fw_update_version.c"
         "fw_update_record.c"
         "fw_update_fetch.c"
         "fw_update_boot.c"
         "fw_update_status.c"
         "fw_update_settings.c"
         "fw_update_http.c"
         "fw_update_cli.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # Service: the manifest check, the pull install through ota_manager's
    # pipe, the job record in NVS and the boot-path rollback. The public
    # header exposes esp_err/stdint only; cJSON comes from the managed
    # espressif/cjson dependency (idf_component.yml). No dependency on
    # espnetlink_link, wifi_manager or usb_host_manager: the uplink word
    # comes from the composition root.
    PRIV_REQUIRES app_update esp_app_format esp_bootloader_format nvs_flash flash_jobs
                  esp_timer esp_http_server console esp_hw_support mbedtls heap
                  log_manager settings_manager cmdline_manager
                  http_server_manager http_client_manager ota_manager
                  partition_migrate restart_tracker dev_status_manager
                  config_backup sleep_manager external_storage
)
