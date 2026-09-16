idf_component_register(
    SRCS "uds_manager.c"
         "uds_manager_settings.c"
         "uds_proto.c"
         "uds_dtc_codec.c"
         "uds_transport_at.c"
         "uds_transport_obd.c"
         "uds_transport_isotp.c"
         "uds_manager_http.c"
         "uds_manager_cli.c"
         "uds_manager_events.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # Service: UDS protocol over a selectable transport (MIC via obd_chip,
    # or firmware ISO-TP on can_manager's provider slot).
    PRIV_REQUIRES obd_chip can_manager can_isotp_esp event_manager obd_gate
                  settings_manager log_manager esp_timer
                  http_server_manager esp_http_server espressif__cjson
                  cmdline_manager console
)
