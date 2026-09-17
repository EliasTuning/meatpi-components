idf_component_register(
    SRCS "event_manager.c"
         "event_manager_settings.c"
         "event_manager_registry.c"
         "event_manager_rules.c"
         "event_manager_eval.c"
         "event_manager_template.c"
         "event_manager_http.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # The automation engine: registries + rules + dispatcher. Depends on
    # settings/log/esp_timer/cJSON ONLY (§9 layering) — every component
    # with events/actions points AT it via its own <comp>_events.c.
    REQUIRES espressif__cjson
    PRIV_REQUIRES settings_manager log_manager esp_timer
                  http_server_manager esp_http_server
)
