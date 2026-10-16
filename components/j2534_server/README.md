# j2534_server

> Wire protocol (all transports): [`J2534_WIRE_PROTOCOL.md`](J2534_WIRE_PROTOCOL.md).
> Windows DLL: `drivers/j2534/` in the WiCAN app repo. BLE binding:
> `ble_j2534` + `ble_manager/BLE_API.md`.

## Summary

SAE J2534 PassThru device: a tester (the Windows DLL, a phone app, a
bench script) opens a session, connects CAN and ISO15765 channels, writes
messages, sets filters and periodic messages, and receives bus traffic as
unsolicited `RX_MSG` frames. One framed wire protocol runs over three
transports: the TCP listener in this component (port 6809), USB CDC-ACM
(`usb_cdc_device`) and BLE (`ble_j2534`, since 2026-09-21). Exactly one
tester at a time across all of them; a second one is answered
`ERR_DEVICE_IN_USE`.

The CAN bitrate is the device's, not the tester's (2026-10-03): the native
node runs at the `can_manager` setting, or at what its listen-before-talk
policy read off the bus. `CONNECT` (CAN or ISO15765) with another baud is
answered `ERR_INVALID_BAUDRATE` instead of a channel that reads and writes
at the wrong rate (the baud was ignored before). With `can_manager.baud =
auto` and nothing proven yet the tester's baud is taken when the bus has
been silent (2026-10-17, `can_manager_adopt_baud()`: a gatewayed OBD port
speaks only when asked, and a tester there read the refused writes as
"connected, no comm to the CAN bus"); the node runs at it until frames
prove it or the watchdog demotes it. When it cannot be taken (the bus not
silent, unreadable traffic seen, a rate this node does not know) the
channel opens and writes answer `ERR_FAILED`: one `W` line per `CONNECT`,
`tx_refused` on `/api/j2534`, and can_core's own `W transmit refused`
line names the cause. Pure rule for the check: `j2534_connect_baud_check()`.

Layout (each file under the 700-line cap):

| File | Role |
|---|---|
| `include/j2534_proto.h`, `j2534_proto.c` | PURE codec + constants: header/message encode/decode, filter match, status names, the transport-agnostic framer (`j2534_frame_read`), `j2534_ack_encode`, the TCP gate predicate. Host-tested |
| `j2534_server.c` | core: lifecycle, request dispatch (`j2534_srv_handle_frame`), periodic messages, the RX pump, status, the Exclusive-bus hold |
| `j2534_server_transport.c` | the `j2534_transport_t` seam: session lock, send lock, coalesced frame writes, `j2534_server_serve_transport()`, the TCP listener + `allow_lan` gate, the legacy serial vtable adapter |
| `j2534_channel.c/.h` | channels: CAN via `can_manager`, ISO15765 via the registered ISO-TP provider (`can_isotp.h`; `can_isotp_esp` in public builds), filters, the reflash gate |
| `j2534_server_settings.c` / `_http.c` / `_cli.c` | descriptor, `/api/j2534`, the `j2534` command |

## API

| Function | Purpose |
|---|---|
| `j2534_server_init/start/stop()` | lifecycle; `start` is a no-op when `enabled` is false |
| `j2534_server_status(out)` | `j2534_server_status_t` incl. `transport` (`tcp`/`serial`/`ble`/`none`) |
| `j2534_server_is_enabled()` | the setting (stable across runtime stop): BLE glue keys its channel registration on it |
| `j2534_server_is_running()` | started and enabled |
| `j2534_server_set_exclusive(on)` / `_exclusive()` | the runtime Exclusive-bus switch |
| `j2534_server_serve_transport(t)` | run one tester session on a `j2534_transport_t` (`name`, `read(ctx,buf,n,timeout_ms)` >0 / 0 timeout / <0 link down, `write(ctx,buf,n)` one whole frame per call). Blocks for the session; returns at once when not running; refuses with `ERR_DEVICE_IN_USE` when a tester holds the session |
| `j2534_server_set_serial_transport(t)` / `_serve_serial()` | the legacy CDC-ACM vtable, thin wrappers over the above (kept for `usb_cdc_device`) |
| `j2534_server_register_http()` | `GET/POST /api/j2534` |

A transport owner runs a task that waits for its link, drains stale
bytes, calls `serve_transport`, and throttles 200 ms when it returns
early (`usb_cdc_device.c`, `ble_j2534.c` are the two templates).

## Dependencies

`can_manager`, `obd_gate` (diagnostics hold + per-conversation gate), the
ISO-TP provider slot (`can_isotp.h`), `settings_manager`, `log_manager`,
`http_server_manager`, `cmdline_manager`, lwIP, `esp_netif`, `esp_timer`,
cJSON.

## Settings (`j2534_server`, version 1, reboot-to-apply)

| Key | Default | Meaning |
|---|---|---|
| `enabled` | `false` | the whole feature (TCP listener, serial and BLE sessions, the BLE characteristics) |
| `port` | `6809` | TCP listener port |
| `allow_reflash` | `false` | let UDS 0x34..0x37 through (ECU programming) |
| `allow_lan` | `false` | TCP only: also accept connections arriving over WiFi STA / USB-Ethernet uplinks |
| `exclusive` | `true` | boot default of the runtime switch: AutoPID off the bus while a tester is attached |
| `cli` | `true` | register the `j2534` command |

## Memory footprint (estimated; PSRAM unless noted)

| What | Size |
|---|---|
| `s_req_payload` (request payload), `s_rx_wire`, `s_tx_frame` (one whole frame) | 4384 + 4160 + 4164 B |
| `j2534_msg_t` scratch: RX pump 1, handler 4, periodic slots 4 (each ~4.2 KB) | ~38 KB |
| tasks `j2534` (TCP listener) + `j2534_rx` (RX pump): 4 KB stacks each | 8 KB (+ 2 TCBs internal) |
| mutexes (send, session) | internal, ~180 B |

**Never put a `j2534_msg_t` on a task stack** (three of them overflowed a
4 KB stack and reset the device, 2026-07-07). No flash writes anywhere in
this component.

## HTTP / CLI

`GET /api/j2534` (status incl. `transport`), `POST /api/j2534
{"exclusive":bool}`; `j2534` prints the same. Documented in
`components/HTTP_API.md` 6e13 and `cmdline_manager/README.md`.

## Testing

- Host: `host_test/` (15 Unity cases: codec round trips and bounds,
  filter match, status names, the framer with partial / delayed /
  dropped / malformed / discarded input, the ACK vectors of
  `J2534_WIRE_PROTOCOL.md` section 7, the TCP gate predicate).
- Benches (`tools/testbench/usb/`): `j2534_bench.py` (TCP handshake +
  `--reflash`), `j2534_transport_probe.py` (raw CAN + ISO15765 on the
  bus), `j2534_gate_test.py` (reflash gate), `j2534_serial_test.py`
  (CDC-ACM); (`tools/testbench/ble/`): `ble_j2534_bench.py` +
  `ble_j2534_pi.py` -> `BLE J2534 PASS`. `TESTING.md` rows.
