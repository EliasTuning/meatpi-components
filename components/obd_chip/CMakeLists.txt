idf_component_register(
    SRCS "obd_chip.c"
         "obd_chip_fanout.c"
         "obd_chip_claim.c"
         "obd_chip_probe.c"
         "obd_chip_report.c"
         "obd_chip_bringup.c"
         "obd_chip_events.c"
         "obd_chip_uart.c"
         "obd_chip_cmd.c"
         "obd_chip_parse.c"
         "obd_chip_stslcs.c"
         "obd_chip_settings.c"
         "obd_chip_fw.c"
         "obd_chip_fw_job.c"
         "obd_chip_http.c"
         "obd_chip_cli.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # public header exposes FreeRTOS queue types only (implicit common req)
    PRIV_REQUIRES event_manager esp_driver_uart esp_driver_gpio settings_manager obd_gate
                  log_manager filesystem http_server_manager esp_http_server
                  cmdline_manager
    # The packaged MIC3624 image (legacy parity, Ali 2026-07-26: a chip
    # not at a current version is auto-flashed to it after bring-up).
    # V2.3.24 since 2026-10-28 (issue #1023), the v4.52p policy: a chip at
    # V2.3.22 or V2.3.24 is current (obd_fw_version_is_current in
    # obd_chip_parse.c). Bumping the packaged version = update this path,
    # OBD_FW_BUILTIN_VERSION in obd_chip_fw.c AND that list. Byte-identical
    # to legacy main/obd_fw/V2.3.24.txt (main fab54ad).
    EMBED_TXTFILES "../obd_chip_manager/obd_fw/V2.3.24.txt"
)
