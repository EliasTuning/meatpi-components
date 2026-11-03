# microlink: vendored component (Tailscale for ESP32)

Vendored 2026-07-07 from https://github.com/CamM2325/microlink tag
**v2.1.0** (MIT, Malone Technologies LLC). Production Tailscale client:
ts2021 control plane, WireGuard data plane, DERP/DISCO/STUN, MagicDNS.
Sole consumer: `vpn_manager` (type "tailscale"). Design +
decisions: `vpn_manager/TASK_tailscale.md`.

**Testing:** no local unit suite, vendored upstream code isn't forked into
a parallel test tree. Its config server (12 routes) + the ts2021/WireGuard/
DERP stack are validated through `vpn_manager` on the bench
(`TS TARGET PASS`, end-to-end MapResponse + WireGuard data plane).

## Local changes (this repo)

1. **Shared WireGuard core.** Upstream bundled its own
   `components/wireguard_lwip`; it was PROMOTED to a top-level
   `components/wireguard_lwip` and is now shared with `esp_wireguard`
   (both VPN types, one data plane, one set of `wireguardif_*`/crypto
   symbols: the private esp_wireguard copies were deleted to avoid a
   link-time clash). `wireguardif_fini()` was re-added to that shared
   copy for esp_wireguard's teardown (upstream microlink had only
   `wireguardif_shutdown`).
2. **IDF v6:** `json` → `espressif__cjson`; dropped `esp_driver_tsens`.
3. **HTTP config server NOT compiled.** `ml_config_httpd.c` (its own
   web config UI + NVS settings brain + the temperature-sensor dep)
   is Kconfig-gated OFF (`CONFIG_ML_ENABLE_CONFIG_HTTPD=n`, upstream
   default) and dropped from SRCS: WiCAN owns config via
   settings_manager + web_ui (Standard §4, one config brain). The
   header ships inline stubs for that build, so the core compiles
   against them unchanged.
4. **`control_url` config field** added to `microlink_config_t` +
   `microlink_init`: Headscale/Ionscale host override that upstream
   only exposed through the (now-absent) config UI's NVS layer.
5. **Internal-RAM (DONE):** microlink is PSRAM-first (handle, cJSON
   hooks, H2/JSON buffers). 3 of the 4 task stacks moved to PSRAM via
   `ml_start_psram_task()` in `microlink.c` (net_io 8K / derp_tx 14K /
   coord 12K = 34 KB off internal; static-create with internal
   `StaticTask_t` TCBs, stacks freed in destroy). `wg_mgr` stays
   INTERNAL (it writes the peer cache to NVS: §2 corollary). All of
   microlink is runtime-allocated on init, so it costs ZERO while
   tailscale is disabled (vpn_manager only inits it when
   `type=tailscale`). See ARCHITECTURE §12b + wican-internal-ram-budget.
6. **mbedTLS 4 (IDF v6):** RNG migrated off the removed
   entropy/ctr_drbg/`mbedtls_ssl_conf_rng` trio to the global PSA RNG
   (`psa_crypto_init()`); moved-header includes
   `mbedtls/private/{entropy,ctr_drbg,chacha20,chachapoly}.h`;
   `CONFIG_MBEDTLS_CHACHAPOLY_C`/`CHACHA20_C` enabled in the root
   sdkconfig.defaults (off by default in mbedTLS 4; Noise needs them).
7. **`GET /key?v=N` server-Noise-key fetch** (`fetch_custom_server_key`
   in `ml_coord.c`): the ts2021 Noise_IK handshake needs the
   coordinator's static public key; upstream hardcoded Tailscale's and
   documented-but-never-implemented the fetch. Runs when a custom
   `control_url` is set (Headscale/Ionscale).
8. **`host:port` control addressing:** `control_url` now parses an
   optional `:port` into `ctrl_port` (upstream hardcoded 80), used at
   both coordinator dial sites (Headscale default is 8080).
9. **Netmap-on-stream (Headscale compatibility), 2026-07-07.**
   Headscale never answers `Stream=false` full-map requests (its
   `mapSession.serve()` handles only stream / endpoint-update /
   read-only; the reply is an empty 200): upstream stalled forever at
   FETCH_PEERS. Changes in `ml_coord.c`:
   - Empty MapResponse tolerated (`map_deferred_to_stream`), and
     END_STREAM is now also detected on HEADERS frames so the empty
     reply is recognized in ~100 ms instead of the 60 s recv timeout.
   - The `Stream=true` long-poll sends `OmitPeers=false` when the
     initial map was deferred, so the full netmap arrives as the
     stream's first message.
   - `process_map_json()` factored out of `do_fetch_peers` and shared
     with the long-poll path: self-Node, peers, **DERPMap**, key
     expiry all parse from streamed maps too; DERP connect is
     requested as soon as a streamed map delivers the DERPMap.
   - Long-poll messages are reassembled as `[4-byte LE length][JSON]`
     (ts2021 map-stream framing) in a PSRAM buffer: one message may
     span many Noise/H2 frames.
10. **H2 frame accumulation in the long-poll path.** Upstream parsed
    H2 frames only within a single decrypted Noise frame; an H2 frame
    spanning two Noise frames (any MapResponse > ~4 KB, i.e. every
    real netmap) lost its tail and desynced the stream. Persistent
    64 KB PSRAM accumulator (`h2_acc_buf`); only complete H2 frames
    are consumed, partial tails wait for the next read.
    (`do_fetch_peers` always accumulated for exactly this reason.)
11. **Effective DERP region** (`ml_effective_derp_region`): if the
    configured/home region isn't in the parsed DERPMap (custom
    coordinators serve their own region IDs), adopt the map's first
    usable region: used for dialing AND advertised as
    `NetInfo.PreferredDERP` (upstream hardcoded region 9/Dallas and
    then dialed the public derp9e.tailscale.com, which is useless
    against Headscale's embedded DERP).
12. **Plain-TCP DERP** (`derp_io_read/write` in `ml_derp.c`): TLS only
    when the DERP port is 443 (real Tailscale relays); Headscale's
    embedded DERP serves the `/derp` upgrade over plain HTTP on the
    control port. The plain path reuses the mbedTLS BIO callbacks so
    error-code semantics are identical at every call site.

13. **`ML_TASK_WG_MGR_STACK` 8K → 7K** (2026-07-07 pm): with USB host +
    CAN + the full OBD stack active, internal free bottomed at ~24 KB with a
    ~15 KB largest block; vpn_state (6 KB, created first) + an 8 KB
    wg_mgr no longer fit one region and microlink_start failed
    ("Failed to create wg_mgr task"). Watermark measured under a live
    tunnel (registration + map + ICMP + HTTP): wg_mgr uses ~3 KB,
    4.1 KB headroom at 7 KB. vpn_manager's state task went 7168→6144
    the same day (3.9 KB headroom measured).
14. **The public DERP relays on mbedTLS 4** (2026-10-31, issue 1024,
    `src/ml_x509_wrap.c`): every public Tailscale relay ends its TLS
    chain with a self-signed Ed25519 "derpkey" meta certificate. mbedTLS
    has no Ed25519 in X.509 and parses it to the bare UNKNOWN_OID, which
    both TLS chain parsers tolerate, but the TLS 1.3 one
    (`mbedtls_ssl_tls13_parse_certificate`) returns its loop's last
    `ret` from its exit path, so the tolerated code of a LAST certificate
    becomes the handshake's error; the relays negotiate 1.3, firmware 4.x
    ran 1.2 (whose parser returns 0). So every relay handshake failed on
    v6: `TLS handshake failed: X509 - Requested OID is unknown`, no
    relay at all with the real control plane, direct paths only. The two
    parse entry points the TLS layer uses are link-wrapped
    (CMakeLists.txt): the unknown-algorithm outcome in a chain that
    already holds a parsed certificate answers 0 (the parser has already
    unlinked the failed node); nothing else changes for a certificate
    that parses or fails otherwise, and a chain's first certificate is
    never skipped. Behind that door a second TLS 1.3 difference: the relay
    sends session tickets right after the handshake and
    `mbedtls_ssl_read` reports each as
    `MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET` (-0x7B00), which the
    HTTP-upgrade reader took for a fatal error; `derp_io_read` answers
    WANT_READ for it (the ticket is consumed inside the library).
15. **`ml_derp_connect()` frees its TLS context on every failure exit**
    (`derp_connect_fail`, same day): the context (two record buffers and
    the handshake state) was freed only by `ml_derp_disconnect()`, so
    each refused handshake leaked about 21 KB of PSRAM and 230 B of
    internal RAM; three attempts per connect request and a request after
    every map update emptied a WiCAN PRO in minutes (the first name
    lookup of a task then hit IDF's lwIP NULL-semaphore panic, the crash
    of issue 1024). A failed round now arms a backoff in the DERP I/O
    task (10 s doubling to 5 min, `ml_derp_conn_t.retry_after_ms`); the
    request bit stays set so the next round starts by itself. The
    connect path's failures log at W (they are retried), the attempts at
    D, the relay state once per round. `microlink_derp_connected()`
    reports the relay for `/api/vpn` `derp`.
16. **Log levels** (same day): the per-packet and periodic lines of
    `ml_wg_mgr.c` / `ml_net_io.c` (`wireguardif_periodic`, the DISCO
    pings and pongs, `WG RX`/`WG UDP TX`, `UDP RX`) and the DERP task's
    5 s heartbeat and 10 s status line went to debug level: they were
    about 360 lines a minute at idle on the console (Standard §10). The
    coordinator's retried failures (`coord_recv failed`, the Noise msg2
    header, `Noise handshake failed`, `TCP connect failed, retrying`,
    the two DNS lines) log at W, not E: the coord task retries them
    itself (rule 5 of the change rules, zero E lines on a retry path).
17. **The flash hook, and the WireGuard manager on a PSRAM stack**
    (2026-11-01): `microlink_config_t.flash_run` (NULL = upstream
    behaviour, the access in line) carries every NVS access of the client
    (the device keys of `microlink_init`, `ml_peer_nvs.c`'s open, read,
    write and erase, the factory reset's erase) through
    `ml_flash_run()`; WiCAN hands it its internal-stack flash worker
    (`flash_jobs_run`, vpn_manager_ts.c), so `ml_wg_mgr` runs on a PSRAM
    stack like the three other tasks (`ml_start_psram_task`, joined
    before its stack is freed; `ML_PSRAM_TASKS` 4) and the 7 KB internal
    stack it kept for the peer cache is gone, as is vpn_manager's 6 KB
    state-task stack (lever 2 of `internal/TASK_ble_tailscale_margin.md`).
18. **Subnet routes** (2026-11-01): `ml_coord.c` reads each peer's
    `AllowedIPs` beyond its own address (IPv4 only, an exit node's
    `0.0.0.0/0` logged and left out, up to `ML_MAX_PEER_ROUTES` 4 per
    peer) into the peer update; `ml_wg_mgr.c` allows them on the peer's
    WireGuard entry (`wireguardif_peer_add_allowed_ip`, new in the
    vendored wireguard_lwip, whose `WIREGUARD_MAX_SRC_IPS` is 5 now) and
    installs every active peer's routes into `ip4_routes` (`routes_apply`,
    up to `ML_MAX_ROUTES` 8, cleared with the netif); `microlink_get_routes`
    is the snapshot for `/api/vpn`. Upstream never read `AllowedIPs`, so a
    network behind a subnet router was unreachable from the client.

19. **The WireGuard netif attached and detached inside the tcpip thread**
    (2026-11-02, the same class as the two beta.5 `vpn_state` crash reports
    of esp_wireguard): `ml_wg_mgr.c` put the netif onto lwIP's list by hand,
    set it up and created the raw output pcb from its own task, and at the
    task's exit ran `netif_set_down` + `netif_remove` there too; lwIP's core
    is single-threaded and this firmware has no core lock, and `netif_remove`
    on a netif that still has an address aborts and frees every TCP
    connection on it. Both blocks run through `tcpip_callback` and a
    semaphore now (`wg_mgr_in_tcpip`, the `ml_zerocopy.c` pattern). The
    task's other raw calls (`wireguardif_connect`, `_update_endpoint`,
    `_add_peer`, `_remove_peer`, `wireguardif_periodic`, `wireguardif_network_rx`
    on the relay path) still run on the task: they touch the device's own
    peer table and send through the raw pcb; a `CONFIG_LWIP_CHECK_THREAD_SAFETY`
    build names them (`udp_sendto` asserts), see
    `internal/TASK_wg_teardown_race.md` for what the bench showed.
20. **The relay through the peer's own region** (2026-11-03, the field
    report of an MQTT broker behind a home subnet router that a WiCAN on a
    phone hotspot could not reach while the VPN page said connected and
    listed the route): upstream sent every relayed packet through its own
    home region (`ML_DERP_REGION` 9, Dallas, for every device: the port
    never measured a nearer one, and the control server echoes the region
    the client reports), and a relay answers PeerGone for a peer homed
    anywhere else; the port logged the PeerGone and dropped the packet.
    Tailscale sends through the PEER's home region (`derpWriteChanForRegion`
    in magicsock/derp.go), dialling it when needed, or through a region it
    heard the peer on (the reverse route, issue 150), which is why peers
    that contacted the device first always worked. `src/ml_derp_pool.c`
    (new) keeps `ML_DERP_EXTRA_CONNS` (2) connections beside the home one,
    each to a peer's region, opened by the first packet for a peer there,
    closed after `ML_DERP_EXTRA_IDLE_MS` (2 min) without a frame, a refused
    dial backed off 30 s doubling to 5 min with the home relay meanwhile;
    the heard-from table feeds the reverse route and a PeerGone clears it.
    `ml_derp.c`'s primitives take the connection (`ml_derp_conn_t`) instead
    of the context (`src/ml_derp_private.h`), `ml_derp_queue_send` stamps
    the item with the peer's region, the I/O task picks the connection per
    item and polls the extra ones with a zero-timeout select so an idle
    one costs no read timeout. The per-packet TX/RX lines moved to debug
    level (they flooded the log ring at idle). `microlink_derp_home_region`
    is new for the surfaces. Not done: the home region by latency
    (Tailscale's netcheck), which would spare the extra connection in the
    common case and halve the relay path for a device far from Dallas.
21. **Sessions on demand for lwIP traffic, no endpoint until proven**
    (same day, the same report): `wireguardif_output_to_peer` answered
    ERR_CONN for a packet without a session and nothing started a handshake:
    a session came only from the peer initiating, from a DISCO pong over a
    direct path (the one-shot in `process_disco_pong`) or from `ml_tcp.c` /
    `ml_udp.c` (`ml_wg_mgr_trigger_handshake`), which this firmware never
    uses: MQTT, the webhook poster and the destinations are lwIP sockets
    routed into the tunnel netif by `ip4_routes`. The vendored driver gained
    `wireguardif_set_session_wanted` (a callback from the ERR_CONN branch,
    data packets only, on the calling thread); `ml_wg_mgr.c` records the
    want per peer slot (a timestamp written on the tcpip thread) and
    `session_wants_service` starts the handshake through the relay
    (`wireguardif_connect_derp`) or to the direct address DISCO has proven,
    sends a CallMeMaybe, lets the driver's timer retry every REKEY_TIMEOUT
    and makes the peer passive again after `ML_SESSION_WANT_MS` (90 s,
    WireGuard's REKEY_ATTEMPT_TIME) without lwIP asking again. `add_peer`
    no longer installs the map's first endpoint as the WireGuard endpoint
    (with it, every handshake the driver started went to an unproven
    address and never through the relay): a pong installs the direct
    address, as before. The port's comment that a relayed initiation is
    dropped by a peer that trimmed us was wrong (`processDERPReadResult`
    calls `noteRecvActivity` for any known peer); what dropped them was 20.
    One piece of this lives outside microlink: the driver marks the netif's
    LINK down while no peer holds a session (`wireguardif_periodic`), and
    `ip4_routes` would then not route a subnet packet to the tunnel, so the
    traffic that was meant to trigger the on-demand session leaked to the
    station and no session ever started (the deadlock the bench showed:
    `session_wants_service` never fired, the session came only from a stray
    DISCO direct path). `ip4_routes` routes to an admin-up tunnel netif
    whether or not its link is up now (2026-11-03); the dropped packet
    reaches `wireguardif_output`, fires the want, and TCP's retransmit rides
    the session up.

## Upstream layout kept

`microlink.h` public API, `src/ml_*.c` (coord/derp/net_io/wg_mgr/stun/
noise/h2/udp/tcp/peer_nvs/zerocopy), `nacl_box.c`/`x25519.c` crypto,
cellular (`ml_cellular.c`/`ml_at_socket.c`/`ml_net_switch.c`: built
but unused on WiCAN; ML_ENABLE_CELLULAR=n). x25519-license.txt kept.
