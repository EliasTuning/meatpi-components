idf_component_register(
    SRCS "j2534_proto.c"
         "j2534_channel.c"
         "j2534_channel_iso.c"
         "j2534_channel_tp20.c"
         "j2534_channel_legacy.c"
         "j2534_server.c"
         "j2534_server_pump.c"
         "j2534_server_voltage.c"
         "j2534_server_transport.c"
         "j2534_server_settings.c"
         "j2534_server_migrate.c"
         "j2534_server_http.c"
         "j2534_server_cli.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # Feature: SAE J2534 PassThru device. TCP transport + session/channel
    # server; CAN channels on can_manager (raw), ISO15765 on the
    # registered ISO-TP provider (can_isotp.h), J1850 VPW on the OBD chip.
    PRIV_REQUIRES settings_manager log_manager
                  can_manager obd_gate obd_chip battery_monitor
                  http_server_manager esp_http_server espressif__cjson
                  cmdline_manager console lwip esp_netif esp_mm esp_timer
)
