# vpn_manager

WireGuard client (rewrite of legacy `vpn_manager`, decisions in
`TASK_vpn_manager.md`). One peer described by settings; a small state
task connects when the network is up AND the clock is valid
(rtc_manager: WireGuard handshakes carry timestamps), verifies the
handshake, publishes `DEV_STATUS_BIT_VPN_ENABLED` + the
`vpn.state {connected, paused, network}` event, reconnects with 5→60 s
backoff on failure, network loss, or 30 s of peer silence, and pauses
(tunnel down on purpose) while the station is on a network the user
marked `Pause VPN` (below).

Crypto/tunnel = the vendored [components/esp_wireguard](../esp_wireguard/)
port (trombik; PROVENANCE.md): this component is its only consumer.

## Settings (`vpn_manager`, v2, reboot-to-apply)

`enabled` (false), `type` (wireguard), `private_key`*,
`peer_public_key`, `preshared_key`* (optional), `address` (local
tunnel IP, optional /cidr: ALWAYS the WG netif address, netmask /32),
`routes` (v2, issue #975: a comma list of up to 8 networks in CIDR form
routed into the tunnel through `ip4_routes`, the tunnel's own network
included, e.g. `10.66.0.0/24, 192.168.1.0/24`; `""` = nothing but
`default_route`; a v1 `allowed_ip` + `allowed_ip_mask` pair migrates to
one route, the network v1 actually routed: the tunnel `address` under
that mask, since the AllowedIPs address itself only ever set the netmask
(issue #988, 2026-10-16: the first cut of the migration took the
AllowedIPs network, so address 10.6.0.2 with AllowedIPs 192.168.86.0/24
lost the peer's subnet and pulled the LAN into the tunnel; a unit that
migrated on alfa-04 keeps its migrated list, check Routes there); the
0.0.0.0 idiom migrates to an empty list), `endpoint`,
`port` (51820),
`keepalive_s` (25), `default_route` (false: meatpi: only the allowed
range routes), `dns` (optional override, saved/restored around the
connection), `cli` (true).

\* secret fields: redacted ("" ) in settings GETs, "" on PUT keeps the
stored value (api_http suffix rule: `_password`, `private_key`,
`preshared_key`). Backup export carries them verbatim.

Cross-field validation happens at PUT (`on_validate` → readable 400):
enabled configs need well-formed 44-char base64 keys, an endpoint and
an address.

## Keys never leave the device

`POST /api/vpn/keygen` generates a Curve25519 pair on the device
(esp_fill_random + x25519_base, buffers wiped), stores the private key
directly into pending settings, and returns ONLY the public key for
registration at the server. Reboot applies.

## Pause on marked networks (issue #976, 2026-10-11)

The user marks networks with `Pause VPN` under Settings > WiFi > Station
(wifi_manager's `sta_vpn_pause` / `fallbackN_vpn_pause`, a switch on the
main network, a tick beside `trusted` on every backup). While the station
is connected to a marked network the state task parks in
`VPN_STATE_PAUSED`: the tunnel is torn down (the DNS override and the
default route with it), no handshakes, no keepalives, no session held at
the server. It asks `wifi_manager_sta_vpn_pause()` on its one-second lap,
so a pause starts within a second of the station landing on a marked
network (from CONNECTED, from an attempt in flight, or at boot) and ends
the moment it leaves, with the backoff at its 5 s minimum. The pause reads
the WiFi station only: a USB Ethernet uplink or the dongle over USB never
pauses the VPN. Surfaces: `state: "paused"` + `network` + `paused` on
`/api/vpn`, the `vpn.state {connected, paused, network}` event, the `vpn`
command's `paused on <network>`, one I line per transition.

## Split tunnel (issue #975, 2026-10-11)

lwIP routes a packet into the netif whose own network holds the
destination, so the WireGuard netif used to reach exactly one subnet
(the one `allowed_ip_mask` named) and "route all traffic" took the
public Home Assistant URL into the tunnel with it. The `routes` list is
handed to [ip4_routes](../ip4_routes/) when the tunnel comes up (after
`esp_wireguard_connect`) and withdrawn before the netif is removed:
lwIP's IPv4 route hook (wrapped there) answers from the table, longest
prefix first, and everything else leaves over the normal connection.
`default_route` is unchanged (the netif becomes the default one) and
overrides the list. The library's own cryptokey routing is wide open
(peer allowed IPs 0.0.0.0/0), so it needs no change. The `vpn` command
and `GET /api/vpn` list the routes. Bench: `tools/testbench/vpn/vpn_routes_bench.py`
(`VPN ROUTES PASS`): a request to a routed network arrives at the
receiver from the tunnel address, one to any other address from the
station's.

## Tailscale: the relay, the leak and the internal-RAM floor (issue 1024, 2026-10-31)

Two field reports (a crash loop with a self-park, and "DNS and connectivity
issues after a reboot") were one chain: every public DERP relay was refused
at its TLS handshake on mbedTLS 4 (the relay's Ed25519 meta certificate,
see `microlink/PROVENANCE.md` 14), every refused attempt leaked the TLS
context (15), the device ran out of internal RAM within minutes, and a
task's first name lookup then hit IDF's lwIP NULL-semaphore panic
(`lwip_guard` turns that into a failed call). The relay works again and the
leak is gone, which leaves the structural budget: Tailscale's bring-up takes
about 20 KB of internal RAM on a bench unit (51.9 KB settled with the dongle
and BLE off, 33.1 KB at the connect, 31.5 KB steady), BLE about 40 KB on
this build (13 KB free at the floor check with it on, 5 KB a minute later),
and both together left 1.2 KB at the connect: sockets, the SD card's DMA
buffers and the web server failed. So Tailscale starts only with
`VPN_TS_INTERNAL_FLOOR_KB` (30 KB) free at that moment; otherwise the state
task holds in `waiting` with a reason built by `vpn_ts_hold_reason()` (pure,
host-tested) from the device's own settings (Bluetooth on, the WiFi memory
profile not Lean, else WireGuard), logs one W line per hold, and starts by
itself when RAM frees up. The reason is on `/api/vpn` `reason` and the `vpn`
command (`held: ...`). The relay's state is `/api/vpn` `derp` (`relay up` /
`relay down` on the console); the `connected (...)` log line carries the
internal RAM left at that moment. Bench: `ts_public_soak.sh` (the DERP leg,
the internal-RAM-flat leg; `.\test.ps1 tspublic`).

## Surfaces

- `GET /api/vpn`: `{state, type, endpoint, network, paused, routes, ts_ip, ts_peers, derp, reason, connects, failures, uptime_s, peers}` (`state` is `disabled | waiting | paused | connecting | connected`; `network` = the station's network, "" without one; `derp` (2026-10-31, issue 1024) = the Tailscale DERP relay connection is up, false for wireguard, while the relay is in backoff and before the first map; `reason` = why the tunnel is held in `waiting`, "" otherwise, see the internal-RAM floor below; `routes` = for wireguard the `routes` setting's networks, for tailscale (2026-11-01) the subnet routes learned from the tailnet, see below).

## Subnet routes on the tailnet (2026-11-01)

A tailnet node that advertises a network as a subnet router (`tailscale up
--advertise-routes=192.168.1.0/24`, approved on the admin console or on
headscale) carries that network in its `AllowedIPs` in the map, beside its
own address. A phone accepts such routes by default; the WiCAN ignored them
(a Discord question, 2026-11-01: an MQTT broker at home behind a subnet
router, reachable from the phone, "WiCAN says it can't connect"), because
microlink installed every peer with its own address only and lwIP sent the
broker's address out of the station. Since 2026-11-01 microlink takes every
IPv4 network of a peer's `AllowedIPs` beyond its own address (up to 4 per
peer, 8 across the tailnet; an exit node's `0.0.0.0/0` is logged and not
taken, IPv6 is not), allows it on the peer's WireGuard entry (outbound
lookup and inbound source check) and installs it in `ip4_routes`, the
destination table in front of lwIP's netif walk, where a directly connected
network always wins (a WiCAN at home on the advertised LAN keeps talking to
it directly). The routes follow the map: a peer removed or a route
withdrawn leaves the table at the next map update, every route goes with
the tunnel. `GET /api/vpn` lists them as `routes` and the `vpn` command
prints `route <cidr> via <tailnet address>`; the log line `ml_wg_mgr:
subnet route 10.99.0.0/24 via betty-peer` names each one. No setting:
accepting the tailnet's approved routes is the phone's behaviour; a device
that must not take one needs the route withdrawn or disapproved on the
control server. Bench: `ts_public_soak.sh` (the subnet router legs:
betty advertises `10.99.0.0/24`, headscale approves it, a destination test
to an address in it arrives at the receiver from the DUT's tailnet
address; `.\test.ps1 tspublic`).
- `POST /api/vpn/keygen`: `{public_key, pending_reboot:true}`.
- CLI `vpn`, same status as text.
- Event source `vpn.state {connected, paused, network}`.
- `vpn_manager_state_str(state)` (public header): the state word above,
  the ONE table every surface uses, the HA push's `vpn_status` included
  (issue #985, 2026-10-16: ha_webhooks kept a four-entry copy that
  `VPN_STATE_PAUSED` outgrew, so `connected` indexed past it and `paused`
  read "connecting"). Never NULL.

## API (`include/vpn_manager.h`)

| Call | Behavior |
|---|---|
| `vpn_manager_init()` | registers the settings, log and event descriptors; no network access |
| `vpn_manager_start()` | creates the state task when enabled (network-gated inside) |
| `vpn_manager_stop()` | tears the tunnel down; the state task parks |
| `vpn_manager_status(out)` | the snapshot: state, type, paused, network, endpoint, counters |
| `vpn_manager_state_str(state)` | the wire word of a state (pure, host-tested) |
| `vpn_manager_tunnel_ip(buf, len)` | the device's own tunnel address while connected, else `ESP_ERR_INVALID_STATE` |
| `vpn_manager_keygen(pub, len)` | a device-side keypair; the private key goes to pending settings, only the public key comes back |
| `vpn_manager_register_http()` | the `/api/vpn` routes (main wires it) |
| `vpn_manager_register_cli()` | the `vpn` command (called from the settings apply behind `cli`) |

## MQTT-behind-the-tunnel

No ordering coupling (meatpi 2026-07-07): mqtt_manager's own retry
loop simply succeeds once the tunnel routes the broker. If retry
latency ever hurts, add a rule on `vpn.state`.

## Footprint

PSRAM: the two peer-snapshot tables of the HTTP and CLI surfaces (1.3 KB each, PSRAM since 2026-09-22), the two route-snapshot tables (2026-11-01, 320 B each), the 176 B hold reason, and since 2026-11-01 the 6 KB state-task stack (`vpn_manager.c`: it was internal because the Tailscale start reads and writes the device keys in NVS from it, `WICAN RAMMAP vpn_manager=6520`; microlink runs that work through its `flash_run` hook now, which `vpn_manager_ts.c` sets to `flash_jobs_run`, the shared internal-stack worker, and the WireGuard manager task of the client moved with it: lever 2 of `internal/TASK_ble_tailscale_margin.md`). The TCB stays internal (FreeRTOS). `.\test.ps1 stackaudit` asserts both placements whenever the tasks exist; `tspublic` asserts them with the tunnel up.

**Where Tailscale's internal RAM goes (mapped 2026-10-31 on the public rig, `.\test.ps1 tspublic`, the per-step lines of `vpn_ts_up()`, `microlink_start()`, the relay and the coordinator; a fresh boot with 53.6 KB free before the start):**

| step | internal RAM | what it is |
|---|---|---|
| `microlink_init` | 4.0 KB | the client's state, queues and event group (FreeRTOS objects are internal), the device keys from NVS |
| `microlink_start`: the DISCO socket | 0.25 KB | one UDP socket |
| the net_io, derp_tx and coord tasks | 0.1 KB | their 34 KB of stacks are PSRAM; only the TCBs stay |
| the `ml_wg_mgr` task | 7.5 KB | its 7 KB stack is internal because it writes the peer cache to NVS |
| the connect (control TCP + Noise, the map stream, the WireGuard netif, the `ml_udp_rx` task's 4 KB internal stack, the sockets) | 9.0 KB | 41.6 KB before, 32.6 KB at `connected` |
| the DERP relay | 0.3 KB | the TLS session is PSRAM (mbedTLS external allocation) |
| the first peer | 0.8 KB | 32.1 KB at the map with the peer, 31.3 KB steady and flat over the soak |
| the lowest point of the first minute | 27.9 KB | 3.4 KB below steady, the bursts of the map and the handshakes |

With the BLE controller's code in flash (`CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY`, the evening's build 1) the same start read 73.0 KB before and 51.5 KB steady, the same 21.5 KB, and with Bluetooth on the Lean profile beside it (40.9 KB before the start) Tailscale connected and held 18.6 KB steady, 15.2 KB at the lowest point: the first run of both together (`.\test.ps1 tspublic` PASS, ble_manager README "the lever that did reach"). The `ml_udp_rx` stack is PSRAM since that build, which changed nothing here: the client's UDP socket API is never used on the WiCAN's path.

So Tailscale cost 22.3 KB steady and 25.7 KB at its lowest point until 2026-10-31, and three task stacks accounted for 17.5 KB of it: `ml_wg_mgr` 7 KB (NVS peer cache), `vpn_state` 6 KB (NVS device keys at the start) and `ml_udp_rx` 4 KB (nothing in its path touches flash). **Since 2026-11-01 the two NVS writers run on PSRAM stacks** (their flash work on the `flash_jobs` worker through microlink's `flash_run` hook), and the same stage on the same unit read: 93.8 KB free before the start (the HTTP server's and the vehicle writer's moves of the previous night on top), `microlink_init` 8.4 KB (the client's 4 KB plus the flash worker's 4.4 KB, created by the first key read and gone 5 s later), `microlink_start` 0.5 KB (the WireGuard manager's row reads 0: it was 7.5 KB), 74.6 KB at `connected` (the lowest point, 19.2 KB below the start), 79.2 KB at the relay's handshake, 84.0 KB steady: **about 10 KB steady, 19 KB at the dip**. The floor rule (the dip plus a 10 KB margin, rounded up) therefore keeps `VPN_TS_INTERNAL_FLOOR_KB` at 30; what moved is the steady cost, which is what Bluetooth and Tailscale together live on. The NimBLE bond store needs the same worker pattern for the host task's 4 KB (lever 3, not built). The per-packet buffers, the TLS contexts and the map's JSON are PSRAM already (`CONFIG_SPIRAM_USE_MALLOC`, allocations above 32 B). Internal: FreeRTOS objects + the lwip
netif esp_wireguard creates while up. **Tailscale, measured 2026-10-31 on
the bench unit (ESP-IDF 6.1, the ESPNetLink on the USB port, BLE off, the
WiFi Lean profile):** the internal floor settles at 51.9 KB with the VPN
off; the Tailscale bring-up (microlink's tasks, queues, the WireGuard
netif, the sockets, the relay) leaves 33.1 KB at the connect and 31.5 KB
steady (largest block 23.5 KB, the minimum of the first minute 27.7 KB),
so about 20 KB; flat afterwards (32219 B over 5 minutes, 30919 to 31531 B
over the bench's 60 s leg). PSRAM: about 0.7 MB while up (the HTTP/2
and map buffers, the relay's TLS context). Host tests: `host_test/`
(12 tests: the pure config checks, the routes list, the v1 route
migration, the state words, the hold reason). Bench: WireGuard server on
rpi001, LOCAL-ONLY (TASK §3.1), handshake + ping + MQTT-through-tunnel;
Tailscale on the betty rig (`ts_public_soak.sh`, `.\test.ps1 tspublic`).
