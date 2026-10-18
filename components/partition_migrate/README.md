# partition_migrate: the partition table in flash becomes this build's

**Summary.** An OTA writes an app slot only, so a unit updated from a
firmware with another partition layout keeps that layout. On the WiCAN Pro
the factory firmware (v4.5x) has ONE 6 MB `storage` at 0x9EE000 where v6
keeps its 256 KB `settings` partition and `storage` behind it; after the
update to v6.00p_alfa-02 through the legacy OTA page nothing could persist
(`esp_littlefs: partition "settings" could not be found`, 41 x `persist
failed`, `FAULT boot_errors: 86 error lines at boot`, a factory reset that
answers 500: the 2026-10-10 field report, reproduced by
`tools/testbench/system/ota_from_factory_bench.py`). This component reads
the table in flash at every boot, compares it with the table this build
was made with (the build's `partition-table.bin`, embedded), and when only
data partitions differ rewrites it and asks the caller to restart. Sits
with the core services: `main` runs it right after `restart_tracker_init()`
and before anything mounts a data partition.

**The rule (pure, host-tested).** Entries are ANCHORED when the bootloader
or the radio depends on them: every app slot (`ota_0`, `ota_1`, `factory`),
`otadata`, `phy_init`, `nvs`. The table in flash is migrated only when
every anchored entry is identical in both tables, both ways; then the
bootloader finds the same image in the same slot after the rewrite and the
WiFi calibration and NVS keys stay. Anything else is FOREIGN and left
alone with one error line (the boot_errors fault then says so). A table
with more than 16 entries, a bad magic or a zero size does not parse and
is foreign too.

**What the data partitions' owners do afterwards.** `settings_manager` and
`filesystem` probe their superblock pair before the mount (2026-10-06):
the legacy LittleFS under `settings` (1536 blocks on a 64-block partition)
is formatted with one W line, the middle of the legacy filesystem under
`storage` (no superblock) likewise. The legacy settings are gone, as the
v6 release note says; the user's NVS and calibration are not. The way
back works too: the legacy firmware uploaded through v6's OTA page boots
under v6's table, mounts v6's `storage` by its name (5888 KB) and writes
its defaults there; v6 uploaded again through the legacy page finds its
`settings` untouched (`tools/testbench/system/downgrade_bench.py`).

## API

| Call | Behavior |
|---|---|
| `partition_migrate_init()` | Registers the log descriptor. No flash access. |
| `partition_migrate_run(allow_rewrite, &result, &ms)` | Reads the 3 KB table region, compares, and with `allow_rewrite` rewrites a migratable table and verifies it by reading it back. `result`: `OURS` (the usual boot, one D line), `REWRITTEN` (the caller restarts now; `ms` = erase + write), `NEEDED` (migratable but not allowed, or built without the flash flag), `FOREIGN` (left alone, one E line), `FAILED` (read, erase, write or read-back failed, one E line). Returns `ESP_OK` for every verdict and an `esp_err_t` only for FAILED. Call from a task with an internal-RAM stack: the write runs with the cache off. |
| `partition_migrate_result_to_str(r)` | `ours` / `rewritten` / `needed` / `foreign` / `failed`. |
| `partition_migrate_flashed_md5(out[33])` / `_build_md5(out[33])` | 2026-10-18: the layout id, the MD5 of the table's 32-byte entries in flash order, which is the digest IDF appends to every table (`CONFIG_PARTITION_TABLE_MD5`) and what the meatpi-devices manifests key their verdicts on (`layout.md5`, `layout_from`): the WiCAN PRO v4.x factory table is `225242e6822a60aa93a9a67abd9442ed`, the v6 table `35c16cb5677477a27c662bd912582329`. The flashed one is cached from the boot read in `partition_migrate_run()` (false and "" before, or when no table parses); the build's own comes from the embedded table. `/api/info.layout_md5`, `GET /api/fw_update targets.self.layout`. Pure core: `pm_table_md5()` and an RFC 1321 `pm_md5()` in `partition_migrate_md5.c`. |

The caller owns the restart and the loop guard. `main_boot_layout()`
(main/main_boot.c) passes `allow_rewrite = false` when the restart tracker
says this boot follows a `partition_migrate` restart, so a table that is
still foreign after a rewrite logs one error and the boot carries on
degraded instead of looping; a rewrite restarts through
`restart_tracker_restart(PARTITION_MIGRATE, BOOT, 0)`. The boot that
follows the migration drops the `boot_errors` fault that the boots
without a settings partition had latched (`dev_status_manager_fault_clear`,
2026-10-10): a user who just updated must not see "1 fault".

## Dependencies

`spi_flash` (`esp_flash_read` / `esp_flash_erase_region` / `esp_flash_write`
on the main chip), `heap`, `esp_timer`, `log_manager`: all private. Init
order: after `log_manager_init()`; before `filesystem_init()` and
`settings_manager_init()`.

**Needs `CONFIG_SPI_FLASH_DANGEROUS_WRITE_ALLOWED=y` in the app** (WiCAN
Pro: `sdkconfig.defaults`, 2026-10-10). The IDF flash driver otherwise
aborts (`_ABORTS`, the default) or refuses (`_FAILS`) any write below the
end of the partition table. Without the flag the component compiles, logs
one error and reports `NEEDED`: never an abort. The flag lifts the guard
for the whole app, so no other code may write below `0x9000`; the running
app's own slot stays protected by the driver regardless.

The embedded table comes from `${BUILD_DIR}/partition_table/partition-table.bin`
with a dependency on IDF's `partition_table_bin` target, so a project that
changes its CSV changes the table this component enforces, and a project
without a partition table (host builds) compiles only the core.

## Settings

None. No console command, no HTTP route.

## Flash-write discipline (standard section 11)

- Every boot: one 3 KB read (`esp_flash_read`), no write. The usual boot
  measures `WICAN FLASH writes=0 erases=0` as before.
- Once in a unit's life, when the table is migratable: one 4 KB sector
  erase at `CONFIG_PARTITION_TABLE_OFFSET` (0x8000) and one 3 KB write,
  then a read-back. **Measured on the WiCAN Pro (16 MB QIO flash, 80 MHz,
  2026-10-10, `partition_migrate_bench.py`): `partition table rewritten in
  22 ms (erase 19 ms, write 3 ms)`.** The window in which the flash holds
  no valid table is that erase + write, about 22 ms; a power cut inside it
  leaves a device the bootloader refuses to start, recoverable only by a
  PC flash. The erase cannot be avoided: the entries that change set bits
  from 0 to 1.
- Never twice in a row: the caller's loop guard above.

## Memory footprint

- Static: the embedded table, 3 KB of `.rodata` (flash-mapped), plus the
  `TAG` and the log descriptor. No `.bss` of note (the parse buffers,
  2 x 16 entries x 36 B, live on the caller's stack for the call).
- Task stacks: none (runs in the boot task).
- Heap: one 3 KB internal buffer (`MALLOC_CAP_INTERNAL`) for the duration
  of `partition_migrate_run()`, freed on return. Internal on purpose: the
  write source must be reachable with the cache off. Measured 2026-10-10:
  the boot RAM map step `partition_migrate_run` shows 0 B retained.

## Tests

- `host_test/`: 14 Unity tests on the pure core with the two real tables
  (2026-10-18: + the RFC 1321 vectors and the two tables' ids against the
  digest IDF appended, with and without the MD5 entry, blank and garbage)
  as byte vectors (the v4.51p release's `partition-table.bin` and the v6
  build's): same, migrate, a moved app slot, a changed `nvs`/`otadata`, a
  missing `phy_init`, blank flash, garbage, an extra data partition, the
  MD5 entry as the terminator, too many entries, an unparsable own table.
  `.\test.ps1 host partition_migrate`.
- `tools/testbench/system/partition_migrate_bench.py`: the legacy table
  written over a running v6 on the bench DUT, a PSU cold boot with the
  console: the W line, `rewritten in N ms`, the `partition_migrate`
  restart record, then a clean boot with the settings intact (only the
  table was swapped, so nothing is formatted); a second boot quiet.
- `tools/testbench/system/ota_from_factory_bench.py`: the field path end
  to end (v4.51p, the v6 image through the legacy OTA page, the first v6
  boot migrates, saves work, the factory reset works, the downgrade);
  `--via <stuck image>` adds the rescue of a unit already on alfa-02
  (the fixed image through v6's own OTA page).
