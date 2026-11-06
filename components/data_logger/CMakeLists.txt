idf_component_register(
    SRCS "data_logger.c"
         "data_logger_writer.c"
         "data_logger_store.c"
         "data_logger_settings.c"
         "data_logger_db.c"
         "data_logger_append.c"
         "data_logger_mf4.c"
         "data_logger_blf.c"
         "data_logger_can.c"
         "data_logger_files.c"
         "data_logger_cursor.c"
         "data_logger_clock.c"
         "data_logger_journal.c"
         "data_logger_events.c"
         "data_logger_http.c"
         "data_logger_export.c"
         "data_logger_gpx.c"
         "data_logger_cli.c"
         "data_logger_recover.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # Feature: producers register params and push records; this owns the
    # sqlite/csv/wdl files on the SD card (batching/rotation/retention,
    # see BENCHMARKS.md for why the pragmas look the way they do). Two
    # streams: params (autopid sink + logger.write rules) and CAN frames
    # (can_manager RX subscription, hence the can_manager dep; autopid
    # stays decoupled, main wires its value sink). The clock journal reads
    # rtc_manager's clock steps and the restart tracker's boot count.
    # /api/logger and the `logger` command are optional (main wires them).
    PRIV_REQUIRES sqlite3 external_storage event_manager dev_status_manager
                  esp_timer rtc_manager restart_tracker
                  http_server_manager settings_manager log_manager
                  cmdline_manager can_manager
)
