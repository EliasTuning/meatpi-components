idf_component_register(
    SRCS "vpn_manager.c"
         "vpn_manager_settings.c"
         "vpn_manager_wg.c"
         "vpn_manager_ts.c"
         "vpn_manager_check.c"
         "vpn_manager_keygen.c"
         "vpn_manager_events.c"
         "vpn_manager_http.c"
         "vpn_manager_cli.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # Feature: THE VPN client, user-selected `type`: plain WireGuard
    # (vendored esp_wireguard API) or Tailscale (vendored microlink).
    # Both share ONE WireGuard core (wireguard_lwip). /api/vpn and the
    # `vpn` command are optional (main wires them).
    # wifi_manager: the per-network "Pause VPN" flag (issue #976), asked on
    # the state task's lap; feature -> service, no cycle.
    # flash_jobs (2026-11-01): microlink's NVS work runs on the flash worker
    # through the client's flash hook, so the state task and the client's
    # tasks live on PSRAM stacks.
    PRIV_REQUIRES esp_wireguard wireguard_lwip microlink esp_netif lwip mbedtls esp_hw_support
                  dev_status_manager rtc_manager event_manager wifi_manager ip4_routes
                  http_server_manager settings_manager log_manager
                  cmdline_manager flash_jobs
)
