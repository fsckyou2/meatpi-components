# config_backup HTTP API (`/api/backup`, 2026-10-11)

The routes of the configuration backup (issue #979). Registered by
`config_backup_register_http()`; the central map is
`components/HTTP_API.md` 6e16. Every body and answer is JSON unless said
otherwise. Key material never comes back through these routes: no
certificates in a downloaded file, and the key only as the file the user
asks for.

## GET /api/backup/status

What the cards show.

```json
{ "device_id": "68ee8f5a653d", "fw_version": "v6.00p_alfa-03", "auto_copy": true,
  "key": { "created": "2026-10-11T05:24:24Z", "downloaded": false, "downloaded_at": "" },
  "parts": {
    "settings":  { "groups": 42, "bytes": 8925 },
    "vehicles":  { "cars": 1, "pids": 16, "filters": 0, "bytes": 10046 },
    "databases": { "dtc_db": 0, "dbc": 1, "bytes": 184000,
                   "left_out": [ { "name": "big.dbc", "why": "over 512 KB" } ] },
    "scripts":   { "count": 0, "bytes": 0 },
    "dashboard": { "present": false, "bytes": 0 },
    "certs":     { "sets": 0, "bytes": 0 } },
  "excluded": [ "logs on the SD card", "..." ],
  "copies": { "auto": true,
    "internal": { "present": true, "refreshed": "2026-10-11T05:24:45Z", "bytes": 19063, "parts": 6, "fw_version": "...",
                  "held": false, "held_since": "" },
    "sd": { "card": true, "present": true, "refreshed": "...", "bytes": 19063, "parts": 6, "fw_version": "...",
            "held": false, "held_since": "", "device_id": "68ee8f5a653d", "free_bytes": 15837413376 } },
  "staged": false }
```

Times are ISO-8601 UTC, `""` while the clock is unknown. `sd.device_id` is
this device's when its own copy is on the card, else the one other
`wican-<id>` folder found there (a card from another WiCAN); `sd.card`
false = no card, then `present` false.

`parts.<part>.left_out` (issue #989, 2026-10-16) lists what a backup of
that part would NOT carry, `[{"name","why"}]`, at most eight names and
then one `{"name":"","why":"and N more"}`; absent when everything goes in.
The reasons: `over 512 KB` (one file, `CB_FILE_MAX`), `more than 128 files
in its folder` (`CB_LIST_MAX`), `a name the backup cannot carry` (over 47
characters or not printable ASCII; a space or a leading dot is fine since
this date, the filesystem's own rule), `could not be read`, `not valid
JSON`. The same list rides the downloaded file (below) and the restore
preview.

`copies.<place>.held` (issue #987, 2026-10-16): the copy's `settings` part
is the one from before a factory reset and no sweep touches it;
`held_since` is the boot after that reset. The hold is set 20 s into the
first boot after a planned factory reset when the copy has a settings part
(both copies when a card is in), survives further boots, and ends when a
settings part is restored from any source or the copies are refreshed by
hand (the refresh route, `backup -r`). Until then the Internal flash row of
the UI says so and offers Restore (the undo) or the refresh (let go).

## GET /api/backup?include=settings,vehicles,databases,scripts,dashboard&redact=1&encrypt=1

The file, chunked, `Content-Disposition: attachment;
filename="wican-<id>-<date>[-encrypted].json"`, `Cache-Control: no-store`.
`include` defaults to the five file parts; `certs` is refused (400: it never
leaves the device). `redact=1` blanks the secret keys and sets
`"redacted": true`. `encrypt=1` seals the document with the device's key;
409 `download the key first` until the key file was downloaded once.

Plain document:

```json
{ "wican_backup": 2, "device_id": "...", "fw_version": "...", "created": "...", "redacted": false,
  "parts": {
    "settings":  { "<component>": { "version": 1, "data": { } } },
    "vehicles":  { "index": {...vehicles.json...}, "tables": { "<key>": {...} }, "live": {...config.json...}, "std_scan": {...} },
    "databases": { "dtc_db": [ { "name": "x.csv", "size": 145408, "b64": "..." } ], "dbc": [ ] },
    "scripts":   [ { "name": "hello.be", "size": 42, "b64": "..." } ],
    "dashboard": {...dashboard.json...} } }
```

A vehicles member is omitted when its file is absent (or not JSON);
`dashboard` is `null` without a layout. A file over 512 KB, a folder's
entries past the 128th, a name the backup cannot carry, a file that could
not be read or is not JSON: each is left out of its part and named in a
top-level member after `parts` (issue #989, 2026-10-16; absent when nothing
was left out; an older firmware restoring the file never looks at it):

```json
{ "wican_backup": 2, "...": "...", "parts": { "..." : "..." },
  "left_out": { "databases": [ { "name": "big.dbc", "why": "over 512 KB" } ],
                "scripts":   [ { "name": "f1.be", "why": "more than 128 files in its folder" }, { "name": "", "why": "and 4 more" } ] } }
```

Encrypted envelope (`redacted` rides the wrapper since 2026-10-16, issue
#986, so the preview can say so before the unlock; the sealed document's
own `redacted` decides the restore):

```json
{ "wican_backup": 2, "encrypted": true, "redacted": false, "device_id": "...", "fw_version": "...", "created": "...",
  "cipher": "aes-256-gcm", "nonce": "<base64, 12 bytes>", "data": "<base64, ciphertext then the 16-byte tag>" }
```

Errors: 400 `unknown part` / `nothing to include`, 500 `backup too large`
(the whole document over 2 MB; one big file no longer fails the download,
it is left out and listed), `could not build`, `could not seal`.

## GET /api/backup/key

`text/plain`, attachment `wican-<id>-backup-key.txt`: two `#` comment lines
(device, date) and 64 hex characters. Marks the key downloaded (NVS).
503 `no key yet` when the start failed.

## POST /api/backup/copies/refresh

Refreshes both copies now (waits up to 30 s): `{"ok":true,"written":["internal","sd"]}`.
504 while the refresh is still running, 503 when the worker is not running.
A copy `held` since a factory reset (status, above) is let go: its
settings part follows the device again. The UI asks before sending this
while a copy is held.

## GET /api/backup/copies/{internal|sd}

Stages that copy for a restore and answers the manifest (below). The SD copy
is this device's folder, else the one other device's. 409 `no SD card`,
404 `no copy there`.

## POST /api/backup/inspect?name=<file name>

The uploaded file as the raw body (any content type, up to 2 MB, staged in
PSRAM, never on flash). Answers the manifest:

```json
{ "source": "file", "name": "wican-...json", "created": "...", "device_id": "...", "fw_version": "...",
  "version": 2, "encrypted": false, "redacted": false, "locked": false, "refused": false, "other_device": false,
  "warnings": [ ], 
  "parts": { "settings": { "groups": 42, "skipped": 0, "bytes": 8925 }, "vehicles": { "cars": 1, "pids": 16, "filters": 0, "bytes": 10046 },
             "databases": { "dtc_db": 0, "dbc": 0, "bytes": 0 }, "scripts": { "count": 0, "bytes": 0 }, "dashboard": { "present": false, "bytes": 0 } },
  "errors": { } }
```

- `locked`: encrypted by another WiCAN and its key not given yet (warning
  `key_missing`); `parts` is empty until `unlock`.
- `refused`: a newer document (`newer`, `wican_backup` above 2 or a settings
  component above the firmware's), a wrong key (`bad_key`), or a part the
  dry run rejected (`errors` names it). A refused backup cannot be restored.
- `other_device`: made by another WiCAN (warning `other_device`).
- `redacted`: blank secrets take the stored values on restore. For an
  encrypted file the sealed document's own flag is read after the unseal
  (issue #986, 2026-10-16: until then only the wrapper was read, which
  carried no `redacted`, and an encrypted redacted file restored blank
  passwords or was refused on the VPN key check).
- `left_out`: the document's own list (above), passed through so the
  preview can name what the backup never carried.
- `version` 1 = today's settings-only document: the `settings` part alone.

400 `not a WiCAN backup` (no `wican_backup`), `missing body`, `backup too large`.
The slot lives ten minutes; a new inspect or copy replaces it.

## POST /api/backup/unlock

`{"key":"<the key file's text, or the 64 hex characters>"}` for the staged
backup of another device, held in RAM for that restore only. Answers the
manifest, unlocked. 400 `not a key file` / `wrong key for this backup`,
409 `nothing staged`.

## POST /api/backup/restore

`{"include":["settings","vehicles"],"keep_identity":true,"keep_vpn_keys":false}`
(`include` omitted = every part present). Applies the ticked parts of the
staged backup, each as a whole, then restarts: `{"restored":["settings","vehicles"],"reboot":true}`.
409 when nothing is staged, the backup is locked or refused; 400 with the
part's error when a write failed. `keep_identity` (this device's
`wifi_manager.ap_ssid` and `hostname` stay) applies to a backup of ANOTHER
WiCAN only: a backup of this device carries its own names, and after a
factory reset the stored ones are the defaults (issue #987: the undo brings
the access point name back). A restore that includes `settings` releases a
held copy (status, above).

## DELETE /api/backup/staged

Drops the staged backup: `{"ok":true}`.

## Settings

`/api/settings/config_backup`: `{"auto_copy": true, "cli": true}` (v1),
reboot-to-apply like every setting.

## The file browser

`/api/fs/*` answers 404 `not found` for `/data/certs`, `/data/backups` and
`/sd/backups` (and anything under them) and skips them in a parent's
listing (`filesystem_api_hidden`).
