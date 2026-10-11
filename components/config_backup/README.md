# config_backup: the backup that carries everything (issue #979, 2026-10-11)

The configuration backup of a WiCAN as the user understands it: one file for
the computer with every part of the device that may leave it, the copies the
device keeps for itself (certificates included, sealed with a key the device
made once) on the internal flash and the SD card, and a restore that shows
what a backup holds before anything changes. Service layer: it reads the
settings export and the files of `/data` by path; autopid, cert_manager and
script_engine are not dependencies.

Three cards on System > Maintenance (`web_ui_v2/web/backup.js`) are its UI;
`HTTP_API.md` beside this file is its contract; `backup` is its console
command.

## The parts

| Part | What | On the device | In a file | In the copies |
|---|---|---|---|---|
| `settings` | every settings component's pending object with its schema version (`settings_manager_export`) | the settings partition | yes | yes |
| `vehicles` | the vehicle store, the per-car tables, the live tables, the standard-PID scan | `/data/autopid/vehicles.json`, `vehicles/<key>.json`, `config.json`, `std_scan.json` | yes | yes |
| `databases` | the trouble-code databases and DBC files the user uploaded | `/data/autopid/dtc_db`, `/data/autopid/dbc` | yes (base64) | yes |
| `scripts` | the stored Berry scripts | `/data/scripts` | yes (base64) | yes |
| `dashboard` | the dashboard layout | `/data/dashboard.json` | yes | yes |
| `certs` | the certificate sets: CA, client certificate, client key | `/data/certs/<set>` | never | yes |

Never in either: the logs on the SD card, the restart history and the fault
log, the web UI extras cache (`/data/cache/www`), the firmware.

Key material never leaves the device through the API: no `certs` part in a
file, and `filesystem_api_hidden()` makes every `/api/fs` route refuse
`/data/certs`, `/data/backups` and `/sd/backups` (404, and a parent listing
skips them). The copies restore only through the restore route, which writes
them back in place and shows nothing.

## The file

Plain (`wican-<id>-<date>.json`): `{"wican_backup":2,"device_id","fw_version",
"created","redacted","parts":{...}}`, every part the JSON value described in
`HTTP_API.md`. Remove sensitive data blanks the secret keys (the api_http
suffixes `_password`, `private_key`, `preshared_key`, `auth_key`, `_token`,
`api_key`, plus `mqtt_manager.username`, `espnetlink.apn_user`,
`ha_webhooks.url/url2`, `data_destinations` `url` and `basic_username`) and
marks the document `redacted`; on restore a blank secret keeps the stored
value (the PUT rule), so such a file never wipes a password.

Encrypted (`wican-<id>-<date>-encrypted.json`): `{"wican_backup":2,
"encrypted":true,"device_id","fw_version","created","cipher":"aes-256-gcm",
"nonce":<b64 12 bytes>,"data":<b64 ciphertext||tag>}` over the plain document,
AAD `wican_backup:2:<device_id>:<created>`. Only the device that made it, or
one given its key file, opens it.

Today's `GET /api/settings/backup` document (`wican_backup: 1`) still restores:
it is the `settings` part alone.

## The copies

`/data/backups/latest/` and `/sd/backups/wican-<id>/latest/`: `manifest.json`
(`wican_copy`, `device_id`, `fw_version`, `refreshed` epoch, a keyed HMAC and
the plain size per part) and one `<part>.enc` per part (`WCBS`, format 1, a
12-byte nonce, ciphertext||tag; AAD `wican_copy:1:<device_id>:<part>`). The
HMAC is HMAC-SHA256 over the plain part keyed with the backup key, so the
manifest leaks nothing and the sweep compares without unsealing.

The worker (`cb_copies`, a static task on an 8 KB internal stack: every
filesystem call needs one, reads included) sweeps 20 s after start, about 5 s
after `config_backup_touch()` (api_http calls it after an fs upload or
delete; touches collapse), and every 120 s otherwise while `auto_copy` is on.
A sweep builds every part, HMACs it, and seals and writes only a part whose
HMAC differs from the manifest; the manifest is rewritten when a part was.
A steady state writes nothing (standard section 11). A manual refresh
(`POST /api/backup/copies/refresh`, `backup -r`) also rewrites the manifest
so its time moves. A card that comes back is re-read; a card written by
another WiCAN (exactly one other `wican-<id>` folder) is offered for restore
and needs that device's key file.

## The key

32 bytes from `esp_fill_random()` the first time the component starts, kept
in NVS (`cfg_backup`: `key`, `created`, `dl`, `dl_at`) outside the settings
partition so a factory reset keeps it. Never logged, never in `/api/settings`;
`GET /api/backup/key` hands out `wican-<id>-backup-key.txt` (two comment
lines and 64 hex characters) and marks it downloaded. An encrypted download
waits until the key was downloaded once (409). There is no replace-key
route: one key opens every backup and copy the device ever made.

## The restore

One staged slot in PSRAM (2 MB cap, ten-minute life): an uploaded file
(`POST /api/backup/inspect`, never written to flash) or a copy
(`GET /api/backup/copies/<internal|sd>`). The header is read with the pure
member scanner (no parse of the big strings), the document unsealed when the
key is known (ours, or one given through `POST /api/backup/unlock`), indexed
by part, and every part dry-run: settings through
`settings_manager_restore(dry_run)` per component (unknown components are
skipped, a newer one refuses the document), files by name, size, base64 and
JSON validity. The manifest answers with facts, warnings (`key_missing`,
`bad_key`, `other_device`, `redacted`, `newer`), per-part counts and errors.
`POST /api/backup/restore` applies the ticked parts, each as a whole (the
files of a part are wiped, then written atomically), with `keep_identity`
(this device's `wifi_manager.ap_ssid` and `hostname` stay, default true) and
`keep_vpn_keys` (this device's `vpn_manager` object stays, default false),
then restarts through `restart_tracker` (CONFIG_APPLY, WEB_UI). Nothing is
reloaded live: the restart loads the new files like any boot.

## API (`include/config_backup.h`)

| Call | Behavior |
|---|---|
| `config_backup_init()` | log descriptor + the `config_backup` settings descriptor |
| `config_backup_start()` | the key (made once), the copies worker |
| `config_backup_stop()` | stops the worker, drops the staged restore |
| `config_backup_register_http()` | the `/api/backup` routes (main wires it before the server starts) |
| `config_backup_touch()` | a sweep soon (api_http after an fs upload or delete); never blocks |

## Dependencies

`settings_manager`, `filesystem`, `dev_status_manager` (identity, clock, the
SD bit), `http_server_manager`, `cmdline_manager`, `restart_tracker`,
`log_manager`, `nvs_flash`, `esp_hw_support` (the random source), `mbedtls`
(the PSA API: AES-GCM, HMAC; base64), `fatfs` (the card's free space),
`esp_http_server`, `console`. cJSON through the managed `espressif/cjson`.
Init after settings_manager and filesystem; start after the filesystem, the
card and the components whose files it mirrors (main.c: after data_logger).
api_http depends on this component for the touch.

## Settings (`config_backup`, v1)

| Key | Default | Meaning |
|---|---|---|
| `auto_copy` | true | keep the copies up to date (the sweeps); off = manual refresh only |
| `cli` | true | register the `backup` console command |

## Memory footprint (estimated, 2026-10-11)

- Internal RAM: the worker's 8 KB stack, two static FreeRTOS objects, the
  32-byte key, about 0.5 KB of state.
- PSRAM: the two manifests (about 0.5 KB), the staged restore slot header;
  during a sweep one part at a time (the plain part, its sealed twin: twice
  the largest part, about 400 KB with a 180 KB database set), during a
  download the whole document and, encrypted, its ciphertext (two documents
  of up to 2 MB), during an inspect the upload and its unsealed twin.
- Flash: event-driven writes only (a key once per device, the parts and the
  manifest of a copy when a part changed, a restore). A steady-state boot
  writes nothing: the manifest is read, the parts hashed, nothing differs.

## Files

| File | Role |
|---|---|
| `config_backup.c` | lifecycle, the touch, the clock and identity helpers, the planned restart after a restore |
| `config_backup_core.c` | PURE: the part names, the top-level JSON member scanner (ranges, no parse of the big strings), the name checks, the ISO time |
| `config_backup_codec.c` | PURE: the redaction rule and its inverse, the key-file codec, the manifest codec |
| `config_backup_crypto.c` | the key in NVS, AES-256-GCM and HMAC through the PSA API, base64 |
| `config_backup_files.c` | the growable PSRAM buffer, files into PSRAM, a directory into a list, JSON files verbatim, files as base64, a directory wipe |
| `config_backup_doc.c` | the six part builders, the whole document, the counts of the status route |
| `config_backup_copies.c` | the copies worker and its sweep, the manifests, the folders, a copy loaded for a restore |
| `config_backup_restore.c` | the staged slot, the header, the unseal, the index, the dry run, the manifest, the apply |
| `config_backup_apply.c` | one part at a time: the dry run and the apply with the keep rules |
| `config_backup_http.c` | the `/api/backup` routes |
| `config_backup_cli.c` | `backup [-r]` |
| `config_backup_settings.c` | `{auto_copy, cli}` |

## Testing

- Host: `host_test/` (21 tests: the scanner, the redaction rule and its
  inverse, the names, the key file, the manifest, a document indexed, a
  version-1 document, a 64 KB string skipped). `filesystem/host_test` covers
  `filesystem_api_hidden`.
- Mock: `tools/webui_preview/probe_backup.mjs` (`BACKUP PROBE PASS`, 44
  checks, plain and `--min`).
- Hardware: `tools/testbench/system/config_backup_bench.py`
  (`CONFIG BACKUP PASS`; the row in TESTING.md).
