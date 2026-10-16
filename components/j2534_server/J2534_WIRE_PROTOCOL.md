# WiCAN Pro: J2534 PassThru wire protocol (version 1)

> The one contract shared by the Windows J2534 DLL (`wican-j2534-driver`),
> the Python benches (`tools/testbench/usb/j2534_bench.py`,
> `j2534_serial_test.py`, `tools/testbench/ble/ble_j2534_pi.py`) and phone
> apps that speak PassThru over BLE. The device side is `j2534_server`
> (`include/j2534_proto.h` is the codec these tables are taken from; the
> byte-exact examples below are the host-test vectors). Transport
> bindings in section 8. Changes to `j2534_proto.h` update this file in
> the same commit.

## 0. Scope

| | |
|---|---|
| Wire version | 1 (`HELLO` answers it) |
| Transports | TCP (port 6809), USB CDC-ACM serial, BLE (stream channel FFF5/FFF6) |
| Protocols | SAE J2534-1: `CAN` (5) and `ISO15765` (6); anything else answers `ERR_NOT_SUPPORTED` |
| Byte order | little-endian everywhere, EXCEPT the CAN id inside message data (big-endian, section 4) |
| Session | one tester at a time across all transports (section 2) |

## 1. Framing

Every message is a frame: a 12-byte header, then `length` payload bytes.

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 2 | magic | `0x4A35` (`"5J"` on the wire: `35 4A`) |
| 2 | 1 | version | `1` |
| 3 | 1 | type | section 3 |
| 4 | 2 | seq | tester's correlation id, echoed in the `ACK`; `0` in unsolicited frames |
| 6 | 2 | channel | `0` for device-level messages, else the channel id (1..4) |
| 8 | 4 | length | payload bytes that follow |

Limits: a request payload is at most `4384` bytes (`J2534_RX_CAP`: one
maximal `WRITE_MSGS`); a device frame is at most `4164` bytes
(`J2534_MAX_FRAME`: header + a 24-byte message head + 4128 data bytes).
The device reads exactly `length` bytes after every header, so frames may
be split or coalesced arbitrarily by the transport (TCP segments, USB
bulk packets, BLE notifications); reassemble by `length`, never by unit
boundaries. A header with a bad magic or version, or a `length` above the
cap, ends the session (the device logs `bad frame header` and drops the
tester): there is no in-band resync in v1, reconnect instead.

## 2. Session model

```
tester                                   device
  HELLO (seq 1)                 ->        ACK NOERROR, result = wire version
  OPEN  (seq 2)                 ->        ACK NOERROR, result = device id (always 1)
  CONNECT CAN (seq 3)           ->        ACK NOERROR, hdr.channel = result = 1
  WRITE_MSGS ch 1 (seq 4)       ->        ACK NOERROR
                                <-        RX_MSG ch 1 (seq 0, unsolicited) ...
  START_FILTER ch 1 (seq 5)     ->        ACK NOERROR, result = filter id
  DISCONNECT ch 1 (seq 6)       ->        ACK NOERROR
  CLOSE (seq 7)                 ->        ACK NOERROR
```

- `seq` is yours; the `ACK` carries it back. An `ACK` is sent after the
  operation completed on the bus side.
- `RX_MSG` frames are unsolicited (`seq 0`, `channel` = the source
  channel) and MAY arrive between a request and its `ACK`. A client
  reads frames continuously and routes by type.
- Exactly ONE tester across TCP, USB and BLE. A second tester's first
  frame is answered `ACK ERR_DEVICE_IN_USE (0x1A)` (with its `seq`), then
  that session ends (TCP: the socket closes; BLE/serial: nothing more is
  read on the link until it drops and comes back). Before 2026-09-21 the
  device closed silently; the ACK is the superset.
- A dropped link is an implicit `CLOSE`: channels, filters and periodic
  messages are torn down, the Exclusive-bus hold is released.
- The device is `enabled` per the `j2534_server` settings (default off);
  a disabled server serves nothing on any transport.

## 3. Message catalogue

Types 0x01..0x15 are tester to device; 0x80.. are device to tester.

| Type | Name | Payload (tester) | ACK `result` |
|---|---|---|---|
| `0x01` | `HELLO` | none | `u32` wire version (1) |
| `0x02` | `OPEN` | none | `u32` device id (1) |
| `0x03` | `CLOSE` | none | none; tears every channel/filter/periodic down |
| `0x04` | `CONNECT` | `protocol u32, flags u32, baud u32 [, tx_id u32, rx_id u32]` | `u32` channel id (1..4), also in `hdr.channel` |
| `0x05` | `DISCONNECT` | none, `hdr.channel` = the channel | none |
| `0x10` | `WRITE_MSGS` | `count u32`, then `count` PASSTHRU_MSGs (section 4) | none; the first failing message's status |
| `0x11` | `START_FILTER` | `type u32` (1 PASS, 2 BLOCK, 3 FLOW_CONTROL), then mask MSG, pattern MSG [, flow-control MSG] | `u32` filter id |
| `0x12` | `STOP_FILTER` | `filter_id u32` | none |
| `0x13` | `START_PERIODIC` | `interval_ms u32` (clamped to >= 5), then one MSG | `u32` periodic id |
| `0x14` | `STOP_PERIODIC` | `periodic_id u32` | none |
| `0x15` | `IOCTL` | `ioctl_id u32` [, parameters] | none |
| `0x80` | `ACK` | device: `status u32 [, result u32]` (payload 4 or 8 bytes) | |
| `0x81` | `RX_MSG` | device: one PASSTHRU_MSG; `seq 0`, `hdr.channel` = source channel | |
| `0x82` | `EVENT` | reserved, never sent in v1 | |

`CONNECT` details: the bus parameters come from `can_manager`, and since
2026-10-03 `baud` is checked against them for CAN and ISO15765: a tester
that asks for another bitrate than the bus runs at (the device's setting,
or what its listen-before-talk policy read off the wire) gets
`ERR_INVALID_BAUDRATE` instead of a channel at the wrong rate. Whole
kbit/s decide (83333 matches the setting `83`); with `can_manager.baud =
auto` and nothing proven yet the tester's baud is TAKEN when the bus has
been silent (2026-10-17, `can_manager_adopt_baud()`): the node then runs
there in the configured mode until frames prove it or the watchdog demotes
it, and the next `CONNECT` is checked against that rate. When it cannot be
taken (the bus not silent, unreadable traffic seen earlier, or not a rate
this node knows) the channel still opens, `WRITE_MSGS` answers
`ERR_FAILED`, the device logs one `W j2534_server: ... write refused` line
per `CONNECT` and counts `tx_refused` on `/api/j2534`. `flags` bit `0x100`
(`TX_CAN_29BIT_ID`) selects 29-bit
ids; `tx_id`/`rx_id` are optional (for ISO15765 they may instead come
from a FLOW_CONTROL filter). Errors: `ERR_DEVICE_NOT_CONNECTED` before
`OPEN` or with a short payload, `ERR_NOT_SUPPORTED` for a protocol other
than CAN/ISO15765 (or no ISO-TP provider in the build), `ERR_FAILED` when
all 4 channel slots are in use.

`WRITE_MSGS`: messages are sent in order; the loop stops at the first
failure and that status is the `ACK`. ISO15765 answers come back as
`RX_MSG` (the ISO-TP reassembly is done for you).

Filters (max 8 per channel): `PASS`/`BLOCK` match `mask`/`pattern` over
the message `data` bytes (for CAN that means the 4 id bytes first): a
message matches when `((data[i] ^ pattern[i]) & mask[i]) == 0` over
`min(mask_len, data_len)`; an empty mask passes everything. With no
filter every frame passes. `FLOW_CONTROL` on an ISO15765 channel (re)binds
the addressing: `pattern` data = the rx id (4 bytes BE), the flow-control
message's data = the tx id.

`IOCTL` v1: `CLEAR_MSG_FILTERS (0x0A)` and `CLEAR_PERIODIC_MSGS (0x09)` act;
`GET_CONFIG`, `SET_CONFIG`, `CLEAR_TX_BUFFER`, `CLEAR_RX_BUFFER` answer
`NOERROR` without effect (bus timing comes from the device settings).

## 4. PASSTHRU_MSG on the wire

24-byte head, then `data_size` bytes:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `protocol_id` (5 CAN, 6 ISO15765) |
| 4 | 4 | `rx_status` (device to tester: `0x02 START_OF_MESSAGE` on an ISO15765 first-frame indication, `0x100 CAN_29BIT_ID`) |
| 8 | 4 | `tx_flags` (tester to device: `0x100 CAN_29BIT_ID`, `0x40 ISO15765_FRAME_PAD`) |
| 12 | 4 | `timestamp` (device: microseconds since boot, taken in the CAN receive interrupt for CAN frames, at reassembly for ISO15765 PDUs; wraps every 71.6 min) |
| 16 | 4 | `extra_data_index` |
| 20 | 4 | `data_size` (0..4128) |
| 24 | n | `data` |

Data conventions:

- **CAN**: `data = [4-byte BIG-ENDIAN CAN id][0..8 frame bytes]`, so
  `data_size` is 4..12. `7DF` is `00 00 07 DF`.
- **ISO15765**: `data` = the UDS payload only; the ids come from
  `CONNECT` or the FLOW_CONTROL filter. `ISO15765_FRAME_PAD` in
  `tx_flags` pads single frames with `0xAA`.

## 5. Status codes

| Value | Name | WiCAN meaning |
|---|---|---|
| `0x00` | `NOERROR` | |
| `0x01` | `ERR_NOT_SUPPORTED` | protocol other than CAN/ISO15765; ALSO the reflash gate (section 6) |
| `0x02` | `ERR_INVALID_CHANNEL_ID` | `DISCONNECT`/data op on an unused channel |
| `0x03` | `ERR_INVALID_PROTOCOL_ID` | |
| `0x04` | `ERR_NULL_PARAMETER` | |
| `0x06` | `ERR_INVALID_FLAGS` | |
| `0x07` | `ERR_FAILED` | no free channel / periodic slot, timer failure, unknown message type |
| `0x08` | `ERR_DEVICE_NOT_CONNECTED` | `CONNECT` before `OPEN` |
| `0x09` | `ERR_TIMEOUT` | ISO-TP send/receive timed out |
| `0x0A` | `ERR_INVALID_MSG` | malformed PASSTHRU_MSG, short payload, unknown periodic id |
| `0x10`..`0x12` | `ERR_BUFFER_*` | |
| `0x14` | `ERR_MSG_PROTOCOL_ID` | message protocol does not match the channel |
| `0x15` | `ERR_INVALID_FILTER_ID` | |
| `0x16` | `ERR_NO_FLOW_CONTROL` | ISO15765 write without a bound tx/rx id |
| `0x17` | `ERR_NOT_UNIQUE` | |
| `0x18` | `ERR_INVALID_BAUDRATE` | `CONNECT` (CAN, ISO15765) with a baud the bus does not run at |
| `0x19` | `ERR_INVALID_DEVICE_ID` | |
| `0x1A` | `ERR_DEVICE_IN_USE` | a tester is attached on another transport |

## 6. Device behaviours

- **Exclusive bus** (setting `exclusive`, default on; runtime switch
  `POST /api/j2534 {"exclusive":bool}`): on tester attach the device asks
  AutoPID (PID polling + DTC scans) off the bus and waits up to 700 ms
  for the acknowledgement before processing the first frame;
  `/api/j2534` shows `autopid_paused`. Released on detach.
- **Reflash gate** (setting `allow_reflash`, default off): UDS services
  `0x34 RequestDownload`, `0x35 RequestUpload`, `0x36 TransferData`,
  `0x37 RequestTransferExit` are refused with `ERR_NOT_SUPPORTED` on
  ISO15765 AND on raw CAN (the device peeks past the id at the ISO-TP
  PCI nibble of single/first frames). Read/diagnose freely, no ECU
  programming unless the owner opened the gate.
- **ISO-TP sessions**: one per rx id; the device's own UDS terminal keeps
  its session 5 s after its last request, so a `CONNECT` to that ECU
  inside those 5 s fails. Retry.
- Limits: 4 channels, 8 filters per channel, 4 periodic messages,
  periodic interval >= 5 ms. Received messages are pushed as soon as the
  RX pump sees them (no device-side queue beyond the channel's 32-frame
  CAN queue): drain promptly.
- Interface exposure (TCP only): with `allow_lan` off the listener
  accepts connections that landed on loopback, WiCAN's own AP or the USB
  network link, and refuses the shop LAN (WiFi STA / USB-Ethernet).
  Serial and BLE never consult it (a cable / a paired link).

## 7. Worked examples (hex, little-endian; `|` separates header, fields)

```
HELLO seq 1
  -> 35 4A 01 01 01 00 00 00 00 00 00 00
  <- 35 4A 01 80 01 00 00 00 08 00 00 00 | 00 00 00 00 | 01 00 00 00     (NOERROR, version 1)

OPEN seq 2
  -> 35 4A 01 02 02 00 00 00 00 00 00 00
  <- 35 4A 01 80 02 00 00 00 08 00 00 00 | 00 00 00 00 | 01 00 00 00     (device id 1)

CONNECT CAN seq 3 (protocol 5, flags 0, baud 500000 = 0x0007A120)
  -> 35 4A 01 04 03 00 00 00 0C 00 00 00 | 05 00 00 00 | 00 00 00 00 | 20 A1 07 00
  <- 35 4A 01 80 03 00 01 00 08 00 00 00 | 00 00 00 00 | 01 00 00 00     (channel 1)

WRITE_MSGS seq 4, ch 1, one CAN message 7DF [02 01 00]  (payload 4 + 24 + 7 = 35 = 0x23)
  -> 35 4A 01 10 04 00 01 00 23 00 00 00 | 01 00 00 00 |
     05 00 00 00  00 00 00 00  00 00 00 00  00 00 00 00  00 00 00 00  07 00 00 00 |
     00 00 07 DF 02 01 00
  <- 35 4A 01 80 04 00 01 00 04 00 00 00 | 00 00 00 00                   (NOERROR)

RX_MSG (the ECU's answer, DLC 8; tt = timestamp, pp = pad byte)
  <- 35 4A 01 81 00 00 01 00 24 00 00 00 |
     05 00 00 00  00 00 00 00  00 00 00 00  tt tt tt tt  00 00 00 00  0C 00 00 00 |
     00 00 07 E8 06 41 00 18 3F 80 03 pp

START_FILTER seq 5, ch 1, PASS 7E8 (mask FF FF FF FF, pattern 00 00 07 E8)
  -> 35 4A 01 11 05 00 01 00 3C 00 00 00 | 01 00 00 00 |
     <mask MSG: head with data_size 4 | FF FF FF FF> <pattern MSG: head with data_size 4 | 00 00 07 E8>
  <- 35 4A 01 80 05 00 01 00 08 00 00 00 | 00 00 00 00 | 01 00 00 00     (filter id 1)

CONNECT ISO15765 seq 6 (protocol 6, tx 7E0, rx 7E8)
  -> 35 4A 01 04 06 00 00 00 14 00 00 00 | 06 00 00 00 | 00 00 00 00 | 20 A1 07 00 | E0 07 00 00 | E8 07 00 00
  <- ACK NOERROR, channel 2
WRITE_MSGS ch 2: one ISO15765 message, data 22 F1 90   -> ACK NOERROR
  <- RX_MSG ch 2, data 62 F1 90 31 57 43 41 4E 30 46 57 30 50 30 30 30 30 30 30 31   (VIN 1WCAN0FW0P0000001)

Reflash gate: WRITE_MSGS ch 2, data 34 00 44 00 00 00 04 00
  <- 35 4A 01 80 07 00 02 00 04 00 00 00 | 01 00 00 00                   (ERR_NOT_SUPPORTED)

Second tester's HELLO (seq 4) while one is attached
  <- 35 4A 01 80 04 00 00 00 04 00 00 00 | 1A 00 00 00                   (ERR_DEVICE_IN_USE)

CLOSE seq 9
  -> 35 4A 01 03 09 00 00 00 00 00 00 00
  <- 35 4A 01 80 09 00 00 00 04 00 00 00 | 00 00 00 00
```

## 8. Transport bindings

### 8.1 TCP

Port `j2534_server.port` (6809), `TCP_NODELAY`. The peer closing the
socket ends the session. Subject to the `allow_lan` gate (section 6).
Reference client: `tools/testbench/usb/j2534_bench.py`.

### 8.2 USB CDC-ACM serial

`usb_host_manager.device_class = cdc`: the WiCAN enumerates as a serial
port; asserting DTR starts the session, any baud rate. Reference:
`tools/testbench/usb/j2534_serial_test.py`.

### 8.3 BLE

Stream channel `j2534` under service 0xFFF0: **FFF5 = device to tester
(indicate: enable indications on its CCCD, each PDU is confirmed by your
stack)**, **FFF6 = tester to device (write / write-without-response)**.
Present in the GATT table only while `j2534_server.enabled` is true
(reboot-to-apply). Requires the paired, encrypted, MITM-authenticated
link like every WiCAN characteristic (`BLE_API.md` section 2).

- Write frames to FFF6 in ATT units of at most `min(490, MTU - 3)`
  bytes; the device reassembles by `length`. Its notifications on FFF5
  are at most that size too and carry frames back to back.
- The device's receive buffer holds 9 KB (two maximal requests): do not
  pipeline more than one request beyond the one outstanding when using
  write-without-response.
- A session starts when the link is secured and ends when it drops
  (implicit `CLOSE`); reconnect to start another.
- `/api/j2534` reports `"transport":"ble"` while a BLE tester is
  attached (`"tcp"`, `"serial"`, `"none"` otherwise).
- While a phone is connected over BLE the device suspends WiFi by
  default (`interface_manager.sta_ble_handover`), so a companion HTTP
  witness must use USB or turn that rule off.

Reference: `tools/testbench/ble/ble_j2534_pi.py` (bleak).

## 9. Observability

`GET /api/j2534`: `enabled, port, listening, client_connected,
device_open, channels, frames_rx, frames_tx, allow_reflash, allow_lan,
exclusive, autopid_paused, transport, phase`. `POST /api/j2534
{"exclusive":bool}`. Console: `j2534`. Logs (tag `j2534_server`):
`tester connected (ble)` / `tester disconnected (ble)` at I, refusals
(`tester already attached on tcp; refusing ble`, `bad frame header`) at W.

## 10. Versioning

`HELLO` returns the wire version; a client refuses to proceed on a
version it does not know. Any change to the header, a payload layout or
a status meaning bumps `J2534_WIRE_VERSION` in `j2534_proto.h` and this
document together.
