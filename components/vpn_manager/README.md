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
one route, the 0.0.0.0 idiom to an empty list), `endpoint`,
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

## Surfaces

- `GET /api/vpn`: `{state, type, endpoint, network, paused, routes, connects, failures, uptime_s}` (`state` is `disabled | waiting | paused | connecting | connected`; `network` = the station's network, "" without one).
- `POST /api/vpn/keygen`: `{public_key, pending_reboot:true}`.
- CLI `vpn`, same status as text.
- Event source `vpn.state {connected, paused, network}`.

## MQTT-behind-the-tunnel

No ordering coupling (meatpi 2026-07-07): mqtt_manager's own retry
loop simply succeeds once the tunnel routes the broker. If retry
latency ever hurts, add a rule on `vpn.state`.

## Footprint

PSRAM: 6 KB state-task stack, plus the two peer-snapshot tables of the HTTP and CLI surfaces (1.3 KB each, PSRAM since 2026-09-22). Internal: FreeRTOS objects + the lwip
netif esp_wireguard creates while up. Host tests: `host_test/`
(6 tests, pure config checks). Bench: WireGuard server on rpi001,
LOCAL-ONLY (TASK §3.1), handshake + ping + MQTT-through-tunnel.
