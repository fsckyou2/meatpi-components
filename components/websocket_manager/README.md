# websocket_manager: WebSocket channel owner (service)

Owns WiCAN's WebSocket CHANNELS: up to 6 named `/ws/...` endpoints
(settings-defined) served by THE one httpd: the component registers routes
with `http_server_manager` (ownership inversion; it never starts a server).
Each channel presents the firmware's standard chunk-stream face (queue RX
via `subscribe`, `send()` TX), so a channel wraps into a `bridge_manager`
endpoint with three one-line functions: OBD↔WS, CAN↔WS, the command line
over WS, all configured bridges, no per-use code here. It knows nothing
about what flows through it.

## API

| Call | Behavior |
|---|---|
| `websocket_manager_init()` | Registers the settings (`"websocket_manager"`) + log descriptors. No routes. |
| `websocket_manager_start()` | Registers the enabled channels' `/ws` routes per the boot-applied settings. **Must run before `http_server_manager_start()`** (main's order guarantees this). Refuses `ESP_ERR_INVALID_STATE` when unconfigured (§4.3 step 5). |
| `websocket_manager_stop()` | Drops the client tables (routes live as long as the server). |
| `websocket_manager_subscribe(channel, q)` | Attach the ONE subscriber queue (items `websocket_chunk_t`). Second subscriber → `ESP_ERR_INVALID_STATE` (a channel is one bridge endpoint). |
| `websocket_manager_unsubscribe(channel, q)` | Detach. |
| `websocket_manager_send(channel, data, len)` | One WS frame (channel's binary/text mode) fanned out to ALL connected clients. |
| `websocket_manager_stats(channel, *out)` | clients, frames/bytes in/out, rx/tx drops, refused. |
| `websocket_manager_channel_name(idx)` | configured channel name per slot (NULL past end): bridge_endpoints registers a jack per configured name. |

## Decided semantics (mirroring socket_manager)

- **Multi-client aggregate**: one channel = one stream. TX fans out one
  frame to every connected client, RX from any client merges into the one
  queue. Per-client addressing: out of scope v1.
- **max_clients** reached → the upgrade is refused **before the 101** (the
  excess client sees the handshake fail, counted in `refused`).
- **Dead clients**: httpd auto-handles PING/CLOSE control frames
  (`handle_ws_control_frames=false`); a closed fd stops reporting
  `HTTPD_WS_CLIENT_WEBSOCKET` and is reaped on the next `send()`
  (`tx_drops`) AND at every handshake (so a quiet channel can't fill with
  ghosts and refuse legitimate clients); the channel keeps running.
- **TCP_NODELAY** is set on every client at handshake: httpd transmits a
  WS frame as two `send()`s (header, payload), with Nagle the payload
  stalls ~40 ms on the peer's delayed ACK (measured; see BENCHMARKS.md).
- **TX never holds the lock across the network**: `send()` snapshots the
  client list, sends outside `s_lock`, re-takes it for reap/stats, RX
  starved measurably otherwise.
- **RX never blocks the httpd worker**: frames are read into request-scoped
  PSRAM (cap 4 KB/frame), chunked into the subscriber queue with
  `xQueueSend(..., 0)`, queue full → `rx_drops`.
- **Reboot-to-apply** (standard §4.2): channels are registered once at
  `start()`.

## The IDF v6 handshake trap (important)

In ESP-IDF v6 httpd **does not call the uri handler at the WebSocket
handshake**: it completes the 101 internally and invokes the handler for
data frames only (`httpd_uri.c`: "If the request is websocket handshake,
then do not call the uri->handler"). Client registration and the
max_clients gate therefore live in **`.ws_pre_handshake_cb`** (returning
non-OK refuses before the 101), which requires
`CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT=y` (set in the root
`sdkconfig.defaults`). A `req->method == HTTP_GET` branch inside the
handler is dead code: that was this component's first (broken) attempt:
clients connected fine but the client table stayed empty and fan-out went
to nobody.

## Settings (`"websocket_manager"`, version 2)

Field-table bounded-array schema (`websocket_manager_settings.c`, standard
§4.1): `channels`, array (maxItems 6) of `{name (a-z0-9_ ≤15), path (must start
"/ws/", ≤31), max_clients 1..4 (default 2), mode binary|text (default
binary), enabled (default false)}`. Cross-item rules (on_validate → pure
`wsm_validate_channels`): unique names, unique paths.

Defaults: the known consumers: `ws_obd` `/ws/obd` binary **enabled**
(ships live, paired with bridge_manager's default `obd<->ws_obd` bridge =
OBD over WebSocket out of the box, 2026-07-05); `ws_log` `/ws/log` text
**enabled** (the log_sinks live log stream: the route only: no line
flows until log_sinks' `ws_enabled` gate, default false, opens); `ws_can`
`/ws/can` binary and `ws_cli` `/ws/cli` text parked (disabled) until
enabled from the UI/API.

**v1→v2 migration** (`on_migrate` → pure `wsm_migrate_channels`,
host-tested): appends the `ws_log` entry to a stored v1 channel array,
skipped when the user already claimed the name/path or the table is full.
maxItems went 4→6 in the same change (`WEBSOCKET_MANAGER_MAX_CHANNELS`,
§12 headroom).

## Files

- `websocket_manager.c`: lifecycle, settings descriptor, name→index API.
- `websocket_manager_ws.c`: the httpd layer: route registration,
  `ws_pre_handshake` gate + client table, data-frame RX, async fan-out TX,
  send-failure/fd-state reaping.
- `websocket_manager_policy.c`: PURE parse/validate (host-testable).

## Tests

- Host suite (`host_test/`, 5 tests on rpi001): parse defaults, text mode,
  the `/ws/` namespace rule, duplicate names/paths, max_clients bounds.
  Run: `tools/testbench/host_tests.sh websocket_manager`.
- Live (2026-07-04, DUT COM2030 + rpi001 via `ws://10.42.0.62/ws/obd`,
  bridge `br_obd = obd <-raw-> ws_obd`, live MIC3624 + ECU sim): `ATI` →
  `ELM327 v2.3`, `VTVERS` → `MIC3624 V2.3.22`, `0100` → `41 00 FF FF FF FF`;
  two-client fan-out byte-identical; third client refused at handshake
  (max_clients 2). **WS LIVE PASS** (`tools/testbench/ws_live_test.py`).
- Benchmarks (2026-07-04, `tools/testbench/ws_bench.py`): see
  `BENCHMARKS.md`, 128 B ~85 KB/s/direction (frame-rate-bound), 1 KB
  WS→TCP 359 KB/s, RTT p50 5.4 ms, OBD polls p50 54.5 ms.

## HTTP observability (endpoint reference: `components/HTTP_API.md` §6e3)

`GET /api/ws` (2026-07-05, served by the `api_http` glue: this component
stays transport-free): configured entries from this component's settings
+ live counters from `websocket_manager_stats()`; `up:false` = configured but not
running. `bridge` (2026-10-16): the enabled bridge on the channel, or
`null` (information, not a fault: `ws_log` is fed by the log sinks, not a
bridge).
