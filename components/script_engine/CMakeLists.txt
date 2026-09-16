idf_component_register(
    SRCS "script_engine.c"
         "script_engine_name.c"
         "script_engine_obd.c"
         "script_engine_settings.c"
         "script_engine_bind.c"
         "script_engine_doc.c"
         "script_engine_examples.c"
         "script_engine_http.c"
         "script_engine_cli.c"
         "script_engine_events.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # Feature: the Berry VM (berry) with device bindings (uds/can/event/dtc).
    PRIV_REQUIRES berry uds_manager can_manager event_manager filesystem obd_gate
                  autopid
                  settings_manager log_manager esp_timer esp_mm
                  http_server_manager esp_http_server espressif__cjson
                  cmdline_manager console
)
