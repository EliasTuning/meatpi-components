idf_component_register(
    SRCS "api_http.c"
         "api_http_system.c"
         "api_http_diag.c"
         "api_http_fs.c"
         "api_http_datapath.c"
         "api_http_ota.c"
         "api_http_util.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # Glue layer: depends DOWN on the cores + the server owner (HTTP_API.md
    # §9). The public header exposes only esp_err_t -> everything private.
    # cJSON comes from the managed espressif/cjson dep (idf_component.yml).
    PRIV_REQUIRES http_server_manager settings_manager dev_status_manager event_manager
                  bridge_manager socket_manager websocket_manager
                  restart_tracker log_manager filesystem ota_manager
                  multipart_upload esp_http_server cmdline_manager
)
