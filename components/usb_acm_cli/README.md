# usb_acm_cli

USB serial (CDC-ACM) devices on the WiCAN's USB **host** port, over the
CherryUSB host stack: a plain USB GPS receiver, and the ESPNetLink dongle's
management console. Overrides the WEAK `usbh_cdc_acm_run/stop` hooks and runs
one PSRAM-stacked RX task (its 2 KB StreamBuffer storage is PSRAM too since
2026-09-22; the control block stays internal) for whatever device binds.

Two switches, settings version 2 (2026-10-11, issue #978):

| Key | Default | What it does |
|---|---|---|
| `gps` | `true` | **Listen.** A device streaming NMEA 0183 (a u-blox receiver) is read from the moment it is plugged in; its fix feeds `GET /api/gps`, the autopid `gps_*` parameters and so Home Assistant, the destinations, the dashboard and the logger. Off: a receiver on the connector is ignored. |
| `console` | `false` | **Talk.** The ESPNetLink's own console over the cable: `POST /api/usb/acm/cmd`, the `acm` CLI, the bridge_manager endpoint `acm` (raw passthrough) and a `gps -p -j` poll every 5 s. Off: nothing is ever sent to the device, and the command route answers 409. Version 1 called this switch `enabled`; `on_migrate` carries the stored value over (`usb_acm_cli_migrate.c`, pure, host-tested). |
| `cli` | `true` | Register the `acm` console command (standard section 6b). |

The reader runs while either switch is on. Why two keys and not one named
`enabled`: the web UI's generic settings form shows only a component's
`enabled` switch while that key is false, which would hide the GPS switch
exactly where a user looks (the All Settings page, the USB page's console
tab).

Surfaces:
- `GET /api/usb/acm` → `{"connected","reading","mode","nmea_sentences",
  "gps","console"}`: is a device bound, is it read, what it turned out to be
  (`console` / `nmea`), and the two switches as applied.
- `POST /api/usb/acm/cmd` `{"cmd","timeout_ms"?}` → send one line, collect
  the reply until the device's `esp>` PROMPT (`{"ok","response",
  "connected"}`). `timeout_ms` (default 8000) is a cap, not a latency:
  collection returns the moment the prompt lands; a 2 s quiet window is the
  prompt-less fallback. **2026-07-13 fix**: the original 150 ms quiet-settle
  raced the echo→payload gap (the modem legs of `lte`/`gps` go silent >1 s
  mid-response) and returned just the echo: the web-UI ESPNetLink card's "no
  reply" bug. 409 `the ESPNetLink console is off` while `console` is false.
- `GET /api/gps`: the last fix in device-contract field names (speed m/s).
- a bridge_manager endpoint `acm` for raw passthrough (while `console` is on).

## GPS

ONE fix cache (`usb_acm_cli_poll.c`), two writers: the RX task (a receiver's
NMEA sentences as they arrive, `usb_acm_nmea.c`) and the console poll task
(the dongle's `gps -p -j` every 5 s, the legacy usb_host cadence, parsed by
`usb_acm_gps.c`; only while `console` is on and the bound device has not
spoken NMEA). Two surfaces read the cache (never any ACM I/O):

- `GET /api/gps`.
- `usb_acm_cli_gps_get()`: for C consumers (the Home Assistant push's `gps`
  block). When the cache has no live fix it consults the **fallback
  provider** (`usb_acm_cli_set_gps_fallback()`, main wires
  `espnetlink_link_gps_get`): the WiFi-modem topology (dongle USB data cut,
  reached over its AP) has no console at all, so `/api/gps` keeps working
  from the HTTP poll. The parser also accepts the dongle's HTTP `/api/gps`
  spelling (`latitude`/`longitude`) next to the console's `lat`/`lon`.

Each refresh also fires `usb_acm_cli_set_gps_sink()`: **main** wires it to
`autopid_publish_external()`, so a fix becomes first-class autopid
parameters (`gps_latitude`/`gps_longitude`/`gps_altitude`/`gps_speed`/
`gps_heading`/`gps_satellites`) and flows wherever autopid data goes: the
Home Assistant push (`autopid_data`), the data_logger, the dashboard, and
event rules, no GPS code in any of those. Only a LIVE fix is published; a
cached AGNSS position is never reported as current. A valid fix older than
20 s (the device gone, or quiet) reads as no fix.

The console is the attached device's OWN CLI (the espnetlink dongle's `esp>`
prompt: `ver`, `lte -s/-r/-o/-i`, `gps`, `speedtest`, ...), not raw AT.

## A plain USB GNSS receiver (NMEA, 2026-10-07)

A receiver such as the bench u-blox 7 (`1546:01a7`, CDC-ACM) streams NMEA
0183 by itself (RMC, GGA, GSA, GSV, GLL, VTG at 1 Hz) and understands no
console. The RX task feeds every chunk to `usb_acm_nmea.c` (pure,
host-tested: bytes in any chunking, a checksum-verified sentence out, a fix
out of every RMC with status A merged with the last GGA's satellites, HDOP
and altitude; an RMC with status V clears the fix). The first well-formed
sentence after an attach turns the console into a receiver
(`usb_acm_cli_mode()` "nmea"): the poll task sends nothing to it, and the
fix cache, `GET /api/gps`, the sink to autopid and the fallback rule are
exactly the dongle's. Since 2026-10-11 none of this needs a setting: `gps`
is on by default (issue #978: a receiver plugged in after setup met a bound
device nobody read, because the reader sat behind the console switch).
Who is on the connector (ids, class) is `usb_host_manager`'s `device`.

**Never bound: `303A:1001`**, an ESP32-S3's ROM/bootloader USB-Serial-JTAG
is on the pads for ~1.5 s after a power-on (the ESPNetLink's app detaches
it before its real `303A:4007` composite device enumerates). CherryUSB's
class match is class-only, so `usbh_cdc_acm_run` filters that VID/PID
itself: the DTR/RTS line sequence it would otherwise send is exactly what
resets the chip (2026-08-24).

## Files

`usb_acm_cli.c` (the host glue, the RX task, the console), `usb_acm_cli_poll.c`
(the fix cache and the dongle poll task), `usb_acm_cli_settings.c` (the field
table, on_apply), `usb_acm_cli_migrate.c` (the pure v1 -> v2 migration),
`usb_acm_cli_http.c`, `usb_acm_cli_cli.c`, `usb_acm_gps.c` (the dongle's JSON
parser), `usb_acm_nmea.c` (the NMEA reader).

## Memory footprint (estimated)

Internal: the two 64 B DMA buffers (heap, while either switch is on), the
StreamBuffer control block, the task TCBs and mutexes. PSRAM: the 2 KB
StreamBuffer store, the 4 KB RX task stack, the 4 KB poll task stack. Flash
writes: none (settings are the manager's).

## Testing

Host suite (`host_test/`, 14 tests): the pure `usb_acm_gps_parse`, the
`gps -p -j` → fix mapping incl. the `"lat"` vs `"cached_lat"` quote
disambiguation and the "valid:false → not a fix, cached position not
adopted" rule; the NMEA reader (2026-10-07): GGA then RMC = a fix with
the merged satellites, altitude and HDOP accuracy, an RMC alone and a GN
talker, a lost fix (status V) and the sentences that only count (GSV, VTG,
a GGA without a fix), a flipped byte rejected by the checksum, bytes fed
seven at a time across sentence boundaries, an over-long line dropped
whole; and the migration (2026-10-11): a stored `enabled` true and false
carried into `console`, a document without the key, a v2 document left
alone, bad input refused. The suite needs cJSON (`main/idf_component.yml`;
the bench Pi is offline from the registry: copy another suite's
`managed_components` + `dependencies.lock` next to `host_test/`).

The rest of the component is inseparable from live USB host I/O + DMA (no
pure unit on the `linux` target); it is covered on the bench:
`tools/testbench/usb/usb_gps_bench.py` (`USB GPS PASS`: the receiver read
from boot with the console off, the fix when the sky allows, `gps_*` in
autopid, the Home Assistant push's `gps` block, the switch off = ignored)
and the espnetlink dongle's console (`ESPNETLINK TARGET PASS`,
`tools/testbench/usb/espnetlink_bench.py`, needs `console` on): the
RX-settle collector, the HTTP/bridge paths, and the GPS poll → autopid
publish path run on real hardware.
