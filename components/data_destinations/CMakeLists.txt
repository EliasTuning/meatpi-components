idf_component_register(
    SRCS "data_destinations.c"
         "data_destinations_core.c"
         "data_destinations_abrp.c"
         "data_destinations_settings.c"
         "data_destinations_post.c"
         "data_destinations_http.c"
         "data_destinations_cli.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # settings/log descriptors; the /api/destinations routes; the console
    # command; outbound MQTT + HTTP(S); the payload sources (autopid
    # snapshot + cached config, dev_status identity/uptime, battery
    # voltage); cert sets are consumed THROUGH http_client_manager
    PRIV_REQUIRES settings_manager log_manager http_server_manager
                  cmdline_manager http_client_manager mqtt_manager
                  autopid dev_status_manager battery_monitor console
                  esp_http_server esp_timer
)
