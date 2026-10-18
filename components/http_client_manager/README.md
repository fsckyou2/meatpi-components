# http_client_manager: the HTTP(S) client owner (service)

Rewrite of the legacy `https_client_mgr`: the one way components do
HTTP-client work, synchronous request/response, TLS via cert_manager
sets, auth helpers, and atomic download-to-filesystem. Consumers:
autopid cloud posts, web-asset fetch-on-miss, future integrations.

## Thread-safety model (the point of the "manager")

Every request builds its own `esp_http_client` instance on the CALLER's
task (zero shared mutable state between requests) and a counting
semaphore caps concurrency at 4 so many tasks can't exhaust sockets/TLS
heap (excess callers briefly block). Any task may call any API.

**Caller contract** (each clause was earned on the bench):

- Calls are SYNCHRONOUS (seconds under bad networks): ordinary tasks
  only: never ISRs, timer callbacks, or handler contexts that must not
  block (mqtt/imu/battery callbacks included).
- `https://` runs the mbedTLS handshake on YOUR stack: budget **≥8 KB**
  (a 4 KB caller stack-overflowed and panicked, live-found).
- `download()` is stack-agnostic: ALL media writes run on filesystem's
  internal-stack async-pipe writer task, so PSRAM-stack callers are
  fine. (The first, staged implementation crashed a PSRAM-stack caller
  in the spi_flash cache-off assert: the pipelined design removed the
  hazard instead of guarding it.)

## API

| Call | Behavior |
|---|---|
| `http_client_manager_init()` | Log descriptor + the limiter. |
| `_request(req, *out)` | Full-control request: method, body, content type, auth, extra headers, TLS options, timeout, response cap. HTTP 4xx/5xx = ESP_OK with `status_code` set; transport failure = error. |
| `_get(url, *out)` / `_post(url, body, len, ct, *out)` | Conveniences. |
| `_download(url, save_path, progress_cb)` | **STREAMED, no size limit** beyond free space: the caller receives into a PSRAM buffer and feeds filesystem's ASYNC PIPE (2×32 KB slots + its internal-stack writer task), network RX and media writes overlap (meatpi's "fast writing while continuously reading"). Atomic via the temp+rename stream: a broken download never leaves a torn file. One at a time. **Measured: 8 MB → /sd in 16.2 s (505 KB/s, WiFi-bound; SD writes fully hidden), md5-identical round trip.** |
| `_stream(req, cb, ctx, *info)` | 2026-10-18 (fw_update's manifest and image): a GET whose body goes to `cb->on_data` chunk by chunk (4 KB PSRAM, no cap); `cb->on_headers(status, content_length)` may stop it before a byte (`ESP_ERR_INVALID_RESPONSE`); `info` carries the final status, the Content-Length, the bytes received, `complete` (the body ended where the server said), the hops and the `ETag`. `extra_headers` carries an `If-None-Match`; a 304 is ESP_OK with no body. |
| `_upload(req, chunk, progress, ctx, *info)` | 2026-10-19 (the firmware image fw_update pushes into the ESPNetLink dongle): a POST (or PUT) of a memory body in pieces of `chunk` bytes (0 = 8 KB) with the Content-Length announced at the open, `progress(ctx, sent, total)` after every piece (return false to stop: `info->aborted`), the answer read into `info->body` (255 bytes). No redirect. The request's timeout bounds every write and the wait for the answer (a peer that verifies the image before answering needs tens of seconds). |
| `_free(resp)` | Release the PSRAM response buffer. |
| `_stats(*out)` | requests / failures / bytes_rx. |

Responses are capped (default 16 KB, `max_response` up to 256 KB):
the legacy read unbounded bodies to heap; this one can't be OOM'd by a
fat endpoint. `http_client_response_t.truncated` (2026-10-18) says the body
went on past the cap.

## Redirects (2026-10-18)

The engine never calls `esp_http_client_perform`, so IDF's own redirect
handling was inert (`disable_auto_redirect` did nothing): a GitHub release
asset, which answers 302 with a 1.2 KB signed Location on another host,
failed. `open_and_fetch()` follows 301, 302, 303, 307 and 308 by hand for a
GET (`hc_redirect_follow`, pure, host-tested): the Location the parser
captured becomes the URL (`esp_http_client_set_redirection`), the connection
is closed and reopened (scheme, host and port may change; the TLS
configuration stays), at most three hops; a POST keeps its 3xx. The client
buffers are 2 KB each way so the hop's request line fits (`HC_BUF_RX` /
`HC_BUF_TX`). `request()`, `stream()` and `download()` share it.

## TLS (priority: `cert_set` > `ca_pem` > bundle)

`cert_set` names a cert_manager set: its CA verifies the server, and a
client cert+key pair (when present) is offered = mutual TLS, the same
model as mqtt_manager. **Live-verified 2026-07-04** against the bench
TLS endpoint (socat + bench CA on rpi001:8443): with `cert_set` →
status 200; without → correctly refused (the bundle doesn't trust the
bench CA). `skip_common_name` exists for rare devices with broken certs.

## Auth helpers (pure, host-tested 5/5 incl. RFC vectors)

`bearer_token` → `Authorization: Bearer …`; `basic_username/password` →
`Authorization: Basic base64(u:p)` (own tiny base64, RFC 4648 vectors);
`api_key`(+`api_key_header`, default `x-api-key`); plus pre-formatted
`extra_headers`. The legacy's query-string builders and custom signer
were dropped in v1: build the URL yourself; ask if a signer consumer
appears.

## Verified live (2026-07-04, driven via the MQTT bench surface)

GET → `{"status":200,"len":33,…}`; 100 KB download → `/data`,
**byte-identical md5** round trip; HTTPS cert_set both ways (above).
Bench fixtures on rpi001: `wican-http-8080` (python http.server) and
`wican-tls-8443` (socat TLS front) systemd units, files in
`~/wican-http`.

## Files

- `http_client_manager.c`: the request engine, the limiter, TLS, the
  streamed GET; its internals are lent to the transfer files through
  `http_client_manager_engine.h` (2026-10-19 split: the file had crossed
  700 lines).
- `http_client_manager_download.c`: the atomic download to the filesystem.
- `http_client_manager_upload.c`: the chunked upload with progress.
- `http_client_manager_auth.c`: PURE url/base64/auth/header helpers.

## Memory (verified against the main sdkconfig)

Nearly everything is PSRAM: response buffers and the 2×32 KB download
ping-pong buffers are explicit PSRAM; mbedTLS sessions (~35–45 KB
per TLS connection incl. the 16 KB/4 KB record buffers) go external via
`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`; esp_http_client internals follow
`CONFIG_SPIRAM_USE_MALLOC` (≥128 B → PSRAM). Internal RAM: only lwIP's
per-socket machinery (a few KB), worst case bounded by the 4-slot
limiter at ~10–15 KB transient, inside the §12b both-radios envelope.
One task of its own: the 4 KB internal-stack download writer (owns the
flash/SD writes); requests run on the CALLER's stack (see the caller
contract above).
