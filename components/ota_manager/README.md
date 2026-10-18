# ota_manager: the firmware update manager (core service)

Owns the device's OWN firmware update: one byte-stream session
(`begin` → `write`× → `end`/`abort`) over the `ota_0`/`ota_1` partitions
(esp_ota). **Transport-agnostic by design**: transports feed the session,
HTTP multipart upload from the web UI today (`api_http`,
`POST /api/ota/upload`), raw HTTP body for tooling, and TCP/MQTT/anything
later through the same four calls, with no changes here.

Naming: "OTA" = this device's application image. The OBD chip's firmware
update is a different mechanism (`obd_chip_firmware_update`).

## API

| Call | Behavior |
|---|---|
| `ota_manager_init()` | Log descriptor, PSA init; no flash access. |
| `ota_manager_start()` | Logs the running partition and whether the image check is on. (The confirm of a pending image moved to fw_update's verify milestone on 2026-10-18: a run must earn it, or a rollback-enabled bootloader could never fall back.) |
| `ota_manager_set_product(name, prefixes, n)` | 2026-10-18: names the product. An image whose descriptor's project name starts with none of the prefixes, or that is not an ESP32-S3 application image, is refused at its first 288 bytes with `not a <name> image: <project>` and never reaches the flash. Without this call every image passes (the old contract). main sets "WiCAN PRO" with `wican-fw_obd_pro` and `wican-pro`. |
| `ota_manager_expect_sha256(sha)` | 2026-10-18: arms a digest for the NEXT begin (the upload route's `?sha256=`): `end()` fails `sha256 mismatch` and activates nothing when the bytes differ. |
| `ota_manager_confirm_running()` | 2026-10-18: `esp_ota_mark_app_valid_cancel_rollback` when the running image is `NEW` or `PENDING_VERIFY` (fw_update's milestone, the pre-restart hook, every begin). One otadata write; the caller's stack must be internal. |
| `ota_manager_begin(total_or_0)` | Opens THE session against the next OTA partition (`0` = size unknown, e.g. multipart) after confirming a pending running image. `ESP_ERR_INVALID_STATE` while another session is receiving; begin from FAILED = retry, from READY = re-upload. |
| `ota_manager_write(data,len)` | Stream image bytes, any chunking. The first 288 bytes are held back and checked (the image magic, the chip id, the descriptor magic, the project name), then everything flows to the flash and the SHA-256 (PSA). Overrun of an announced size / a refused head / backend failure → FAILED + flash session released. |
| `ota_manager_end()` | Compares the digest when one was expected, validates (esp_ota magic + digest), sets the boot partition → READY. The TRANSPORT then reboots via `restart_tracker_restart(OTA_APPLY, <source>)`. |
| `ota_manager_abort()` | Drop the session; the running firmware is untouched. |
| `ota_manager_status(*out)` | state / received / total / error / target partition / the image's `project_name` and `version` once its head is in / `piped` (feeds `GET /api/ota/status`). |
| `ota_manager_pipe_begin/write/end/abort` | 2026-10-18 (`ota_pipe.c`): the session for a producer on a PSRAM stack (fw_update's download task). Begin, every write, end and abort run on a 5 KB internal-stack writer created for the session (its stack goes back to the heap at end or abort); the producer copies into two 4 KB PSRAM slots and blocks while both are full. One session at a time with the direct API: an upload while the pipe receives (or the other way round) is `ESP_ERR_INVALID_STATE`. |
| `ota_manager_img_state_str(st)` | `new` / `pending_verify` / `valid` / `invalid` / `aborted` / `undefined`. |

A failed or aborted session can never hurt the running firmware: the
inactive partition is scratch space until `end()` flips the boot pointer,
and `esp_ota_end` rejects images that don't validate; since 2026-10-18 a
foreign image (an ESPNetLink's on a WiCAN PRO) or one whose digest differs
from the manifest's never gets that far.

## Dependencies

`app_update` (esp_ota), `log_manager`, `mbedtls` (SHA-256 through the PSA
API: mbedtls 4.1 has no `mbedtls_sha256` API): private. No transport
dependencies (the whole point). The reboot is the transport's job through
`restart_tracker`: this component never restarts anything.

## Memory footprint

Static: the session struct (about 450 B with the 288-byte head and the
expected digest) + mutex, the PSA hash operation (about 300 B), the pipe's
two 4 KB PSRAM slots and their queue control blocks. esp_ota's own write
buffering is IDF-internal; the HTTP transport's 4 KB rx buffer is
request-scoped heap (§12b). The pipe's writer stack (5 KB) is internal heap
for the session only; it logs its headroom before it ends. No steady-state
allocations.

## Tests

- **Host (`host_test/`, 15 tests)**: the pure session state machine against
  a recorder backend, happy path, write-before-begin, double-begin,
  announced-size overrun, short image, empty image, backend-failure
  latching (+ no double-abort after validation failure), retry-from-FAILED,
  abort-resets; 2026-10-18: the head check (our names accepted incl. the
  v4.5x and the constant one, the ESPNetLink's refused with the text, a bad
  image magic, no descriptor, another chip, one-byte chunking, an image
  shorter than the head), the digest (match, mismatch with the slot aborted,
  none, the expectation gone with its session), the old contract without a
  product. `.\test.ps1 host ota_manager`, or `bash
  tools/testbench/host_gcc_wsl.sh ota_manager` under WSL.
- **Bench**: `tools/testbench/system/fw_update_bench.py` legs f3 (the pipe),
  f4, f5 (the digest and the product refusals on both transports), f12 (the
  mutual exclusion).
- **Live (2026-07-04, the real thing)**: 1,510,992-byte image uploaded over
  Wi-Fi to the composed firmware via BOTH transports, multipart
  (`curl -F firmware=@wican-fw.bin`, the HTML-UI path) and raw body
  (`curl --data-binary`), **~8.3 s each (~180 KB/s incl. flash writes)**;
  partition flipped ota_0→ota_1→ota_0, device self-rebooted via
  restart_tracker, history records `planned ota_apply web_ui`, memory
  healthy after. Serial-flash recovery is safe: `flash_args` includes
  `ota_data_initial.bin`, resetting boot to `ota_0`.
