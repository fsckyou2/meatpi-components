# rtc_manager: battery-backed RTC owner (service)

Owns the RX8130CE RTC (I2C 0x32 on `i2c_bus`). The chip's backup caps keep
time through reboots and short unpowered periods, so timestamps are sane
seconds after power-on with no network:

- **start()** (every boot AND deep-sleep wake: both come through
  app_main): if the RTC holds a plausible time (2020..2099, valid
  fields, a fresh board / drained caps reads 2000-01-01 or garbage and
  is REJECTED), the system clock is restored from it and
  `DEV_STATUS_BIT_TIME_SYNCED` is set. The conversion is the component's
  own days-from-civil arithmetic (`rtc_time_to_epoch`, host-tested), not
  `mktime`: nothing about it depends on a TZ setting (the legacy
  firmware's stamps followed whatever TZ was set, issue #984).
- **SNTP task** (settings-gated): syncs the system clock AND writes the
  RTC back on **every internet (re)connect**, the task watches the
  network bits and re-syncs within seconds of each reconnect edge (not
  just the first connect), plus a periodic resync every
  `sync_interval_h` while the link stays up (60 s retry after a failed
  attempt). Live-verified: hotspot bounce → STA rejoin → resync 2 s
  later.

**Time is UTC everywhere** (RTC and system clock). Timezone is a
presentation concern of the consumer: legacy's `worldtimeapi.org` HTTP
lookup is intentionally gone (dead dependency, and local-time RTCs make
DST math everyone's problem).

## The clock's history (2026-10-13, issue #984)

Every set of the system clock is a **step** the component measures and
counts: the boot restore, each SNTP sync (lwIP writes the clock itself;
the component measures it the same way) and a manual set. The measure is
the change of `wall - mono` (esp_timer being the monotonic reference), so
it is exact to the millisecond whoever wrote the clock, and it carries the
monotonic time of the set, so a consumer can tell which of its own records
were taken before it. The first set of a boot moves the clock from 1970
(a cold start) or by the chip's lag (a warm reset carries the clock; the
restore then moves it by a second or two).

- `rtc_manager_clock_state()`: valid, the latest source (`none` / `rtc` /
  `sntp` / `manual`), the generation (bumps at every set), the count of
  steps this boot and the newest step.
- `rtc_manager_steps_since(generation, out, cap)`: the steps a consumer
  missed (the last 8 are kept). `data_logger` polls the generation before
  every batch and re-bases the rows queued before a step (its README,
  "Time").
- The event `time.step {delta_ms, source}` and the value `${time.valid}`
  for rules.
- `GET /api/rtc` carries `clock {source, generation, steps_this_boot,
  last_step {delta_ms, source, at}}` (HTTP_API.md 6d); `rtc` prints the
  same line.
- `rtc --forget` writes 2000-01-01 into the chip: the next POWER-ON boot
  starts without a clock (the plausibility gate rejects it), the bench's
  drained-caps unit on demand. A software restart keeps the running
  system clock.

## API

| Call | Behavior |
|---|---|
| `rtc_manager_init()` | Settings + log descriptors; the clock baseline (wall - mono as found). No bus traffic. |
| `rtc_manager_start()` | Chip bring-up (legacy control-register values: they configure backup-cap charging, do not touch), clock restore, SNTP task. |
| `rtc_manager_get_time(*tm)` | RTC as UTC broken-down time; `ESP_ERR_INVALID_ARG` when implausible. |
| `rtc_manager_sync_from_system()` | Persist the system clock into the RTC (the one write path). |
| `rtc_manager_now_iso8601(buf, len)` | `YYYY-MM-DDTHH:MM:SSZ` from the system clock. |
| `rtc_manager_set_time(epoch)` | Manual set (UTC seconds, 2020..2099): system clock + RTC + TIME_SYNCED, the no-NTP path; a step. |
| `rtc_manager_forget_chip()` | 2000-01-01 into the chip (the next power-on boot has no clock). |
| `rtc_manager_time_valid()` | Clock trusted this boot (RTC restore, SNTP, or manual). |
| `rtc_manager_clock_state(*out)` / `rtc_manager_steps_since(gen, out, cap)` / `rtc_manager_source_name(src)` | The clock's history for consumers (above). |
| `rtc_manager_sync_now()` | Blocking on-demand SNTP sync (primary + fallback). |
| `rtc_manager_last_sync()` / `_ntp_server()` / `_sntp_enabled()` | Status getters for transports. |
| `rtc_manager_register_http()` | `GET /api/rtc` (time + validity + chip time + sntp status + the clock block) / `POST /api/rtc {"epoch":N}` / `POST /api/rtc/sync` (on-demand): HTTP_API.md 6d; main wires it. |

## Decided semantics

- Chip reads use the coherent double-read pattern (burst twice until the
  seconds register agrees: a rollover between bursts would tear the date
  fields).
- Chip access serializes on a mutex; the SNTP task runs on a PSRAM stack
  (network only, no flash writes).
- TIME_SYNCED means "the system clock is trustworthy", whether it came
  from the RTC or NTP: consumers (logging timestamps) don't care which.
- A manual set counts as a step and marks the clock valid even when the
  chip write fails (the system clock moved; the failure is logged at W).

## Settings (`"rtc_manager"`, version 1, field table)

`cli` (bool, default true): register this component's console command(s) with cmdline_manager on the settings boot apply (reboot-to-apply). Ownership: the component registers its own commands, main wires nothing (2026-07-05).

`enabled` (bool, true) · `sntp` (bool, true) · `ntp_server` (str,
"pool.ntp.org": user-configurable; live-verified against
time.google.com) · `ntp_server2` (str, "": optional fallback tried
when the primary fails; live-verified: bad primary → fallback synced) ·
`sync_interval_h` (1..168, default 24).

Legacy `sync_sys_time.c` review (2026-07-04): adopted its explicit
`TZ=UTC0` pin (no component can skew the UTC-everywhere model), its
multi-server redundancy (as the configurable fallback), and its
sanity floor: a server answering with a pre-2020 date is ignored, so
a bogus NTP reply can't poison the clock or the RTC.

## Files

- `rtc_manager.c`: lifecycle, restore, SNTP task, the clock-step door.
- `rtc_manager_time.c`: PURE BCD/struct-tm codec + plausibility gate +
  the civil-to-epoch conversion (host-tested).
- `rtc_manager_clock.c`: PURE clock-step bookkeeping (host-tested).
- `rtc_manager_rx8130.c`: chip layer.
- `rtc_manager_http.c`, `rtc_manager_cli.c`, `rtc_manager_events.c`,
  `rtc_manager_settings.c`: the surfaces.

## Testing

- Host: `host_test/` (12 tests: the codec, the fresh-board rejection,
  the epoch vectors incl. 2038 and 2099, the step measure from a cold and
  a carried baseline, a zero step, the 8-entry table and the catch-up by
  generation). `.\test.ps1 host rtc_manager`.
- Bench: `tools/testbench/system/log_clock_bench.py` (`.\test.ps1
  logclock`, `LOG CLOCK PASS`): an SNTP actor on the Pi steps the DUT's
  clock while the logger writes; the unsynced boot after `rtc --forget`
  and a power cycle. TESTING.md.

## Memory

SNTP task 4 KB PSRAM stack; state a few hundred bytes (the step table).
(estimated)

## CLI

`rtc [-s|-r|-i|--forget]` (`rtc_manager_cli.c`, registered on the
settings apply): bare = the summary incl. the clock-step line; `-s` sync
now; `-r` read the chip; `-i` the responding probe; `--forget` the
drained-caps unit on demand.
