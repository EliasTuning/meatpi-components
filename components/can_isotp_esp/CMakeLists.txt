idf_component_register(
    SRCS "can_isotp_esp.c"
         "can_isotp_esp_mbox.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # Service: the public build's ISO-TP provider for can_manager's
    # can_isotp.h slot — the vendored esp_isotp stack in its externally-
    # managed mode over the shared bus (can_manager keeps owning the TWAI
    # node). Registers ONLY when no add-on pack has (main calls
    # can_isotp_esp_register right after ext_manager_init).
    PRIV_REQUIRES can_manager esp_isotp log_manager esp_timer
)
