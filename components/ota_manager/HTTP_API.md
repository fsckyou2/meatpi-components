# ota_manager: HTTP API reference

> **Implemented (2026-07-04)** by the `api_http` glue (`api_http_ota.c`,
> streaming via the `multipart_upload` component); live-verified both ways.
> Conventions: `components/HTTP_API.md` §1.

## POST /api/ota/upload

Upload a firmware image. Two body forms, auto-detected by Content-Type:

- **`multipart/form-data`** (the HTML UI): the file part named `firmware`
  or `ota_file` (any part carrying a filename is accepted). Streamed: no
  full-body buffering.
- **anything else** = raw image body
  (`curl --data-binary @wican-fw.bin -H "Content-Type: application/octet-stream"`).

**Legacy alias `POST /upload/ota.bin`** (added 2026-07-11, same handler):
the pre-v5 route the HA integration falls back to (multipart field
`ota_file`), the device-contract v2 migration bridge so firmware and
integration can update in either order. Keep for ≥2 releases.

**Response 200** (then the device reboots ≈1 s later via
`restart_tracker_restart(OTA_APPLY, WEB_UI)`: the UI should show
"rebooting" and re-poll `/api/status` until `partition` flips):

```json
{ "ok": true, "received": 1510992, "partition": "ota_1", "reboot": true }
```

**`?sha256=<64 hex>`** (2026-10-18): the digest the image must have; the
session compares it before anything is activated. Malformed = 400
`sha256 must be 64 hex characters`.

**Errors**
| Code | Body | When |
|---|---|---|
| 400 | `{"error":"image validation failed"}` | not a valid app image (magic/digest) |
| 400 | `{"error":"short image (connection dropped?)"}` | body ended early vs Content-Length |
| 400 | `{"error":"not a WiCAN PRO image: <project name>"}` | 2026-10-18: the descriptor's project name is none of ours (an ESPNetLink image, say); refused at the first 288 bytes, nothing written |
| 400 | `{"error":"not an application image"}` / `"no application descriptor"` / `"an image for another chip (id 0x....)"` / `"short image (no application descriptor)"` | 2026-10-18: not an ESP32-S3 application image |
| 400 | `{"error":"sha256 mismatch"}` | 2026-10-18: `?sha256=` was given and the bytes differ; the slot stays scratch |
| 400 | `{"error":"flash write failed"}` / others | see `ota_manager_status().error` |
| 409 | `{"error":"update already in progress"}` | another session is receiving: a second upload, or the pull installer's (`GET /api/fw_update`); this request never aborts it |

A failed upload leaves the RUNNING firmware untouched; simply retry.

## GET /api/ota/status

Progress/diagnostics (the UI polls this from a second connection for a
progress bar during multipart uploads):

```json
{ "state": "receiving", "received": 524288, "total": 0,
  "error": "", "partition": "ota_1",
  "project_name": "wican-fw_obd_pro", "version": "6.1.0", "source": "upload" }
```

`state` ∈ `idle | receiving | ready | failed`; `total` is 0 for multipart
(size unknown); `project_name` / `version` are the image's descriptor once
its first 288 bytes are in ("" before); `source` is `upload` or `fw_update`
(the pull installer's pipe, 2026-10-18). **Measured**: a 1.44 MB image
completes in ~8.3 s over Wi-Fi (−47 dBm bench).
