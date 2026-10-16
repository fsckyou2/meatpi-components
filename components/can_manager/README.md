# can_manager

Owner of the native CAN (TWAI) bus: WiCAN Pro TX=GPIO2 / RX=GPIO1 /
STDBY=GPIO38 (Kconfig `WICAN_CAN_*`). Wraps ONE shared `can_core`
handle (adopted code, see its PROVENANCE.md); every consumer
registers into it: add-on AT engines, autopid's `elm327` backend,
slcan/gvret/internal-CAN endpoints. Design +
status: `TASK_can_manager.md`.

## Settings (`can_manager`, v2, reboot-to-apply)

| key | type | default | |
|---|---|---|---|
| `enabled` | bool | false | opt-in |
| `baud` | enum | "500" | `auto`, or 33/83/95/100/125/250/500/1000 kbit/s (33 = 33.333, 83 = 83.333, 95 = 95.238) |
| `silent` | bool | false | bus-wide listen-only (no ACK, no TX pin) |
| `cli` | bool | true | the `can` console command |

v1 documents migrate unchanged (their `baud` values are all still valid).

## Listen before talk (2026-10-03)

A CAN node at the wrong bitrate destroys the traffic of the bus it sits on:
every frame it cannot read gets an error flag, the sender retransmits until
it is bus-off. On this chip that is true in listen-only mode too
(`TWAI_LL_HAS_LOM_DOM_ISSUE`; bench: 445 error frames a second on a 250k bus
with this node "listening" at 500k). So:

- The node ALWAYS starts listen-only, and listen-only means the TX signal is
  not routed to the pad at all (`can_core_driver.c`).
- Two frames read at the bitrate in use prove it; a node the settings want
  talking is promoted to normal mode then.
- Eight receive errors without a frame are a mismatch: `auto` tries the next
  candidate (500, 250, 125, 1000, 100, 83, 95, 33; a rest of 2 s after a
  round in which none read the bus), a fixed bitrate stays where it is,
  listen-only, and says so (`state: mismatch`).
- A silent bus has no bitrate. A FIXED bitrate goes to its configured mode
  after 300 ms of silence, as the node always did (a gatewayed OBD port only
  answers when asked); `auto` keeps listening.
- `auto` on a silent bus takes a bitrate a tester states (2026-10-17,
  `can_manager_adopt_baud()`: a J2534 CONNECT's baud, python-can's `S6` on an
  slcan connection): the node runs at it in the configured mode, `adopted`
  and unverified, until frames prove it or the watchdog demotes it; refused
  when the bus is not silent, was bitten, or the rate is not a candidate. A
  fixed bitrate is never overruled. Until something states it every transmit
  is refused, and the first refusal of an episode is logged
  (`W can_core: transmit refused: the node may not talk, ...`): a field
  report had read the silent refusal as "J2534 connected, no comm to the
  CAN bus".
- A node that talks and meets traffic it cannot read is demoted at once (the
  error interrupt wakes the RX task every 8 errors; measured cost: 5 to 6
  destroyed frames). After that, silence no longer promotes it: only frames.
- While it only listens, a mismatch needs the evidence twice, 10 ms apart:
  a listening controller is error-passive and turns ONE corrupted frame into
  a cascade of 41 to 48 error interrupts.
- A listening controller can be deaf to a saturated bus at a higher bitrate
  (no frame, no error: it never sees the idle time it waits for). The link
  glue therefore looks at the RX line itself when the controller has nothing
  to say; "busy" counts as mismatch evidence (`rx_deaf`).
- Transmission is gated: `can_manager_send()` returns `ESP_ERR_INVALID_STATE`
  while the node may not talk (`tx_refused`), `can_manager_tx_ready()` asks
  first.

The policy is pure (`can_autobaud_core.c`, host-tested); `can_core_link.c`
applies it from the RX task.

A software reset does not reset the CAN controller. A shutdown handler takes
it off the bus on every planned restart, and `can_manager_init()` puts the
transceiver into standby first thing (after a panic the controller would
otherwise keep acknowledging, with nobody behind it, until the bus comes up
again).

## Bit timing (2026-10-03)

One shape for every bitrate: 20 time quanta, sample point 80 %, SJW 3
(`can_timing_core.c`, host-tested), the released firmware's table. Left to
itself `esp_driver_twai` aims at 87.5 % below 500 kbit/s; at 250 kbit/s the
node then mis-read about one frame in 1500 on the bench (stuff / form
errors; in normal mode each is an error flag on the bus). The helper also
makes the exact 33.333 / 83.333 / 95.238 kbit/s the rounded setting values
stand for.

## Surfaces

- `GET /api/can`: status + stats (HTTP_API §6e10): `state` (`detecting` /
  `listening` / `mismatch` until the link has its verdict, then `running` /
  `bus_off` / `recovering`; `stopped` while the bus is down), `listen_only`,
  `verified`, `baud_auto`, `baud_detected`, the bus-off/recovery counters,
  the frame-loss localizers `rx_overrun` (the controller's own FIFO
  overran = the receive interrupt was served too late), `rx_missed` (TWAI
  RX queue overflow = the can_core RX task starved) + `dispatch_drops`
  (drop-oldest in a subscriber queue = that consumer's drain starved),
  `rx_bad` / `rx_deaf` /
  `tx_refused`, `err{stuff,form,bit,ack,other}`, `probe{}`, and
  `subscribers[]` (`idx`, `name`, `drops`) of `subscribers_max`.
- `can` CLI (`can -z` zeroes counters; `can send <id> [hexbytes] [-r]`
  transmits one frame: 3 hex digits = 11-bit id, more = 29-bit).
- C API: `can_manager_core_handle()` (the shared handle),
  `can_manager_send()`, `can_manager_tx_ready()`,
  `can_manager_subscribe_queue()` (drop-oldest frame fan-out, 16 slots),
  `can_manager_subscriber_name()` / `_get()` (who reads the bus and what
  each one's queue lost), `can_manager_capacity()` (boot health report:
  `WICAN CAPS can_subs`), `can_manager_status()`.
- What is on the bus? `can_manager_probe()` listens for at most 400 ms and
  answers `silent`, `live` at 500 or 250 kbit/s, or `unreadable`; it works
  while can_manager is disabled (a temporary listen-only node without a TX
  pin). `can_manager_watch(true)` holds that listener so a caller can ask
  before every transmission (autopid's bus guard does, until the bitrate is
  proven).

## What is on the bus: probe, watch, id sample (`can_manager_probe.c`)

Split out of `can_manager.c` 2026-10-03 (700-line rule; the lifecycle file
keeps the one handle and the life lock, `can_manager_private.h` has the
accessors). `can_manager_probe()` and `can_manager_watch()` are unchanged (a
listen-only node of their own while the bus is disabled by settings).
`can_manager_sample_ids(ms, out, max, &n, &frames)` listens for `ms` and
returns the distinct identifiers heard (id, 29-bit, DLC, count): a running
bus or a held watch is read as it is, otherwise a node of its own goes up
and the window opens once its link is verified (the bitrate found by
listening, up to 4 s; a silent bus returns nothing). Never transmits. The
caller is autopid's detection job, which must tell a J1939 network (well-known
groups on 29-bit ids) from an OBD port before the native bus is enabled. One
`I` line per sample (`sample: own node, 174 frames, 17 ids in 1024 ms, link
running (verified) at 250 kbit/s`).

## Transmit: owned frames and a bounded retry (2026-10-03, phase 6)

The IDF node driver keeps the POINTER of the frame a transmit hands it
(`esp_twai_onchip.c`, `_node_queue_tx`: `p_curr_tx = frame`, or a queue of
pointers) and formats it into the hardware when the hardware comes free.
`can_core_transmit()` used to build the frame on its own stack: a
use-after-return whenever the hardware was busy, harmless on the quiet bench,
not for the bursts J1939 active mode sends (CTS, end-of-message, requests).
`can_drv_transmit()` now copies the frame into one of `CAN_DRV_TX_SLOTS`
(16) slots in internal RAM that stay busy until the frame's tx_done
interrupt; no slot free = `ELM327_ERR_BUSY`. The controller is single shot
(`fail_retry_cnt 0`, see the comment there: retry-for-ever parks a node
error-passive on a bus nobody acknowledges), so the tx_done callback
re-queues a frame that was lost from the interrupt: the bounded software
retry phase 0 found owing. Two budgets, because the two ways a frame fails
are not alike: a LOST ARBITRATION (a higher-priority frame started at the
same time; normal on a busy bus, no error, the error counters stay put) is
retried up to `CAN_DRV_TX_RETRIES_ARB` (16) times, as a controller with
automatic retransmission keeps trying; an ERROR (no acknowledge: nobody on
the bus, or a bit read back wrong) only `CAN_DRV_TX_RETRIES` (3) times,
because each such attempt costs the node 8 transmit-error-counter points.
The two are told apart by the HAL's arb_lost error event for the attempt
(`arb_lost` moved since it went out); an attempt whose event arrives after
its tx_done counts as an error, the safe side. One budget of 3 for both lost
2 of 20 J1939 requests under a 1000 frame/s flood of priority-0 frames
(bench 2026-10-03, the exposure the J1939 active bench keeps).
`CONFIG_TWAI_IO_FUNC_IN_IRAM=y` puts the driver's transmit entry in IRAM for
the retry (sdkconfig.defaults). Counters `tx_done`, `tx_retries`, `tx_lost`
on `/api/can` and `can`.

## Receive-error storms and the interrupt watchdog (`rx_storms`, 2026-10-03)

A controller listening at a bitrate CLOSE to the bus's (83.3 against 95.2
kbit/s, 95.2 against 100: the candidate walk of `auto` passes through such
pairs) sees a bit error every few bit times and raises a bus-error interrupt
for each: tens of thousands a second (`rx_bad` +134 989 in one 4 s leg of
the autobaud bench). The level-1 interrupt dispatcher serves the source that
re-asserts fastest first, so the tick starves and the interrupt watchdog
resets the chip. Caught on the console 2026-10-03 16:31:00, the autobaud
bench's `n11` leg walking from 83 to 95 kbit/s: `Guru Meditation Error:
Core 0 panic'ed (Interrupt wdt timeout on CPU0)`, both cores idle at task
level, `_xt_lowint1` busy for 300 ms; two such resets in five runs before
(the one of the phase 5 regression that restart_tracker had filed as
"deepsleep"). Nothing at task level can help, the task never runs. So the
error ISR counts errors per 20 ms window and past 400 (20 000/s; a plain
wrong bitrate on a bus at line rate makes about 1800/s) MASKS the
controller's interrupts with the HAL's register access
(`twai_ll_set_enabled_intrs(&TWAI, 0)`: the register, not the driver, which
has no such call) and wakes the RX task. The node is deaf from then on,
which the link policy already handles: the RX line look
(`link_look_at_the_line`) reads a busy line with a controller that says
nothing as mismatch evidence and bounces the node, and every bounce starts a
new node with its interrupts on. `rx_storms` on `/api/can` and `can` counts
the storms; one `W` line per storm.

## The node handle has a lock (`can_core_node.c`, 2026-10-05)

The link policy bounces the TWAI node on the RX task (delete + create)
whenever it changes the bitrate candidate or the mode, and
`can_core_get_stats()` reads the node's error counters from any task: the
bus guard's probe every 10 ms while a listener finds a bus's bitrate, `GET
/api/can`, the `can` command. `twai_node_delete()` lets the HAL go (its
register pointer becomes NULL) and frees the node before our handle is
cleared, so a status read in that window loaded through NULL + 0x3c, the TEC
register: `Guru Meditation Error: Core 1 panic'ed (LoadProhibited)`,
`twai_ll_get_tec` <- `twai_node_get_info` <- `can_core_get_stats`. Caught
with the console on 2026-10-05 02:06:56, 0.27 s after a detection's end on a
250 kbit/s bus (there every listener starts at 500 and is bounced to 250
while the probe polls: roughly one panic in a hundred looks; on a 500 kbit/s
bus there is no bounce and it never happened), and on demand a few minutes
later through `GET /api/can` asked from four threads while the autobaud
bench's `n11` / `n12` legs bounced the node (one panic in about 150 s). The
reset nobody could explain in the EU truck bench's first run (2026-10-03
16:57) sat at the same spot.

`can_core_node.c` holds a mutex around every create and delete of the handle
(`can_drv_start_on_core()`, `can_drv_stop()`, moved there from
`can_core_driver.c`) and `can_drv_node_info()`, THE way to read the node's
status from a task that is not the one bouncing it (it may wait a few
milliseconds for a bounce; false while the node is down). `can_drv_quiesce()`
(the restart path, which may not block) gives a bounce in flight 50 ms.
Transmits have their own gate (`tx_open` / `tx_users`, `can_core_link.c`);
the recovery calls run on the task that bounces. A new reader of node state
from another task goes through `can_drv_node_info()` or takes the same lock.
Bench: leg `n13` of `can_autobaud_bench.py` (the bouncing node of `n12`,
its status asked for from four threads for a minute: no reset).

## Frames the controller loses (`rx_overrun`, 2026-10-03)

The controller's receive FIFO is 64 BYTES: four to five 29-bit frames. When
its interrupt is served later than that (2.5 ms at 250 kbit/s line rate,
1.2 ms at 500), the frames that arrive meanwhile are kept as "overrun" slots
without data. The IDF node driver (v6.0.2, `esp_twai_onchip.c`) releases such
a slot and moves on: no callback, no counter. Measured before this counter
existed (TASK_j1939_wwh.md, M4): 1 or 2 frames of 20000 gone at 250 kbit/s
line rate with every counter at zero.

`can_core_driver.c` counts them by wrapping the HAL read the driver's
interrupt calls (`-Wl,--wrap=twai_hal_read_rx_fifo` in `CMakeLists.txt`,
`__wrap_twai_hal_read_rx_fifo` in IRAM, the counter in internal RAM like
every other interrupt counter). A false from the real function is one lost
frame. If a later IDF renames the function the link fails on
`__real_twai_hal_read_rx_fifo`, on purpose: the alternative is to lose the
counter without noticing. A receive the driver refuses to hand over in its
callback is counted in `rx_missed`.

So a frame that was on the wire is in exactly one place: `rx`, or one of
`rx_overrun`, `rx_missed`, and per subscriber `drops`. The J1939
conservation bench holds the listener to that
(`tools/testbench/can/j1939_conservation_test.py` in the firmware repo).

## Bus-off recovery (2026-07-10)

A transmit-error cascade (shorted bus, baud-mismatch collisions) drives
TEC to 256 and the TWAI controller goes BUS-OFF: before this it stayed
down until reboot. Now `can_core`'s RX task services TWAI alerts:
`twai_initiate_recovery()` immediately on BUS_OFF (hardware waits for
128×11 recessive bits, i.e. completes only once the bus is sane), then
an automatic `twai_start()` after a policy backoff: immediate for an
isolated fault, 1→2→4…30 s cap when re-offending within 60 s (pure
policy in `can_core_recovery.c`, host-tested). Episodes are counted
in `/api/can` (`bus_off`/`recoveries`) and `can`; the web UI Status
tile flags `BUS-OFF · auto-recovering`. Fault-injection bench:
`tools/testbench/can_recovery_bench.py` (PCAN at a mismatched baud
blasting while the DUT transmits → real bus-off → verifies both
counters and traffic after recovery).

## Tests

- Host: `.\test.ps1 host can_manager` (44 tests: filters, recovery policy,
  the listen-before-talk policy, the bit timing).
- Bench: `tools/testbench/can/can_autobaud_bench.py` → `CAN AUTOBAUD PASS`
  (`.\test.ps1 autobaud`). The PCAN adapter keeps a bus alive and is the
  judge: zero error frames on the wire at the right bitrate, at the wrong
  one, in auto, across restarts; the bitrate walk over every setting value;
  autopid's bus guard on top (the OBD chip is never pinned to a protocol the
  bus contradicts); the node's status read from four threads while the link
  policy bounces it (`n13`).
- `tools/testbench/can/can_conservation_test.py` → `CAN CONSERVATION PASS`
  (exactly-N at three load tiers).

## Notes

- stop() parks TWAI and drives the transceiver into standby; main's
  sleep-prepare calls it (after ext_manager_stop, before storage).
- The ESP TWAI transceiver and the MIC3624 are two nodes on the SAME
  vehicle CAN: legal; traffic policy between autopid-via-MIC and
  native-CAN consumers is the user's call in v1.
- Memory: the can_core RX task (internal 4 KB) exists only while
  `enabled` (or while a probe / watch listens); statics negligible.
- The TWAI interrupt is cache-safe: its callbacks must not touch the handle
  (PSRAM). The ISR counts into internal RAM (`can_core_driver.c`).
- Single shot: a frame that loses arbitration or meets an error frame is
  dropped (`fail_retry_cnt = 0`; every value but -1 is single shot on this
  controller). A bounded software retry is owed before the node talks on a
  busy bus (TASK_j1939_wwh.md, phase 6).
