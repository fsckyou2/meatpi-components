# restart_tracker: host unit tests

The component's four pure files (no esp_cache, esp_timer, NVS or panic
hooks), IDF `linux` target. Run via `.\test.ps1 host restart_tracker`.

## What is covered (46 tests)

### `test_core.c`: the restart history (`restart_tracker_core.c`), 8 tests

| Case | What it proves |
|---|---|
| random memory is invalid and resets | power-on garbage never masquerades as history (magic/version/CRC) |
| boot recording and counters | one record per boot; sequence + boot_count advance; valid state adopted |
| planned restart consumed once | intent recorded into exactly the next boot, then cleared |
| unexpected classification | panic, the watchdogs and a brownout count; sw/poweron/external/deepsleep/usb/jtag don't (usb = esptool's reset after a flash over the native USB port, jtag = a debugger, 2026-10-17); a planned boot never counts (IDF 5+ reason numbers, the names to the end of the enum) |
| history ring wraps | 8-entry ring arithmetic; newest sequence at latest index |
| CRC detects tamper | one flipped bit invalidates the state |
| tuning guard clobber is harmless | the 64 sacrificial bytes may be overwritten at every boot |
| invalid time stored as zero | pre-2024 clock → timestamp 0 + time_valid 0 |

### `test_crash_core.c`: the crash note (`restart_tracker_crash_core.c`), 15 tests

Layout (300-byte note, 2720-byte store); random memory is no note; each of
the two checksums catches a flipped bit; the pending note lands in its
record's slot under the record's number; a head-only note is filed
incomplete; no pending note clears the slot; a foreign store or a fresh
history clears the kept notes and still files the pending one; lookup never
returns another boot's note, ring wrap included; a bad kind or slot is
refused; the summary line of each kind; text made printable; kind names.

### `test_brake_core.c`: the crash-loop brake (`restart_tracker_brake_core.c`), 15 tests

| Case | What it proves |
|---|---|
| layout and validity | 12 bytes at offset 8 of the store; garbage, all-zero and a scribbled count are invalid |
| which resets are crashes | panic, the three watchdogs, CPU lockup: yes; power, brown-out, EN, a restart call: no |
| three quick crashes park | streak 1, 2, then `PARK` at 3, parked for good by default |
| a settled run ends the streak | its own later crash is one crash of a healthy run; three quick ones are needed again |
| any other reset ends the streak | a planned restart, a wake, the EN pin, a brown-out |
| a park that retries keeps the streak | the start after it is one try; a quick crash parks at once (streak 4, parks 2) |
| a retry that settles is healthy again | streak and parks back to 0 |
| a park ended from outside starts over | no retry mark (a flasher, the EN pin): a fresh start |
| a crash inside the park parks bare | `PARK_BARE`, and it stays bare; a bare park that retries is a try like any other |
| safe mode is not braked | a crash of it, or leaving it, starts over; an unknown mode is recorded as normal |
| retry time | the product's own retry; the bench's knob is kept over boots that do not park, used by one park, gone for the next |
| report budget | four, then none; boots do not give it back, a settled run does |
| a long loop stays parked | 300 failed tries: counters stop at 255, the verdict never falls back to a normal boot |
| the count outlives the PSRAM history | a fresh history clears the notes and keeps the count; a scribbled count starts from zero and keeps the notes; a foreign store resets both |
| record fields and names | `mode` and `settled` land in this boot's record with a valid CRC, a new record starts clean; the new names |

### `test_report_core.c`: the stored crash report (`restart_tracker_report_core.c`), 8 tests

| Case | What it proves |
|---|---|
| layout and validity | 360 bytes; garbage, a flipped bit, another size or version, and a record around something that is no note are all invalid |
| build fields | the date (a clock that was never set is no date), streak, parked, the firmware string short, exactly full and too long |
| identity | when, which boot, which core's leftovers do not make another crash; place, image, task, a caller, the chain's length, the message do; an impossible frame count reads no memory past the note |
| decide: first, same, other | nothing stored or a foreign blob: store; the same crash a minute on: keep, and that needs no budget; another crash: store, budget allowing |
| decide: news about the same crash | "parked" once; the date after a day, not after 23 hours; a copy without a date gets one, a dated copy is never replaced by an undated one |
| a crash loop costs a bounded number of writes | the usual loop (one crash, parked, a failed retry every ten minutes for ten days): 4 writes; 10 000 different crashes that never settle: 4 writes; a settled run gives the budget back |
| text | the report byte for byte, snprintf semantics (a 40-byte buffer gets the start and the full length), null arguments |
| text of what was not recorded | a head-only note stored by another image before the clock was set; a watchdog with the other core's frames; the longest report there can be stays under 1024 bytes |

## Expected result

```
46 Tests 0 Failures 0 Ignored
OK
```
