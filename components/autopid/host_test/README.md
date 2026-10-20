# autopid: host unit tests

Pure modules only (`autopid_sched.c`, `autopid_resp.c`, the parse half
of `autopid_config.c`, `autopid_std.c`, the codecs,
`autopid_vehicle_core.c` with its own `test_vehicle.c` and the vehicle
store's `autopid_vehicle_index.c` + `autopid_vehicle_codec.c` with
`test_vehicle_index.c`, compiled with `AUTOPID_HOST_TEST`: no chip, no
filesystem). Run via `.\test.ps1 host autopid`.

## What is covered (133 tests, 2026-10-20)

| Case | What it proves |
|---|---|
| **one request per PID per period** | THE legacy fix #1 regression: a 5-parameter PID over a simulated 10 s produces ~10 requests at 1 Hz, not 50, parameters aren't schedulable units |
| group inheritance + override | PID period 0 inherits the group; per-PID period wins; a runtime group override retargets the whole group (§5b action) |
| group toggle gates entries | disabled group hides its entries from `ap_sched_next`; the ephemeral flip re-exposes them |
| type gates | `std_enabled=false` hides STD PIDs |
| high-fidelity round-robin | two period-0 PIDs strictly alternate (last_run tiebreak) |
| fail backoff | 3 consecutive fails → period ×4; one success restores |
| stagger | initial due times spread by 10 ms so big tables don't burst |
| headers-off single frame | `41 0C 1A F8` → 4 payload bytes (service echo INCLUDED, B0=0x41) |
| SEARCHING then data | noise lines skipped |
| headers-off ISO-TP | `014` + `0:`/`1:`/`2:` rows → concatenated, trimmed to announced length |
| headers-on single | `7E8 06 41 00 …` → PCI length honored (6 bytes) |
| lowest responder wins | 7E9 + 7E8 both answer → only 7E8 kept (legacy rule) |
| headers-on ISO-TP multiframe | FF `10 14` + CFs `21/22` reassembled to 0x14 bytes (VIN) |
| error lines | NO DATA / CAN ERROR / `?` → ESP_FAIL; only-noise/empty → ESP_ERR_NOT_FOUND |
| cross-talk guard | `ap_payload_matches_cmd`: payload must echo (service\|0x40)+identifier (010C vs 0105, UDS 22xx DIDs, mode 03, response-count hint digit); AT/ST/VT unchecked |
| ATMA filter frames | legacy header shapes (contiguous `123`/`18DAF110`, byte-split ids), id mismatch skipped, the REAL bench `<DATA ERROR` suffix (markers skipped, bytes kept), noise/header-only rejected |
| **filter stream: busy bus** | crowded monitor (many other ids + noise + pre-ATCRA buffered frames), target = ONE frame among them, fed at EVERY chunk size 1..200: chunk-boundary reassembly can't miss it |
| filter stream: duplicates | same id repeating in one window → FIRST frame wins (collector stops); a fresh window picks up the newest traffic |
| filter stream: mixed DLC | other ids with DLC 1..8 around a DLC-2 target: line length never confuses the match; captured length = the frame's REAL length |
| filter: short-DLC bounds | expression beyond the captured frame (`B6` on 2 bytes, `[B0:B3]`, bit refs) = ESP_ERR_INVALID_SIZE: parameter skipped, never cached as garbage; in-range params on the same frame still evaluate |
| filter stream: truncation | an overlong corrupt line (>160 chars) dropped WHOLE; the next line still parses |
| filter monitor bounds | `monitor_ms` outside 50..60000 rejected at config parse |
| std expression mapping | legacy bit_start/scale/offset → v6 expressions (`[B2:B3]*0.25`, `B2-40`, `B3*0.78125-100`); placeholders refused |
| **std whole-table sweep** | every non-placeholder row of the vendored SAE table generates an expression the REAL parser accepts (no exponent literals, byte refs 2..8) |
| std scan-bitmap parse | `41 00 …` rows headers on/off, SEARCHING noise, multi-ECU OR-merge, wrong-PID echo/NO DATA/truncation rejected |
| config happy path | groups/pids/filters/params counted, auto `default` group, type map, group refs, is_extended |
| bad expression rejected | `[B3:B0]` fails validation; tables wiped on failure |
| unknown group + duplicate params rejected | referential integrity + cache keying |
| empty + garbage | `{}` = valid empty tables; non-object JSON = INVALID_ARG |
| **vehicle: ATDPN shapes** | `A6`/`6`/`A8\r>`/`a9`/`AA` accepted; bare `A`, `?`, `0`, `D`, garbage, trailing junk rejected (test_vehicle.c) |
| vehicle: VIN validity | 17 chars A-Z0-9 without I/O/Q; length, case and punctuation rejected |
| vehicle: VIN from 0902 | single line, SEARCHING noise, headers-off ISO-TP rows, headers-on multi-frame, two responders (lowest wins); `NO DATA`/`UNABLE TO CONNECT`/`?`, the padded 15-char bench transcript, non-ASCII, an excluded letter, a 0100 echo and a short reply rejected |
| vehicle: VIN from 22F190 | `62 F1 90` + 17 (single and ISO-TP rows); NRC `7F 22 31`, `NO DATA`, cross-service shapes rejected |
| vehicle: responders from 0100 | headers-on 11-bit and 29-bit ids with their bitmaps, headers-off rows OR-merged into one entry, noise = 0 |
| vehicle: fingerprint | deterministic, order independent, duplicate ids merged, 8 lowercase hex, one bit / one ECU difference changes it, empty = "", FNV-1a reference vector |
| vehicle: protocol + prelude | pinned setting wins, "0" uses the current car's char unless the fallback flag, the legacy ATTP map for 6..9 and protocol-only preludes, 29-bit classification, `ap_veh_proto_valid` |
| vehicle: first-pass import | the first-pass vehicle.json reads back (import once into the store); empty doc, garbage / array / no version / version 2 rejected, bad VIN or protocol dropped |
| vehicle: ATSP in a profile chain | `ap_init_sanitize` turns `ATSP7` / `atsp 7` / `ATM1` inside an init chain into `ATTP7` / `ATM0`; the std prelude passes untouched (the chip's base protocol is learned only through `obd_chip_protocol_save`) |
| **store: keys + names** | key = VIN, else `fp:<8 hex>`, else ""; key validity (what a route / file name may carry: no `../`, no upper-case hex); default names `"<WMI> <last 4>"` / `"Car <4 hex>"` (test_vehicle_index.c) |
| store: find | by key / VIN / fingerprint, "" never matches, duplicate key refused by add |
| store: subset rule | accessory (2 ECUs) vs ready (3 ECUs) = same car both ways; another main-ECU bitmap or a differing secondary = not; headers-off prints compare as sets; empty sets never match |
| store: match policy | the situations table: a VIN decides alone (even on another car's responders), another VIN = new car, exact fingerprint, drift found by the subset rule (`exact=false`), nothing = no match, a `fp:` car adopted when its VIN appears but never a VIN car |
| store: LRU eviction | the oldest `last_seen` goes, never the current car; tie = older `first_seen`; the 9th car evicts and `current` is re-indexed; remove shifts `current`; a lone current car cannot be evicted |
| store: touch guard | clock unset = no write; first touch stamps both; same day = no write; a day later = one write; clock backwards = no write |
| store: responder text | `7E8:BE7FB813,7E9:80000001` both ways, `*` for headers-off ids, 29-bit ids, junk tokens skipped, the cap honoured, a too-small buffer |
| store: vehicles.json round trip | every field survives (escaped name, `current` as a key, no per-entry `current` flag in the file), an empty index, a too-small buffer |
| store: full index fits | 8 cars with every string at its cap and 8 responders each fit `AP_VEH_INDEX_JSON_MAX` (prints the size) |
| store: load bounds | garbage / array / no version / version 2 / NULL rejected; no `vehicles` = empty; entries without a key dropped, duplicates dropped, keys derived or re-derived, bad VIN/protocol sanitized (`a` upcased), out-of-range numbers zeroed, the 9th+ entry dropped |
| **chain: the report (#964)** | the BMW odometer row's init `ATSH6F1;ATCEA60;ATCRA660;ATFCSH6F1;ATFCSD60300000;ATFCSM1` sets header, filter, extended address and flow control; the restore is the prelude + `ATCEA;ATFCSM0` |
| chain: published profiles | the i3 (`ATAT0` too: `ATAT1` comes back), Mini (`ATSP6;ATS0`: protocol + print), e-Ulysse chains and a uds car's `ATSH18DA58F1` classify as documented |
| chain: read like the chip | spaces and case ignored; the off / reset forms (`ATCEA`, `ATCRA`, `ATFCSM0`, `ATCAF1`, `ATH0`...) set nothing; the on forms do; resets, displays, reads, requests, junk and NULL set nothing; an overlong command is nobody's |
| chain: the restore builds | prelude alone when it covers everything; every bit at once fits `AP_RESTORE_LEN`; no prelude = the undo commands alone; a short buffer is cut and terminated |

## Expected result

```
133 Tests 0 Failures 0 Ignored
OK
```
