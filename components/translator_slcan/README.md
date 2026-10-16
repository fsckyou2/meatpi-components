# translator_slcan

The Lawicel **slcan** ASCII CAN framing codec, as a `bridge_manager`
translator (`"slcan"`). Bridges the internal `can` endpoint to a byte stream
(TCP/USB/WS), e.g. for `python-can`'s `slcan` interface.

- `decode` (can→client): CAN-wire chunk → `tIIIL DD..\r` / `TIIIIIIIIL..\r`
  (`t/T`=data 11/29-bit, `r/R`=RTR).
- `encode` (client→can): `t/T/r/R` lines (reassembled across chunks in the
  ctx) → CAN frames. Channel commands (`O/C/S/Z/M/m/V/N/F`) are **absorbed**:
  the bus is owned by `can_manager` settings; `python-can` needs no replies.

Wire format shared with the `can` endpoint: `can_manager/include/can_frame_wire.h`.
Design: `bridge_manager/DESIGN_translators.md §4`. Registered by
`translator_slcan_init()` (main's init pass).

## Use
Configure a bridge + socket (web UI / API), then any slcan client connects:
```
socket_manager.servers += {name:"slcan0", proto:"tcp", port:3333}
bridge_manager.bridges += {name:"br_slcan", a:"can", b:"slcan0", translator:"slcan"}
```
`a` MUST be the CAN endpoint (a→b = decode). Enable the CAN bus first.

## Tests
- Host: `./test.ps1 host translator_slcan`, encode/decode round-trips
  (11/29/RTR/dlc), byte-fragmentation, multi-frame, malformed, control absorb.
- Bench: `.\test.ps1 slcan` (= `tools/testbench/can/slcan_bridge_test.py`,
  python-can + PCAN both directions, the /ws/can monitor beside the TCP
  client, the Linux clients on rpi001, and first the trap: a server with
  no connection row) → **SLCAN BRIDGE PASS**. Throughput:
  `slcan_perf_test.py`.
