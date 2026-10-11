# filesystem

## Summary

The firmware's general-purpose file API (core layer, ARCHITECTURE.md §5). Owns the
internal-flash data filesystem (LittleFS on the `storage` partition, mounted at
`/data`) and presents one path-based interface that routes logical paths to the
right backend. `/sd/...` is reserved for the SD card backend and reports
`ESP_ERR_INVALID_STATE` until `external_storage` exists and is wired up (v1 ships
the internal backend only). The `settings` partition is deliberately **not** served
here: `settings_manager` owns it for fault isolation.

Pinned design decisions (closes REVIEW.md §6.1):

- **Path namespace:** callers address a backend by logical prefix (`/data`, `/sd`).
  No transparent fallback between backends.
- **SD absent:** operations on `/sd/*` fail with `ESP_ERR_INVALID_STATE`; callers
  decide their own fallback policy.
- **Writes are always atomic:** `filesystem_write()` = temp file + `fsync` +
  `rename`, and auto-creates parent directories.
  On the card's FAT a `rename` over an existing file fails (errno 17), so
  the replace there is unlink + rename (`fs_rename_replace`, 2026-10-11,
  found under config_backup's copies): a power cut between the two calls
  leaves the temp file beside no target, which readers treat as absent. A power cut leaves the old file
  or the new one, never a torn one.
- **`list` is part of the API**: the settings UI needs it to enumerate candidates
  for the schema `format:"file"` keyword.

## API

| Function | One-liner |
|---|---|
| `filesystem_init()` | Mount the internal data FS (formats on first use). |
| `filesystem_start()` / `filesystem_stop()` | Uniform lifecycle; `stop` unmounts (re-`init` allowed, test flows). |
| `filesystem_write(path, data, len)` | Atomic replace; creates parent dirs. |
| `filesystem_read(path, buf, buf_len, *out_len)` | Whole-file read; `ESP_ERR_INVALID_SIZE` + required size if `buf` too small. |
| `filesystem_size(path, *out)` | File size in bytes. |
| `filesystem_exists(path)` | `bool`; false on any error. |
| `filesystem_delete(path)` | Unlink file / remove empty dir. |
| `filesystem_mkdirs(path)` | Ensure a directory chain exists. |
| `filesystem_list(dir, cb, ctx)` | Enumerate entries (name, is_dir, size); cb can stop early. |
| `filesystem_open(path, mode)` | Validated `FILE*` for streaming (chunked HTTP serving). Streaming writes are **not** atomic. |
| `filesystem_info(prefix, *total, *used)` | Backend capacity. |

Errors: `ESP_ERR_INVALID_ARG` (bad path, wrong prefix, `..`, `//`, trailing `/`,
≥128 chars), `ESP_ERR_INVALID_STATE` (not inited / backend unavailable),
`ESP_ERR_NOT_FOUND`, `ESP_ERR_INVALID_SIZE`, `ESP_ERR_NO_MEM`, `ESP_FAIL`.

## Dependencies

- `joltwallet/littlefs` (managed, private): the internal FS.
- `vfs` (private): POSIX layer.
- Init order: `filesystem_init()` runs in `main` after `external_storage_init()`
  (once that exists) and before anything that reads/writes files
  (`download`, `http_server_manager`). See ARCHITECTURE.md §11.

## Settings (`"filesystem"`, version 1)

Minimal descriptor, one knob: `cli` (bool, default true), register the
`fs` console command with cmdline_manager on the settings boot apply
(reboot-to-apply). Registered via `filesystem_register_settings()`,
wired by main right after settings_manager_init because this component
inits before it (the log_manager_register_settings pattern,
2026-07-05).

## HTTP API (requirement: full endpoint reference: `HTTP_API.md` in this directory; conventions: `components/HTTP_API.md`)

Read-only in v1, via the `api_http` glue: `GET /api/fs/list?path=…` (drives
the settings-UI `format:"file"` picker) and `GET /api/fs/info?path=…`
(capacity). Path validation is this component's own (`fs_path_resolve`).
Write/delete/upload routes are deliberately deferred until an auth story
exists.

## Concurrency & memory

- One recursive mutex serializes all operations. `filesystem_open()` handles are
  caller-owned stdio streams outside that serialization.
- All file data bounces through a 2 KiB **internal-RAM** scratch buffer
  (`// internal: cache-off during FS write`), so caller buffers may live in PSRAM.
- Standard §2 corollary still applies to callers: no `filesystem_write()` from a
  task with a PSRAM stack.

## Memory footprint (estimated: replace with measured before release)

| Where | What | Size |
|---|---|---|
| Internal `.bss` | scratch buffer + mutex | ~2.2 KiB |
| PSRAM `.bss` | state struct | < 32 B |
| Heap (internal) | LittleFS mount (esp_littlefs caches) | ~4–8 KiB while mounted |
| Task stacks | none (runs in caller context) | 0 |

## First boot, and a partition that is not ours

A never-formatted `storage` partition reads as erased flash (0xFF). Letting
`esp_vfs_littlefs_register()` discover that makes `lfs_mount()` log
"Corrupted dir pair" at **E** level before `format_if_mount_failed`
formats it: two E lines that latched a `boot_errors` fault on every
brand-new unit (fresh-unit bench 2026-08-31). Since 2026-09-05 the mount
probes the superblock pair (blocks 0 and 1) first and formats a blank
partition explicitly (one **I** line, no mount attempt). Since 2026-10-06
the probe is `fs_lfs_probe()` (`filesystem_path.c`, pure, host-tested) and
`fs_mount_prepare()` (`filesystem_mount.c`) also formats, with one **W**
line, a pair that holds a LittleFS superblock of another block count or no
superblock at all: a unit that ran the factory firmware (legacy v4.5x)
keeps that firmware's 6 MB LittleFS under our layout, `settings` starts on
its superblock and this partition starts in its middle (data blocks, or
erased ones). LittleFS mounts a superblock whatever block count it claims
(the ESP port takes the count from the superblock), which is how the
factory filesystem "mounted" on the 256 KB settings partition until that
day (83 E lines, a `boot_errors` fault, no setting persisted: Ali's fresh
unit). A LittleFS of our own size that fails to mount still takes the loud
path on purpose: that is real corruption. settings_manager does the same
for its `settings` partition with its own copy of the probe (it sits
below this component). Verified: erase-flash + first boot = 0 E lines,
`WICAN FAULTS active=0`; a factory-style filesystem under a first boot
(`tools/testbench/system/factory_flash_bench.py`, `FACTORY FLASH PASS`
2026-10-06) = 0 E lines, the two W lines, no fault, the defaults persisted.

## Tests

- **Host (Unity, `host_test/`):** pure path logic (prefix routing, traversal/`//`
  rejection, length caps, temp-name and parent derivation) plus the
  blank-flash predicate (`fs_region_is_blank`: erased, formatted magic,
  single stray byte, empty/NULL) and the superblock probe (`fs_lfs_probe`:
  blank, ours, the factory firmware's 1536 blocks, one erased block of the
  pair, the entry deeper in a block; a FAT boot sector, data blocks, the
  name without a struct, another block size, a version-1 filesystem, a
  struct cut off by the probe's length, bad arguments). 13 tests. `idf.py --preview
  set-target linux && idf.py build` (Linux host required), run the ELF.
- **On-target (`test_apps/`):** mounts the real `storage` partition and exercises
  write/read roundtrip, atomicity (no temp leftover), nested auto-create,
  list/size/delete, bad-path rejection, `/sd` unavailability, and persistence
  across a stop/re-init remount. Builds against the **main firmware's partition
  table** (`wican_pro_partitions_table.csv`) per Coding Standard rev 2.1 §7.

## CLI

`filesystem_register_cli()` (main, CLI builds) registers the `fs` command with cmdline_manager (`filesystem_cli.c`): per-backend usage for /data and /sd.
