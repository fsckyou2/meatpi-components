# restart_tracker

## Summary

Core component that answers "why did the device reboot?" without touching
flash. Its state (boot counter, unexpected-reset counter, an 8-entry history
ring, and a pending planned-restart intent) lives in **PSRAM `.noinit`**
(`EXT_RAM_NOINIT_ATTR`) behind a magic/version/CRC-32 envelope with an
`esp_cache_msync` write-back after every mutation. Warm resets (esp_restart,
panic, watchdog) preserve everything; a power cycle leaves random PSRAM, which
the envelope detects and resets cleanly. It also provides the **one sanctioned
way to reboot on purpose**, `restart_tracker_restart(reason, source, flags)`,
so the next boot always knows whether the restart was planned and by whom.
This is the PSRAM-survival pattern `log_manager`'s crash ring reuses.
A boot that follows a crash also knows **where** it crashed: the crash note
(below), written by two hooks around IDF's panic handler. Since 2026-10-05
the newest distinct crash is also kept in NVS (the stored crash report: the
one thing this component writes to flash, behind a wear guard), and three
crashes in a row end in a verdict the composition root must obey: park the
device instead of starting it again (the crash-loop brake).

## API

| Function | One-liner |
|---|---|
| `restart_tracker_init()` | Validate/adopt the PSRAM state, record this boot (consumes any pending intent). Call **early** in `main`. |
| `restart_tracker_start()` / `_stop()` | Lifecycle uniformity (§3); passive component, both trivial. |
| `restart_tracker_mark_planned_restart(reason, source, flags)` | Announce an intentional restart without performing it. |
| `restart_tracker_restart(reason, source, flags)` | Mark + `esp_restart()`. `noreturn`. Use this, never raw `esp_restart()`. |
| `restart_tracker_get_state(out)` | Snapshot everything (history ring included). |
| `restart_tracker_get_latest_record(out)` | This boot's record. |
| `*_to_str(...)` | Human-readable reset/planned-reason/source names. |
| `restart_tracker_get_crash(sequence, out)` | The crash note filed under that boot; `ESP_ERR_NOT_FOUND` when it has none. |
| `restart_tracker_crash_summary(crash, buf, cap)` | The note in one line (the boot log, the CLI, the HTTP `summary`). |
| `restart_tracker_crash_kind_to_str(kind)` | `exception`, `abort`, `int_wdt`, `task_wdt`, `debug`. |
| `restart_tracker_get_brake(out)` | The crash-loop brake: this boot's verdict (`NORMAL`, `PARK`, `PARK_BARE`), how long a park lasts, and the count as it stands (streak, parks, report budget, settled). Main asks right after init. |
| `restart_tracker_set_boot_mode(mode)` | Declare park or safe mode before the mode does anything: the next boot's verdict and the history record read it. |
| `restart_tracker_settle(force)` | Marks this run settled once it was up `RESTART_TRACKER_SETTLE_S` (600 s): ends a streak, refills the report budget. Main's 60 s loop calls it; `force` is the bench's knob. No flash. |
| `restart_tracker_set_test_retry(seconds)` | Bench: the next park ends by itself after 10..3600 s, once. |
| `restart_tracker_get_report(out)` | The crash report stored in NVS; `ESP_ERR_NOT_FOUND` when there is none. |
| `restart_tracker_report_text(report, buf, cap)` | The report as the text a user sends on (`RESTART_TRACKER_REPORT_TEXT_MAX` = 1024 holds any). |
| `restart_tracker_clear_report()` / `_restore_report()` | Forget it (one NVS erase) / write it back after a caller erased NVS wholesale (safe mode's factory reset). Flash: internal-stack callers only. |
| `restart_tracker_boot_mode_to_str(mode)` | `normal`, `park`, `park_bare`, `safe`. |

Reasons: none / user_request / config_apply / config_recovery / ota_apply /
factory_reset / safe_mode / power_wake / internal_recovery / periodic_wake
(sleep_manager's check-in, 2026-09-07: before that it was filed as
power_wake) / park_retry (a parked device starts again, 2026-10-05) /
partition_migrate (the partition table in flash was rewritten with this
build's, 2026-10-10: `partition_migrate`, by `main_boot_layout()`) /
ota_rollback (the boot path rolled a firmware update back to the previous
slot after three quick crashes of the new image, 2026-10-18:
`fw_update_boot_check()`, source `boot`, flags = the job id).

**The pre-restart hook (2026-10-18).** `restart_tracker_set_pre_restart_hook()`
installs one function that `restart_tracker_mark_planned_restart()` calls
first, in the caller's context and outside the spinlock: fw_update confirms
a pending image to the bootloader there (a settings submit two minutes
after an update would otherwise make a rollback-enabled bootloader fall
back). The hook must return within a second or two and may be called from a
PSRAM-stack task, so a flash write inside it runs on an internal-stack
helper.
Sources: web_ui / cmdline / console / mqtt / ota / safe_mode / config_server
/ sleep_mode / button / pairing / park (the crash park's own timer) / boot
(the boot path itself: the partition migration restart).

## The crash note

IDF prints where a crash happened (the Guru Meditation text) on the console
and nowhere else, and never names the task. The tracker keeps the essentials
for the next boot, without flash, PSRAM, heap, locks or logging on the crash
side:

```
crash -> panic_handler()                 esp_system/port/panic_handler.c
  -> esp_panic_handler(info)   WRAPPED   stage A: ten words into RTC memory
       IDF prints the Guru Meditation text, as always
       -> panic_restart()      WRAPPED   stage B: task, uptime, backtraces,
                                         texts; then the reset
next boot -> restart_tracker_init()      files the note under this boot's
                                         record, two W lines
```

- **Two link-time hooks** (`restart_tracker_crash.c`; CMakeLists:
  `-Wl,--wrap=esp_panic_handler`, `--wrap=panic_restart`). Stage A runs
  before IDF prints and reads nothing but IDF's own `info` and exception
  frame. Stage B runs after the last printed byte: a fault in it costs the
  tail of the note, never the console text (IDF's nested-panic path prints
  the second fault and resets). The first panic of a run keeps its note.
- **The note** (300 bytes, `rt_crash_note_t`): kind (exception, abort,
  interrupt watchdog ...), core, `EXCCAUSE`, PC, `EXCVADDR`; then the task's
  name, uptime, the image's ELF SHA (16 hex characters), IDF's name of the
  exception or the abort / assert / stack-overflow text, 16 backtrace PCs
  (exactly the PCs of IDF's `Backtrace:` line, so `bt[0]` is `pc - 3`) and
  up to 6 of the other core when IDF saved its frame (watchdog, cache
  error). Two checksums: a note whose stage B never finished still has a
  valid head (`complete` false).
- **The store** is RTC slow memory (`RTC_NOINIT_ATTR`, 0x50000000): plain RAM
  beside the cache, kept over the CPU reset a panic ends in and over light
  sleep, never loaded or cleared at boot, part of no heap. It holds the
  pending note and one kept note per history record. Random after a
  power-on: every note checks itself.
- **Filing** (`rt_crash_collect()`, pure): a valid pending note moves to the
  slot of this boot's record under this boot's number; no pending note
  clears the slot; a store that is not ours, or a history that starts again
  at boot 1, clears every kept note first. Lookup is by boot number.
- **What it will not tell**: a reset without a panic (brown-out, power cut,
  EN pin, a raw `esp_restart()`); a task that hangs with interrupts on
  (`CONFIG_ESP_TASK_WDT_PANIC` is off: no reset, nothing to note); more than
  16 frames, registers, other tasks (that is a core dump, and there is no
  partition for one). After a power cycle the notes are gone, like the
  history; the flasher's EN reset may lose them too.
- **Decoding**: the PCs need the ELF of the image that crashed. The note
  carries that image's id, `GET /api/restart/history` the running one;
  `tools/crash_decode.py` (firmware repo) checks the ELF against it and runs
  addr2line.

## The crash-loop brake

A firmware that crashes at every start must not be started for ever: it
keeps the vehicle's battery awake, pokes the vehicle's bus at every boot and
can never be reached to be fixed. The brake counts, and gives a verdict; the
composition root obeys it (firmware `main_park.c`: LED breathing red, OBD
chip asleep, CAN transceiver in standby, USB rail off, light-sleep naps).

**The rule, whole** (`restart_tracker_brake_core.c`, pure):

- A run that ends in a **crash** (reset reason panic, interrupt / task /
  RTC watchdog, CPU lockup) **before it settled** adds one to the streak.
  At `RESTART_TRACKER_BRAKE_STREAK` (3) the boot's verdict is `PARK`.
- A run **settles** when it has been up `RESTART_TRACKER_SETTLE_S` (600 s;
  main's 60 s loop calls `restart_tracker_settle()`). That ends the streak:
  one crash of a healthy run is not a loop.
- **Any reset that is not a crash** ends the streak too: a planned restart,
  a wake from sleep, the EN pin, a brown-out, a power cycle (which empties
  the count with the RAM it lives in). A crash that a restart request had
  announced is that restart going wrong, and is not counted.
- A park that **ends to try again** (the button, or the park's timer) keeps
  the streak: the start that follows is one try, and one more quick crash
  parks at once, not after three.
- A crash **inside the park** gives `PARK_BARE`: park again with less. Main
  leaves out the LED, the one part of a park with a driver behind it, and
  keeps the pin writes that put the board to sleep.
- **Safe mode** is the user's own recovery and is never braked.

**Where the count lives**: twelve bytes with their own CRC in the crash
note store, RTC slow memory. Not PSRAM: a firmware that loses its PSRAM at
every boot (a memory test left on, a bad chip) is exactly what the brake
has to stop, so the count survives the history starting over
(`test_brake_count_outlives_the_psram_history`). A count that fails its CRC
starts from zero, the safe direction. No flash.

**How a park ends**: the power is cycled (a fresh start: three tries); the
button is held about 3 s (one try; keep holding and that boot goes to safe
mode, where the crash report is); or the park's own timer, when the build
has one. `CONFIG_WICAN_RESTART_TRACKER_PARK_RETRY_MIN` (Kconfig, default
0 = never): with N a parked device tries one normal start every N minutes.
Off as shipped: a parked device stays parked until someone attends to it.

**What it does not brake**: a loop slower than the settle time (the
sleep delay, 5 min as shipped, is under it, so a crash at every sleep entry
is caught; a sleep delay raised above 10 min is not); restarts that are not
crashes (a component asking for a recovery restart, brown-outs: the sleep
manager's battery guard is the net for resets on a weak battery); a crash
before `restart_tracker_init()` runs, which nothing in the firmware can
count.

Measured on the bench unit, 13.5 V (`.\test.ps1 crashnote`, stage 2): 162 mA
awake, 44 mA in the firmware's sleep mode, 47 mA parked; a park that sets no
pin at all 134 mA. The 3 mA over sleep mode are not the LED, the OBD UART
line or the chip's reset (three probe builds): they are what a park does not
touch and sleep mode shuts down in order (SD card, radios).

## The stored crash report

The notes live in RTC memory and go with the power, and a parked device is
one its user unplugs. So the boot that files a note also stores it in NVS:
namespace `rt_crash`, key `report`, one 360-byte blob (`rt_report_t`: the
note, when it was stored, the firmware's version string when the image that
crashed is the one storing it, the brake's streak, whether the device
parked). One report, the newest distinct crash.

- **When**: inside `restart_tracker_init()`, the second thing a boot does,
  before any filesystem is mounted and before the brake's verdict is
  obeyed: a device that parks now already has its report on flash. NVS and
  not a file for that reason, and because safe mode opens NVS and mounts no
  filesystem on purpose.
- **The wear guard** (`restart_tracker_report_core.c`, pure; standard §11).
  A crash is stored when it is not the one already stored: another place,
  image, task, message or call chain (`rt_report_identity()`). The same
  crash again is stored only for news about it: the brake parked the device
  on it, the stored copy had no date and this one has, or a day went by.
  And whatever the news, at most `RT_REPORT_BUDGET` (4) reports are stored
  until a run settles. A crash loop therefore costs four small writes at
  the very most, then none; the usual loop (one crash, three times, parked)
  costs two.
- **What a write costs**, measured: 9 to 12 flash writes, 468 to 500
  bytes, no erase (`WICAN FLASH writes=10 wbytes=472`), in the boot that files the
  note. The same crash again: `writes=0`. A boot with no crash before it:
  `writes=0`, the report is only read. NVS on the bench unit: 118 of 504
  entries used, the report takes 15. As arithmetic: a report is 15 of a
  page's 126 entries, so about 8 reports fill a page and cost one erase,
  spread over the partition's 4 pages: a 100 000-cycle part is good for
  some 3 million reports.
- **Who reads it**: `GET /api/restart/history` (`report`), `GET
  /api/restart/report` (text), the Status page's `Last crash` row (Report:
  view, copy, download, clear), the console (`restart_tracker --report`),
  safe mode's page and console. `tools/crash_decode.py --report <file>`
  (firmware repo) turns a report someone sent into function names.
- **What it survives**: a power cycle, a firmware update (the report keeps
  the id of the image that crashed), the console's `factoryreset` (settings
  partition only), safe mode's factory reset (which erases NVS and puts the
  report back: `restart_tracker_restore_report()`). It goes with `DELETE
  /api/restart/report`, `restart_tracker --clear-report`, a different crash
  replacing it, or NVS being erased by main after `NO_FREE_PAGES`.
- **What it will not tell**: what the crash note cannot (above), and a
  second crash while the budget is spent: the report stays the last one
  that fitted.

**At every IDF version bump** run `.\test.ps1 target restart_tracker` and
`.\test.ps1 crashnote`. Both hooks wrap symbols private to IDF. A rename
fails the link; a call that moved into the file of its target (or got
inlined) would stop the notes silently, and only those two runs see it.
Measured on IDF v6.0.2: stage B takes 242 us before the reset;
the hooks add 32 B of stack while IDF prints and 160 B at
the reset call.

## Dependencies

- `esp_mm` (cache msync) + `esp_timer`: private, target glue only.
- `nvs_flash` (the stored crash report). NVS must be initialised before
  `restart_tracker_init()`; when it is not, the report is neither read nor
  stored and the boot says so in one W line.
- Kconfig: `CONFIG_WICAN_RESTART_TRACKER_PARK_RETRY_MIN` (0..1440, default
  0): the crash park's own retry, see "The crash-loop brake".
- `esp_app_format` (the image id in a crash note), and two symbols private
  to IDF's `esp_system`: `esp_panic_handler`, `panic_restart` (see above).
  The crash file is Xtensa code; on a target that is not Xtensa or has no
  RTC memory the component builds `restart_tracker_crash_stub.c` and
  `restart_tracker_get_crash()` answers `ESP_ERR_NOT_SUPPORTED`.
- Requires `CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY=y` (enabled in the
  main sdkconfig 2026-07-03).
- Init order: right after `log_manager_init()`, before anything that could
  crash, so the boot record exists. No settings, no descriptor. Main calls
  `main_park_check()` right after it and `restart_tracker_settle(false)`
  from its 60 s loop; safe mode calls `restart_tracker_set_boot_mode(SAFE)`.
- **Firmware rule:** transports reboot via `restart_tracker_restart()` (the
  settings submit-then-reboot uses `CONFIG_APPLY`/`CONFIG_SERVER`), never raw
  `esp_restart()`: a raw restart shows up as planned=0/"software".

## Settings (`"restart_tracker"`, version 1)

Minimal descriptor, one knob: `cli` (bool, default true), register the
`restart_tracker` console command with cmdline_manager on the settings
boot apply (reboot-to-apply). Registered via
`restart_tracker_register_settings()`, wired by main right after
settings_manager_init because this component inits before it (the
log_manager_register_settings pattern, 2026-07-05).

## HTTP API (requirement: full endpoint reference: `HTTP_API.md` in this directory; conventions: `components/HTTP_API.md`)

`GET /api/restart/history` (boot records with to-str names, newest first; the
running image's `elf_sha`; a `crash` object on every record that follows a
recorded crash; each record's `mode` and `settled`; the `brake`; the stored
`report`), `GET /api/restart/report` (the stored report as text),
`DELETE /api/restart/report` and
`POST /api/restart` (respond, flush ≈1 s, then
`restart_tracker_restart(USER_REQUEST, WEB_UI, 0)`). Implemented by the
`api_http` glue: this component must NOT depend on the HTTP server. The
legacy `restart_tracker_http.c` was dropped; these routes replace it.

## Memory footprint

| Where | What | Size |
|---|---|---|
| PSRAM `.noinit` | `restart_tracker_state_t` (8-record ring, 64-byte tuning guard) | 520 B (map) |
| RTC slow memory `.rtc_noinit` | the crash note store: 8 + 12 the brake's count + 300 pending + 8 x 300 kept | 2720 B of 8192 (map; no heap, no internal RAM) |
| PSRAM `.bss` | the stored report as this boot knows it; this boot's verdict and the boot log's text buffers; the console command's buffers | 360 B; 524 B; 2912 B (map) |
| Internal | two spinlocks, three flags, the console command's argument table; the hooks' state | 64 B + 6 B (map) |
| Flash (code; strings not counted) | the crash note: hooks, filing, text (`restart_tracker_crash*.c`) | 2548 B (map) |
| | the brake's count and verdict (`_brake_core.c`) | 550 B (map) |
| | the stored report: record, wear guard, text, NVS calls (`_report*.c`) | 2227 B (map) |
| NVS | one blob, `rt_crash` / `report` | 360 B = 15 entries |
| Task stacks / heap | none (passive, caller context) | 0 |
| Stack, caller's | init on the boot task: two report records (720 B) and the NVS call under them | measured headroom of the 8192-byte boot task: 5612 B in a boot that stores a report, 5676 B in one that stores nothing |
| | `restart_tracker --report` on the console task (the text and its line buffers) | about 1.1 KB: 3824 B of 6144 left (measured) |
| | `GET /api/restart/history` and `/report` on the HTTP worker | no change to its headroom (5284 B, measured) |
| Flash writes | **0 in a boot that follows no crash, and none at runtime.** A boot that files a crash that is news: one NVS blob, measured 9 to 12 writes / 468 to 500 B / 0 erases; the same crash again: 0. Bounded by the wear guard above (standard §11: this is the one place the component moves `WICAN FLASH`, and only after a crash) | see left |

## Tests

- **Host (`host_test/`, 46 tests):** 15 for the brake
  (`test_brake_core.c`): which reset reasons are crashes; three quick
  crashes park; a settled run and any other reset end the streak; a park
  that retries keeps it and parks at once on the next crash; a retry that
  settles is healthy again; a park ended from outside starts over; a crash
  inside the park gives the bare park; safe mode is not braked; the retry
  time and the bench's one-shot knob; the report budget; 300 failed tries
  never fall back to a normal boot; the count outlives the PSRAM history
  and is reset with a foreign store or a bad CRC; the record's `mode` and
  `settled`. 8 for the stored report (`test_report_core.c`): the record's
  layout and validity; what makes two crashes the same one; the wear
  guard's decisions; a ten-day crash loop and 10 000 different crashes cost
  4 writes each; the text, byte for byte, its snprintf semantics, the
  longest report inside 1024 bytes. 15 for the crash note's pure half
  (`test_crash_core.c`): random memory is no note; each checksum catches a
  flipped bit; the pending note lands in its record's slot under the
  record's number and the pending slot is empty afterwards; a head-only note
  is filed incomplete; no pending note clears the slot; a foreign store or a
  fresh history clears the kept notes and still files the pending one;
  lookup never returns another boot's note, ring wrap included; the summary
  line of each kind; text made printable. And 8 for the pure core: garbage-memory detection,
  boot recording/counters, planned-intent consumed exactly once, unexpected
  classification (panic / the three watchdogs / brownout yes; sw / poweron /
  external / deepsleep / usb / jtag no; planned never) on the IDF 5+ reason
  numbers (INT_WDT 5, TASK_WDT 6, WDT 7, DEEPSLEEP 8, BROWNOUT 9, USB 11,
  JTAG 12: until 2026-10-03 the core carried IDF 4's, and an
  interrupt-watchdog reset was recorded as "deepsleep" and not counted as
  unexpected; until 2026-10-17 the USB-Serial-JTAG reset esptool issues
  after a flash over the native USB port, and a debugger's, counted as
  unexpected: ported from the ESPNetLink production station), the reason
  names, ring wrap, CRC tamper, invalid-clock handling.
- **On-target (`test_apps/`, `.\test.ps1 target restart_tracker`):** a
  self-driving app, one step per boot. Step 0 is the **real esp_restart()**
  of old (`.noinit` survival, intent consumption, software-reset
  classification). Steps 1 to 14 crash on purpose and check the note the
  next boot filed: an invalid store in a task (internal stack, PSRAM stack),
  `abort()`, an assert, the stack overflow hook, the interrupt watchdog on
  each core, a fault inside a timer interrupt, a call through NULL, a fault
  with the flash cache disabled, a corrupted frame chain, stage B made to
  fault and to hang, the store filled with garbage. The PC side
  (`tools/testbench/system/rt_target_check.py`, firmware repo) compares
  every note with IDF's own panic text of the same crash and gives
  `RT TARGET PASS` (79 checks, 2026-10-05). The app's link wraps three
  IDF functions as probes; the component carries no test hook. Main
  partition table per rev 2.1. Since 2026-10-05 the run's 13 crashes, all
  quick and all different, also prove the brake and the report on the real
  RTC memory and the DUT's NVS: the streak counts every one, the verdict
  turns to park at the third, the report is stored four times and not once
  more, and the run clears it at the end.
- **On the production firmware** (firmware repo, `.\test.ps1 crashnote`):
  `CRASH NOTE PASS` (the note, one small write per new crash, none for the
  same crash, the report word for word after a power cycle) and
  `CRASH PARK PASS` (three quick crashes park the device: console, HTTP
  silent, supply current against sleep mode; the timed exit; the next crash
  parks at once; a power cycle is a fresh start).

## CLI

`restart_tracker_register_cli()` (main, CLI builds) registers the `restart_tracker` command with cmdline_manager (`restart_tracker_cli.c`). The `system restart` command (main_cli.c) reboots through restart_tracker_restart with SOURCE_CMDLINE.

`-l` and `-a` print a record's crash note under it (`crash:` the summary,
`backtrace:` the PCs). `--panic` crashes on purpose, unannounced, in the
three ways the tracker tells apart: `--panic` or `--panic=abort` (`abort()`),
`--panic=fault` (a store to 0x0000BAD0: `StoreProhibited`), `--panic=wdt`
(interrupts off and spin: the interrupt watchdog). The bench gate
`.\test.ps1 crashnote` uses all three.

`--report` prints the stored crash report (the text of `GET
/api/restart/report`), the brake in one line (`Brake: verdict=normal
streak=0/3 parks=0 settled=yes report_budget=4`) and NVS's occupancy;
`--clear-report` forgets the report. Two knobs for benches that crash the
device on purpose: `--settle` marks this run healthy now, so the crash that
follows is a first crash and not one of a streak (three quick `--panic` in a
row DO park the device: power-cycle it to get it back); `--park-retry <s>`
makes the next park end by itself after 10..3600 s, once. A record of a boot
that parked or ran safe mode prints `mode:` under it.
