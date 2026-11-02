# esp_wireguard: vendored component

Vendored 2026-07-07 from `components/legacy/esp_wireguard` (originally
the trombik/esp_wireguard port with reference-C crypto: blake2s,
chacha20poly1305, x25519, curve25519 smult). Upstream API untouched:
`esp_wireguard.h` (init/connect/set_default/peer_is_up/disconnect).
`examples/` dropped. Sole consumer: `vpn_manager`.

2026-07-17: the WireGuard CORE this component links
(`components/wireguard_lwip`, shared with microlink/Tailscale) had lost
the legacy tree's `underlying_netif` mechanism: outer encrypted UDP was
sent with `udp_sendto()` (routed), so with `default_route` enabled the
outer packet routed back into the WG netif: infinite encapsulation
recursion → tcpip stack overflow ~25 s after connect (first keepalive).
Re-ported into `wireguard_lwip/src/wireguardif.c`: pin the uplink netif
at init, validate it per send (fall back to any ready non-WG netif for
WiFi→LTE failover), and send outer packets with `udp_sendto_if()`.
Reproduced + verified on bench against a public endpoint over LTE.

2026-11-02: two beta.5 crash reports in task `vpn_state` (a double free in
`tlsf_free`, an InstructionFetchError in `tcp_abandon`) decoded to
`esp_wireguard_disconnect()`'s `netif_set_ipaddr(ANY)`: lwIP's core is
single-threaded (the tcpip thread, or the core lock, which this firmware
does not build), and upstream ran `netif_add`, `netif_set_up`,
`netif_set_default`, `netif_set_ipaddr` and `netif_remove` on the caller's
task; the address change makes lwIP abort and free every TCP connection on
the tunnel address, so a teardown under a live MQTT or Home Assistant
connection raced the tcpip thread on the same PCBs. Every lwIP core
sequence of `esp_wireguard.c` runs inside the tcpip thread now
(`wg_in_tcpip()`: `tcpip_callback` and a semaphore; the netif create, the
peer add, the connect, the default-route take and give-back, the whole
disconnect, the failed-connect cleanup); the peer's `getaddrinfo` stays
on the caller. Reproduced first on a `CONFIG_LWIP_CHECK_THREAD_SAFETY=y`
build (`assert failed: netif_add ... Required to run in TCPIP context!`,
task `vpn_state`, 9 s after boot), the same build silent after the change;
the race bench is `tools/testbench/vpn/wg_teardown_cycle_bench.py`
(`.\test.ps1 wgcycle`). Hand-over: `internal/TASK_wg_teardown_race.md`.
