# ble_manager builds only when the BT stack is in the config (the main
# firmware always has CONFIG_BT_ENABLED; test apps of OTHER components may
# not: they discover every component dir but must not drag Bluedroid in).
# The GATT backend follows the sdkconfig BT host choice: Bluedroid
# (ble_manager_gatt.c) or NimBLE (ble_manager_gatt_nimble.c), switch by
# flipping CONFIG_BT_BLUEDROID_ENABLED/CONFIG_BT_NIMBLE_ENABLED (the
# memory/perf A/B, meatpi 2026-07-05). Same on-air contract either way.
if(CONFIG_BT_ENABLED)
    set(blm_srcs "ble_manager.c"
                 "ble_manager_settings.c"
                 "ble_manager_io.c"
                 "ble_manager_pack.c"
                 "ble_manager_channel.c"
                 "ble_manager_http.c"
                 "ble_manager_ctrl.c"
                 "ble_manager_diag.c")
    if(CONFIG_BT_NIMBLE_ENABLED)
        # GAP/security/notify path + the service table (stream channels
        # are NimBLE-only; the Bluedroid backend stubs them); the bond
        # store's writes on the flash worker (2026-11-01)
        list(APPEND blm_srcs "ble_manager_gatt_nimble.c"
                             "ble_manager_gatt_tx.c"
                             "ble_manager_gatt_svc.c"
                             "ble_manager_gatt_adv.c"
                             "ble_manager_store_nvs.c")
    else()
        list(APPEND blm_srcs "ble_manager_gatt.c")
    endif()
else()
    set(blm_srcs "")
endif()

idf_component_register(
    SRCS ${blm_srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # Public header exposes only esp_err/FreeRTOS queue types -> all private.
    # (Legacy ble.c stays in this directory as the on-air reference: it is
    # deliberately NOT compiled.)
    # flash_jobs: the NimBLE host task runs on a PSRAM stack, its NVS work
    # on the worker (2026-11-01)
    PRIV_REQUIRES bt settings_manager log_manager dev_status_manager esp_timer
                  http_server_manager esp_http_server espressif__cjson
                  flash_jobs
)

# WiCAN (2026-11-01, lever 3 of internal/TASK_ble_tailscale_margin.md): the
# NimBLE host task runs on a PSRAM stack, so the bond store's NVS writes,
# which NimBLE issues from that task, go through the flash_jobs worker:
# the persist functions of IDF's ble_store_nvs.c are wrapped at link time
# (ble_manager_store_nvs.c; the ip4_routes idiom, the -u beside each wrap
# makes the linker take the wrapper out of this static library at its
# first scan). Only with the NVS store compiled in: without it the persist
# functions are inline stubs of ble_store_config_priv.h and a wrapper
# would have no __real_ symbol. The two optional ones follow their
# Kconfig options for the same reason. The load (ble_store_config_init)
# is wrapped too: the host calls it from its own task at every startup
# (ble_hs_pvcy_set_default_irk under CONFIG_BT_NIMBLE_STATIC_TO_DYNAMIC).
if(CONFIG_BT_ENABLED AND CONFIG_BT_NIMBLE_ENABLED AND CONFIG_BT_NIMBLE_NVS_PERSIST)
    set(blm_persist ble_store_config_init
                    ble_store_config_persist_our_secs
                    ble_store_config_persist_peer_secs
                    ble_store_config_persist_cccds
                    ble_store_config_persist_csfcs
                    ble_store_config_persist_local_irk
                    ble_store_config_persist_rpa_recs)
    if(CONFIG_BT_NIMBLE_ENC_ADV_DATA)
        list(APPEND blm_persist ble_store_config_persist_eads)
    endif()
    if(CONFIG_BT_NIMBLE_HOST_BASED_PRIVACY)
        list(APPEND blm_persist ble_store_persist_peer_records)
    endif()
    foreach(sym ${blm_persist})
        target_link_libraries(${COMPONENT_LIB} INTERFACE
                              "-Wl,--wrap=${sym}"
                              "-u __wrap_${sym}")
    endforeach()
endif()
