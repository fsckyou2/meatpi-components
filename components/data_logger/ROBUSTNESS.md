# data_logger: robustness against power cuts, crashes and corrupt files

Written 2026-09-07 after the bench unit's params database went "disk image
is malformed" (many hard resets that day) and the logger sat in a loop
(reopen, insert fails, reopen) dropping every record. meatpi's brief:
*"use the PSRAM to reduce the number of writes; recover as much data as
possible; a corruption must never stop us from booting and must be
flagged; stop writing on a user restart and before sleep; map out the
fail cases and work through them."*

Note on power: the OBD port stays powered with the ignition off, so a
power cut is a battery disconnect, an unplug, a brown-out or a reset,
not every drive. It still has to be survivable.

## Where the data sits

```
producers ──► PSRAM rings (params 2048 rec, frames ≤8192 rec) ──► writer task ──► card
              .noinit envelope: survives warm resets                 one batch per
              (registry + both rings, validated at boot)             flush_ms / batch_rows
```

A record is in exactly one of three places: the PSRAM ring, the batch in
flight (inside one sqlite transaction / one stdio buffer), or committed
on the card. The table below says what happens to each place per case.

## Fail-case matrix

| # | Case | Before | Now | Still at risk |
|---|---|---|---|---|
| 1 | Power cut / EN reset **mid-commit, sqlite** | `journal_mode=MEMORY` and the port compiled with `SQLITE_NO_SYNC`: a torn commit left the file malformed; the logger reopened the same file forever and dropped every record | The port syncs again (`SQLITE_NO_SYNC 0`: `esp32_Sync` = fflush+fsync), `journal_mode=PERSIST` (rollback journal on the card, header zeroed per commit: no create/delete churn) + `synchronous=FULL`: a commit is atomic, the next open rolls a hot journal back | the batch in flight (≤ `flush_ms` of samples) |
| 2 | Power cut mid-write, **text** files (csv / jsonl / candump / asc) | the torn last line stayed; the next boot appended after it | resume cuts the file back to its last newline (`dl_recover_text_keep`, `truncate()`) before appending | the torn line + the batch in flight |
| 3 | Power cut mid-write, **binary .wdl** | a torn record made everything after it unreadable; the next boot appended after the garbage | resume walks the file and truncates to the last complete record (`dl_recover_wdl_scan`); files over 16 MB are not resumed (the walk is a full read): a new file starts. `wdl_dump.py` already stops at a torn tail | the torn record + the batch in flight |
| 4 | Power cut, **mf4 / blf** | single-use files with in-place header patches per commit | unchanged: a cut leaves stale header counts, readers see fewer records | the batch in flight |
| 5 | **Corrupt file** found: at resume (`PRAGMA quick_check`, files ≤ 8 MB) or at a write (`SQLITE_CORRUPT` / `SQLITE_NOTADB`) | reopen loop, no flag, every record dropped | the file is set aside as `<name>.corrupt` (its `-journal` deleted, at most 2 set-aside files per stream), fault code `logger_file_corrupt` latches in NVS (System Monitor, `/api/faults`), what sqlite can still read is copied into a fresh file with the **old name** (≤ 120 s budget; it keeps its place in the series. The salvaged file has no `ts` index: the port's VFS has no temp files for the sort, so the index build fails on the device and is logged as a warning, the file is complete, queries walk it; `CREATE INDEX records_ts ON records(ts)` on a PC adds it), logging continues in a **new** file. `/api/logger` reports `corrupt`; the Logger page shows a banner; the File Manager labels the file; `tools/db_recover.py` does the thorough pass on a PC | rows sqlite cannot read |
| 6 | **Crash / watchdog / restart without the clean stop** | the rings were `.bss` (zeroed at boot): every queued record lost | registry + rings live in a PSRAM `.noinit` envelope (magic, version, caps, CRC over the registry names, index sanity, per-record sanity: `data_logger_recover.c`); the next boot writes them **before** anything new and reports `salvaged` | nothing queued; the batch in flight is committed or rolled back (case 1) |
| 7 | **User restart** (web UI / CLI / MQTT), settings apply, OTA, factory reset | `esp_restart()` mid-write | an `esp_register_shutdown_handler` hook flushes and closes both files (synchronous `data_logger_stop`, ≤ 3 s) before the reset | nothing |
| 8 | **Sleep entry** | `data_logger_stop()` only flagged the writer; main unmounted the card a few calls later, possibly with files open | `data_logger_stop()` waits for the writer to park (files closed) before returning | nothing |
| 9 | Card removed while logging | writer parks, rings keep absorbing (drop-oldest), resumes on mount | unchanged | ring overflow while the card is out |
| 10 | Card full / persistent write errors | errors counted, handle dropped, retried every 2 s (log spam) | same, plus after 3 consecutive open failures a `logger_storage_error` fault and a 10 s back-off | new records once the ring overflows |
| 11 | FAT-level corruption / mount failure | external_storage reports no card; writer parks | unchanged: nothing the logger does runs on the boot path (the writer task is asynchronous), so a bad card never stops the boot | everything until the card is fixed |
| 12 | Clock not set / stepped | monotonic names (`epoch+1`); rows at 1970 + uptime until SNTP, then every later row jumped; the relative formats went negative on a backward step; nothing said so | (2026-10-13, issue #984, README "Time") every record carries its monotonic stamp, the writer re-bases the rows queued before a step, the relative formats take their time from the monotonic clock, the first valid clock and any step of a minute or more rotate the files (a boot without a clock never resumes a synced file), the clock journal `/sd/logs/clock.jsonl` and the in-band markers say what happened, `tools/log_clock.py` re-bases old files from it | rows written before the first valid clock keep 1970 + uptime (journaled); a device that never syncs cannot be re-based |
| 13 | EN-pin reset / flash on the bench | - | PSRAM keeps most bytes but with bit errors: the envelope's CRC rejects it and the logger starts clean (seen: restart_tracker reports a fresh state after every EN reset) | queued records (as cases 1 to 3) |

## Fewer writes

The rings already batch in PSRAM; the write cadence is `batch_rows` (256)
or `flush_ms`. `flush_ms` now defaults to **5000 ms** (was 1000): five
times fewer commits, and with case 1 fixed each commit is one journal
write + two fsyncs, so the card sees far fewer, larger, atomic writes.
The trade: a power cut costs up to 5 s of samples; a crash or a restart
costs nothing (cases 6 to 8). The param ring grew to 2048 records so a
5 s batch at the fastest autopid rate is a small fraction of it.

## Two rules learned on the bench (2026-09-07)

- **No flash writes from the writer task.** Its stack is in PSRAM; a
  flash write turns the cache off and the stack with it (the
  `cache_utils.c:126` assert). Fault codes are latched from a short-lived
  internal-stack task (`dl_store_fault`, data_logger_store.c), never inline.
- **Trust nothing that answers nothing.** With `SQLITE_OMIT_INTEGRITY_CHECK`
  in the port, `PRAGMA quick_check` returned no row and the first build
  set every good file aside. The port now builds the check, yet it still
  answers nothing (open question, `rc 101`), so a missing answer counts
  as *unknown*, never as corrupt, and the effective resume check is the
  `SELECT COUNT(*)` scan of each table that `sq_open` runs anyway: it
  walks the whole b-tree and returns `SQLITE_CORRUPT` on a torn file.

## Boot safety

- `data_logger_init` only adopts the PSRAM envelope and registers
  settings; `data_logger_start` creates the writer task. No card IO on
  the boot path.
- A corrupt or unreadable file is set aside inside the writer task; the
  main loop, HTTP and everything else never wait for it.
- `quick_check` runs only for files ≤ 8 MB (≈ a few seconds on the
  card); bigger files are trusted until a write fails.
- Salvage is attempted **once**, right after detection: a crash during
  salvage leaves `<name>.corrupt` (never retried) and a valid, partial
  `<name>.db` that the next boot simply resumes.

## Surfaces

- `GET /api/logger`: `salvaged` (records carried over the last warm
  reset), `corrupt` (set-aside files on the card).
- Fault codes: `logger_file_corrupt` (detail = the file), `logger_storage_error`.
- Events: `logger.error {count}` fires for "corrupt" as it does for
  "write".
- Logger page: a banner while set-aside files exist; a chip when records
  were salvaged. File Manager: `.corrupt` files carry an explanation.
- `tools/db_recover.py <file.db.corrupt>`: the thorough PC-side recovery
  (`sqlite3 .recover`, falling back to a row walk).

## Verification

- Host: `host_test/`, text tail, .wdl resync, set-aside names, record
  and ring sanity, CRC (test_recover.c); 20 tests pass.
- Bench, 2026-09-07, the unit's own malformed params DB (3.4 MB):
  - **Case 5**: resumed, the first INSERT returned "database disk image
    is malformed", the file was set aside as `dl_1784563547.db.corrupt`,
    `logger_file_corrupt` latched (header badge "1 fault", System
    Monitor), the salvage copied **101,547 rows in 75 s** into
    `dl_1784563547.db` (PC: `PRAGMA integrity_check` = ok, 11 params; no
    `ts` index, see case 5), and again 101,547 rows in 73 s on a re-run,
    logging continued in `dl_1784563548.db`. `/api/logger` `corrupt:2`,
    `errors:1` (that one INSERT), the Logger banner and the File Manager
    hint show. No panic, no boot impact.
  - **Case 7**: a settings submit and a `POST /api/restart` both logged
    "restart: flushing and closing the log files" → "stopped: log files
    flushed and closed" before the reset; the next boot resumed the
    file with no error.
  - **Case 6**: `restart_tracker --panic` over the console bridge →
    boot with `reset=panic`, "salvaged 47 record(s) queued before the
    last reset", `/api/logger` `salvaged:47`; after the clean API
    restart the records produced between the stop and the reset came
    back the same way (51).
  - **Case 8**: `sleep test 8` → "entering sleep (13.38 V)" →
    "data_logger: stopped: log files flushed and closed" → naps → wake
    by reboot; the next boot resumed the file with no error.
  - **Case 13**: every EN-pin reset that day gave `restart_tracker`
    "prev state invalid": the envelope starts clean the same way.
- Not bench-tested: the text/.wdl torn-tail repair (host-tested), card
  removal (unchanged code), card full.
