idf_component_register(
    SRCS "obd_gate.c"
         "obd_gate_core.c"
         "obd_gate_diag.c"
         "obd_gate_settings.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # Policy: serializes MIC-chip vs ESP-engine conversations on the one
    # physical CAN bus. Depends only on core (settings/log) — both
    # requesters point DOWN at it.
    PRIV_REQUIRES settings_manager log_manager esp_timer
)
