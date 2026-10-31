# ip4_routes: destination routes for lwIP (the split tunnel)

**Summary.** lwIP routes an outbound packet into the netif whose own network
contains the destination, so a VPN netif reaches exactly one subnet: the one its
netmask names. Issue #975 (2026-10-10) needed a WireGuard tunnel that carries a
few private networks while a public URL keeps using the normal connection, which
"route all traffic" cannot give. This component keeps a small table of
{network, mask} -> netif and answers lwIP's IPv4 route lookup from it before
lwIP's own netif walk. Sits with the core services (below the VPN); `main` calls
`ip4_routes_init()` before `vpn_manager_init()`.

**How it gets in front of lwIP.** IDF's lwIP port defines
`LWIP_HOOK_IP4_ROUTE_SRC` as its own `ip4_route_src_hook` (source-address
routing, `components/lwip/port/hooks/lwip_default_hooks.c`), a strong symbol
with no Kconfig choice (only the IPv6 hook has one). The CMakeLists adds
`-Wl,--wrap=ip4_route_src_hook`, so lwIP's calls (from `ip4_route()` with a
NULL source and from `ip4_route_src()` with a bound one) land in
`__wrap_ip4_route_src_hook`: the destination is matched against the table
(longest prefix wins; the netif must be up with its link up) and anything
else falls through to `__real_ip4_route_src_hook`, IDF's hook. A
`-u __wrap_ip4_route_src_hook` beside the wrap makes the linker take the
wrapper out of this static library at its first scan: without it a link
that nothing else ties to the component (an on-target test app, which
builds every component in the tree) applied the wrap and ended in an
undefined reference to the wrapper (found 2026-10-29). The same
pattern wraps `esp_panic_handler` (restart_tracker) and
`twai_hal_read_rx_fifo` (can_manager); re-check it at every IDF bump.

**A directly connected network wins (2026-11-01).** Before the table's
answer is taken, the hook walks the other netifs that are up: a destination
inside one's own network stays on that netif, the way lwIP's own walk and
every IP stack route, and the way Tailscale keeps a LAN local while a subnet
router advertises that very LAN (its routing-table rule with
`suppress_prefixlength 0`). Without it a WiCAN at home, on the network its
home subnet router advertises (the Tailscale subnet routes microlink learns
from the map since the same day), would send its own LAN's traffic into the
tunnel; the same held for a WireGuard `routes` entry naming the station's
network. The default route (`netif_set_default`) never had this problem: a
connected network was always more specific than it.

**Owners.** vpn_manager's WireGuard glue (the `routes` setting, up to 8) and
microlink's WireGuard manager (the subnet routes a tailnet's subnet routers
carry, up to 8 across the peers, `ml_wg_mgr.c` routes_apply); one VPN type
runs at a time, so the 16-slot table holds either.

## API (`include/ip4_routes.h`)

| Function | What |
|---|---|
| `ip4_routes_init()` | the log descriptor; no side effects |
| `ip4_routes_set(netif, entries, n)` | replace that netif's routes (n == 0 clears); `ESP_ERR_NO_MEM` + one E line when the table would overflow, untouched then |
| `ip4_routes_clear(netif)` | drop that netif's routes; call BEFORE the netif is removed |
| `ip4_routes_capacity(&used, &cap)` | occupancy for the `WICAN CAPS` line (`ip4_routes`) |
| `ip4_routes_parse_cidr(text, &e)` | pure: "a.b.c.d/n", a bare address is /32, host bits cleared |
| `ip4_routes_parse_list(list, out, max, err, len)` | pure: a comma or space separated list; the count, or -1 with the offending token or "more than N routes" in `err` |
| `ip4_routes_format(&e, out, len)` | pure: "a.b.c.d/n" |

Entries are host order with the network pre-masked. The table holds
`IP4_ROUTES_MAX` (16) routes for every owner together; vpn_manager uses up to 8.

## Dependencies

`lwip` (the netif type, the address helpers), `log_manager`. Owners today:
`vpn_manager` (the WireGuard netif; the Tailscale netif keeps its own /10
netmask and needs no table).

## Settings

None. The lists come from the owners' settings (vpn_manager `routes`).

## Memory footprint

Internal RAM: the table, 16 x 12 bytes + the count (the routing hot path
reads it for every outbound packet, under a spinlock), measured 0 heap.
No task.

## Testing

`host_test/` (Unity, linux target): the CIDR parser and formatter, the list
parser, and the table's set / clear / longest-prefix match (16 tests).
On the device: `tools/testbench/vpn/vpn_routes_bench.py` (`VPN ROUTES PASS`,
TESTING.md) proves a request to a routed network leaves through the tunnel
and one to any other address through the station, both judged at the
receiver by the source address.
