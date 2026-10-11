# external_storage: the SD/MMC card owner (HAL)

Owns the SD card as a DEVICE (Architecture §4): card-detect supervision
with hot-plug, the SDMMC 4-bit host, and the card's FAT mount at the `/sd`
VFS root. It knows nothing about what's stored: file access goes through
`filesystem` (`/sd/...` paths); the composition root glues this component's
mount events to `filesystem_sd_set_mounted()` (neither depends on the
other).

## Hardware (WiCAN Pro, meatpi 2026-07-04)

SDMMC slot via the GPIO matrix: CLK=21, CMD=47, D0=14, D1=13, D2=12,
D3=48; 4-bit width, internal pullups, `SDMMC_FREQ_HIGHSPEED` (driver
derates as needed). Card-detect = **GPIO40, active low** (socket switch to
GND, internal pullup: polarity confirmed live 2026-07-04).

## API

| Call | Behavior |
|---|---|
| `external_storage_init()` | Detect-pin GPIO + log descriptor; no card access. |
| `external_storage_start()` | Starts the detect task (250 ms poll, 4-sample debounce: the pure `es_detect` machine); mounts immediately when a card is seated. |
| `external_storage_stop()` | Unmount + stop the task. |
| `external_storage_is_present()` / `is_mounted()` | Debounced presence / mount state. |
| `external_storage_set_callback(cb)` | The ONE mount-event callback (composition root → filesystem backend flip). Fires `false` BEFORE unmounting so consumers stop routing first. |

Publishes `DEV_STATUS_BIT_SDCARD_MOUNTED`. Mount policy:
`format_if_mount_failed = false`, it's the USER'S card; an unreadable card
logs an error and stays unmounted. A yanked card degrades `/sd` operations
to `ESP_ERR_INVALID_STATE`, never a crash.

## Dependencies

`esp_driver_sdmmc`, `fatfs`, `esp_driver_gpio`, `dev_status_manager`,
`log_manager`: all private. No filesystem edge (glued in main).

## Memory footprint (measured 2026-07-26, `idf.py size-components`)

| Where | What | Size |
|---|---|---|
| Flash | code + rodata | 2.2 KiB |
| PSRAM `.bss` | detect-task stack (SD I/O uses the driver's own DMA buffers; no cache-off work on this stack, §2) | **4,096 B** |
| Internal `.bss` | TCB, state | **389 B** |
| Internal heap | SDMMC driver DMA buffers + FATFS work areas (driver/FATFS-owned, while mounted) | ~10 KB (estimated) |

## Tests

- **Host (`host_test/`, 3 tests)**: the pure detect debouncer, sub-threshold
  glitches never surface, clean insert/remove edges fire exactly once,
  seating wobble settles correctly. **Green on rpi001 2026-07-04.**
- **Live (2026-07-04, composed main, deployed via OTA)**: the bench card
  mounted at boot (`sdcard_mounted` bit set; a 16 GB SanDisk SC16G,
  `mounted /sd: SC16G 15193MB`, which this note called "a real 2.8 GB card"
  because `/api/fs/info` said so: the 32-bit cast of issue #982 in
  `filesystem_info()`, fixed 2026-10-11), existing FAT content listed
  via `/api/fs/list?path=/sd`, capacity via `/api/fs/info` (FATFS
  free-cluster query), upload → byte-identical download → delete on the
  card through the UI file-manager API. Hot-plug (remove/re-insert while
  running) exercises the same debounce+mount path: physically verify at
  the bench when convenient.

## CLI

`external_storage_register_cli()` (main, CLI builds) registers the `sdcard` command with cmdline_manager (`external_storage_cli.c`): presence/mount plus card model+size from the component's own sdmmc data; FILESYSTEM usage is the `fs` command (filesystem owns that).

## Settings (`"external_storage"`, version 1)

Minimal descriptor, one knob: `cli` (bool, default true), register the `sdcard` console command(s) with cmdline_manager on the settings boot apply (reboot-to-apply). Registered via `external_storage_register_settings()`, wired by main right after settings_manager_init because this component inits before it (the log_manager_register_settings pattern, 2026-07-05).
