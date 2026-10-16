# restart_tracker: HTTP API reference

> **Implemented (2026-07-03)** by the `api_http` glue (on-target suite green).
> Conventions: `components/HTTP_API.md` §1. 2026-10-05: the crash-loop
> brake (`brake`, `mode`, `settled`) and the stored crash report (`report`,
> `GET` / `DELETE /api/restart/report`).

## GET /api/restart/history

The reboot forensics: counters plus the PSRAM-persistent history ring
(up to `RESTART_TRACKER_HISTORY_LEN` = 8 boots), newest first. Enum fields are
delivered as their `*_to_str` names, never raw numbers.

**Response 200**
```json
{
  "boot_count": 17,
  "unexpected_resets": 1,
  "elf_sha": "76a961dd5f08aabb",
  "brake": {
    "verdict": "normal",
    "streak": 1,
    "limit": 3,
    "parks": 0,
    "settled": false,
    "settle_s": 600,
    "report_budget": 3
  },
  "report": {
    "stored_time": 1782513601,
    "time_valid": true,
    "firmware": "v6.00p_alfa-01",
    "streak": 1,
    "parked": false,
    "crash": { "summary": "exception LoadProhibited at 0x42209640 ...", "kind": "exception", "pc": "0x42209640" }
  },
  "records": [
    {
      "seq": 17,
      "reason": "panic",
      "planned": false,
      "planned_reason": "none",
      "source": "unknown",
      "flags": 0,
      "boot_time": 1782513601,
      "time_valid": true,
      "request_time": 0,
      "request_uptime_ms": 0,
      "mode": "normal",
      "settled": false,
      "crash": {
        "summary": "exception LoadProhibited at 0x42209640 (address 0x0000003c), core 1, task \"apid_scan\", up 18 s, image 76a961dd5f08aabb",
        "kind": "exception",
        "reason": "LoadProhibited",
        "cause": 28,
        "pc": "0x42209640",
        "excvaddr": "0x0000003c",
        "core": 1,
        "task": "apid_scan",
        "in_isr": false,
        "uptime_s": 18,
        "text": "",
        "backtrace": ["0x4220963d", "0x4220685d", "0x42206805"],
        "backtrace_more": false,
        "backtrace_corrupt": false,
        "other_core": [],
        "nested": false,
        "elf_sha": "76a961dd5f08aabb",
        "same_image": true,
        "complete": true
      }
    },
    {
      "seq": 16,
      "reason": "software",
      "planned": true,
      "planned_reason": "config_apply",
      "source": "config_server",
      "flags": 0,
      "boot_time": 1782513000,
      "time_valid": true,
      "request_time": 1782512998,
      "request_uptime_ms": 483211,
      "mode": "normal",
      "settled": true
    }
  ]
}
```

- `elf_sha` (top level): the running image, the first 16 hex characters of
  its ELF file's SHA-256 (what IDF prints as `ELF file SHA256` at boot).
- `crash`: only on a record whose boot followed a crash that went through
  IDF's panic handler (README, "The crash note"). It describes the run
  BEFORE that boot.
- `mode` (per record): how that boot ran: `normal`, `park` (the crash-loop
  brake parked it asleep), `park_bare` (parked without the LED: the park
  itself had crashed), `safe` (safe mode). A parked or safe-mode boot
  serves no HTTP: these values are read on a later boot.
- `settled` (per record): that run stayed up `settle_s` seconds (or was
  marked by hand on the console). A crash of a settled run is not part of a
  crash loop.

| `crash` field | Meaning |
|---|---|
| `summary` | the note in one line, as the boot log and the CLI print it |
| `kind` | `exception`, `abort`, `int_wdt`, `task_wdt`, `debug` |
| `reason` | IDF's name of the exception (`LoadProhibited`, `Interrupt wdt timeout on CPU0`); empty for `abort` and for an incomplete note |
| `cause` | `EXCCAUSE` as a number (28 = LoadProhibited, 29 = StoreProhibited) |
| `pc`, `excvaddr` | hex strings, as the console's register dump prints them |
| `core`, `task`, `in_isr`, `uptime_s` | where and when: the core, the task that was running on it, whether inside an interrupt handler, seconds since that run's boot |
| `text` | for `abort`: `assert failed: ...`, `abort() was called at PC ...`, `***ERROR*** A stack overflow in task ...` (96 characters at most) |
| `backtrace` | hex strings: the PCs of the console's `Backtrace:` line, 16 at most (the first is `pc - 3`) |
| `backtrace_more`, `backtrace_corrupt` | the stack went on after the 16th frame / IDF's sanity check ended the walk |
| `other_core` | up to 6 PCs of the other core, when IDF saved its frame (interrupt watchdog, cache error) |
| `nested` | a second panic happened inside the panic handler; the note is the first one's |
| `elf_sha`, `same_image` | the image that crashed, and whether it is the one running now (decode the PCs with that image's ELF: `tools/crash_decode.py`) |
| `complete` | false: the handler did not finish the note; only `kind`, `cause`, `pc`, `excvaddr`, `core` are real |

| `brake` field | Meaning |
|---|---|
| `verdict` | what this boot was told to do: `normal`, `park`, `park_bare`. Over HTTP it always reads `normal`: a boot that parks starts no network |
| `streak` | runs in a row that crashed before they settled, as it stands now (0 again once this run settled). After a park ended to try again it still reads 3 or more: one more quick crash parks at once |
| `limit` | the streak that parks the device (3) |
| `parks` | parks since that streak began |
| `settled` | this run was up `settle_s` seconds |
| `settle_s` | how long a run must stay up to count as healthy (600) |
| `report_budget` | crash reports that may still be stored in flash before a run settles (the wear guard; 4 when full) |

| `report` field | Meaning |
|---|---|
| (the object) | present only while a crash report is stored in flash (NVS). It outlives a power cycle; `records[].crash` does not. One report: the newest distinct crash |
| `stored_time`, `time_valid` | unix seconds of the boot that stored it; `0` and `false` when the clock was not set |
| `firmware` | version string of the image that crashed; empty when another image stored the report |
| `streak` | the brake's streak when it was stored |
| `parked` | the brake parked the device on this crash |
| `crash` | the crash, in the shape of `records[].crash` (its `same_image` compares with the image running now) |

- `boot_time`/`request_time` are unix seconds; `0` + `time_valid:false` means
  the wall clock wasn't set yet (pre-NTP boot).
- `reason` values (the `esp_reset_reason_t` names; the list follows the enum
  since 2026-10-04, `usb` and `jtag` deliberate since 2026-10-17): `poweron,
  external, software, panic, interrupt_wdt, task_wdt, wdt, deepsleep,
  brownout, sdio, usb, jtag, efuse, power_glitch, cpu_lockup, unknown`
  (`invalid` for a value outside the enum). `unexpected_resets` counts a boot
  whose reason is none of `poweron`, `external`, `software`, `deepsleep`,
  `usb` (the USB-Serial-JTAG reset esptool issues after a flash over the
  native USB port) or `jtag` (a debugger), unless the restart was planned.
- `planned_reason` values include `partition_migrate` (2026-10-10): the boot
  before it rewrote the partition table in flash with this build's (a unit
  updated by OTA from the factory firmware), `source` `boot`.
- `planned_reason` values include `park_retry` (2026-10-05): a parked device
  started again, by `source` `park` (its timer) or `button`.
- After a power cycle the ring restarts (PSRAM `.noinit` is only warm-reset
  persistent); `boot_count` restarts at 1: that is correct behavior, not loss.
  The brake's count restarts with it; the stored report stays.

**Errors**: `503 {"error":"tracker state unavailable"}` if init never ran
(should not happen in composed firmware).

## GET /api/restart/report

The stored crash report as plain text: what a user copies into a support
request. The console's `restart_tracker --report` and safe mode's page
(`/crash_report`) give the same lines; `tools/crash_decode.py --report`
(firmware repo) turns the addresses into function names.

**Response 200** (`text/plain; charset=utf-8`, `Cache-Control: no-store`)
```
WiCAN crash report
Device:    68ee8f5a653d
Firmware:  v6.00p_alfa-01-36-g0eebbd6-dirt
Image:     ab67e6d34a77b139
Stored:    2026-10-05 03:37:24 UTC
Loop:      3 crashes in a row; the device parked itself
Crash:     exception StoreProhibited at 0x42087fdc (address 0x0000bad0), core 0, task "cli_console", up 11 s, image ab67e6d34a77b139
Backtrace: 0x42087fd9 0x42088462 0x421281bf 0x421278d3 0x42127e1e 0x4038c53d
```

- One `Label:` per line, the value from column 12. `Device` is the device
  id (`/api/info`). `Firmware` reads `not the one that stored this report`
  when the image that crashed is not the one that stored it; `Stored` reads
  `the clock was not set`; `Image` reads `not recorded` for an incomplete
  note.
- `Loop:` only when the crash was at least the second in a row or parked
  the device. `Backtrace:` ends in ` ...` when the stack went on, in
  ` (corrupt)` when the walk was stopped. `Core N:` (the other core's
  frames) only when IDF saved them.
- At most 1024 bytes.

**Errors**: `404 {"error":"no crash report is stored"}`.

## DELETE /api/restart/report

Forget the stored report (one NVS erase). The notes in RAM stay.

**Response 200**
```json
{ "cleared": true }
```
`cleared` is also `true` when nothing was stored; `false` when NVS refused.

## POST /api/restart

The UI reboot button. No body required; optional body `{"flags":N}` is passed
through to the record.

**Response 200**
```json
{ "ok": true }
```
then ≈1 s flush delay, then
`restart_tracker_restart(USER_REQUEST, WEB_UI, flags)`: the reboot lands in
the next boot's history as `planned:true, source:"web_ui"`. Never a raw
`esp_restart()`.
