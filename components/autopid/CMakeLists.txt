idf_component_register(
    SRCS "autopid.c"
         "autopid_settings.c"
         "autopid_runner.c"
         "autopid_transport_obd.c"
         "autopid_filter.c"
         "autopid_config.c"
         "autopid_sched.c"
         "autopid_group.c"
         "autopid_resp.c"
         "autopid_std.c"
         "autopid_cache.c"
         "autopid_dtc_codec.c"
         "autopid_dtc.c"
         "autopid_dtc_db_codec.c"
         "autopid_dtc_db.c"
         "autopid_dbc_codec.c"
         "autopid_dbc.c"
         "autopid_http.c"
         "autopid_cli.c"
         "autopid_events.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # Feature: scheduled PID polling over obd_chip; expressions via the
    # pure expression_parser; tables on /data via filesystem; voltage
    # pause via battery_monitor. cmdline/http only by the surface files
    # (own-routes + §6b self-registered CLI).
    REQUIRES espressif__cjson
    PRIV_REQUIRES sleep_manager obd_chip expression_parser settings_manager filesystem obd_gate
                  battery_monitor log_manager esp_timer cmdline_manager
                  http_server_manager esp_http_server console
                  event_manager dev_status_manager can_manager
                  uds_manager
)
