# cherryusb: vendored component

Vendored 2026-07-07 from **meatpi's fork**
https://github.com/meatpiHQ/CherryUSB branch `dynamic-shared-buffer`
(local clone's `sharred-buffer`, same commit
`bf7af0f6b044b4f6be99fd6c20e3e1a258fbd282`: "Fix ASIX RX buffer
overflow and frame parsing"). Upstream base: CherryUSB 1.5.2.

The fork's key change (NOT present in the upstream
`cherry-embedded__cherryusb 1.5.2~2` managed component the legacy
build used): `common/usbh_eth_shared_buf.{c,h}`, all five USB-eth
class drivers (CDC-ECM/NCM, RNDIS, ASIX, RTL8152) alias their RX/TX/
INT/CTRL buffers onto ONE shared DMA pool sized to the largest enabled
driver, instead of five private static sets. Only one eth class runs
at a time on this hardware.

Local changes on top of the fork (this repo):
- Trimmed: demo/, docs/, zephyr/, tools/, third_party/ (MSC-fatfs
  template only), port/* except port/dwc2 (ESP32-S3), README/images,
  idf_component.yml (vendored, not managed).
- `usbh_eth_shared_buf` made **heap-backed** (internal-RAM budget,
  2026-07-07): the pool is allocated from INTERNAL|DMA heap by
  `usbh_eth_shared_buf_alloc()` when an eth class starts and freed on
  stop, so the RAM is only consumed while a USB-eth adapter is
  actually attached. See the file header for the contract.

- OSAL thread stacks → **PSRAM** (2026-07-07 pm, `osal/idf/
  usb_osal_idf.c`): every CherryUSB thread (usbh hub/psc 4 KB + the
  active eth-class RX 2 KB) now uses `xTaskCreatePinnedToCoreWithCaps`
  with SPIRAM stack caps (TCB stays internal via pvPortMalloc);
  `usb_osal_thread_delete` routes through `vTaskDeleteWithCaps`, which
  supports CherryUSB's self-delete pattern (delete(NULL)) via IDF's
  cleanup-task mechanism. Safe because the threads never touch flash
  and URBs/class state live in internal .bss (the IRAM DWC2 ISR never
  reads a PSRAM stack). Measured: +5.9 KB internal free / +5.6 KB
  largest block at full bench density; `USB ETH TARGET PASS` +
  `TS TARGET PASS` re-verified after. NOT moved (deliberately):
  `g_usbhost_bus` (~4 KB .bss, the ISR walks pipe/URB state in it;
  PSRAM would fault during flash-cache-off windows), `ep0_request_
  buffer`/`g_setup_buffer` (DMA), the ~10 KB IRAM ISR code (cache-off
  safety).

Consumers: `usb_eth_host` (+ `usb_acm_cli` later). The CherryUSB
platform glue `platform/idf/usbh_net.c` must stay EXCLUDED from the
build (usb_eth_host's CMake marks it HEADER_FILE_ONLY): its strong
usbh_*_run/stop/eth_input symbols would bypass our netif glue.

- **DEVICE stack enabled for role=device (2026-07-07 pm, `CMakeLists.txt`
  ESP branch):** added `CONFIG_CHERRYUSB_DEVICE 1` +
  `CONFIG_CHERRYUSB_DEVICE_CDC_RNDIS 1` + `CONFIG_CHERRYUSB_DEVICE_CDC_ACM 1`
  so the device core (`core/usbd_core.c`) + RNDIS (`class/wireless/
  usbd_rndis.c`) + CDC-ACM device classes COMPILE. This is for the
  J2534 USB transport (usb_host_manager `role=device` → WiCAN is a
  USB-Ethernet device to a PC, TASK_j2534_server.md §5). Host + device
  stacks BOTH compile; only one runs at boot (the S3 OTG is host XOR
  device, role-gated; device init is never called in role=host).
  NCM device is NOT vendored (no `class/cdc/usbd_cdc_ncm.c` despite the
  cherryusb.cmake stub) so RNDIS (Windows-native) is the USB-Ethernet
  device class. BUILD-PROVEN + no host-mode regression (device-enabled
  firmware boots clean in role=host, WiFi + `J2534 REFLASH/DRIVER PASS`
  re-verified, 21 KB internal free). The device→esp_netif bridge
  component (usb_net_device) is the remaining Phase-3 bring-up.

- **`class/cdc/usbd_cdc_ncm.{c,h}`: WICAN-AUTHORED (2026-07-08), not
  upstream:** CherryUSB has NO device-side NCM class at any version
  (only `usbh_cdc_ncm`); the cherryusb.cmake `CONFIG_CHERRYUSB_DEVICE_
  CDC_NCM` stub existed with no file behind it. We wrote the class
  (NTB16-only, one NDP/NTB on TX, spec-conformant RX parse) because
  **Windows 11 24H2 removed the legacy RNDIS driver**: CDC-NCM is what
  the inbox `usbncm.inf` (UsbNcm.sys) binds. Differences vs the unused
  `CDC_NCM_DESCRIPTOR_INIT` template in `usb_cdc.h` (kept as-is): our
  `CDC_NCM_ALT_DESCRIPTOR_INIT` adds the **alt-0 (no EPs) / alt-1 data
  interface pair** (NCM 1.0 §5.3, Windows requires it; link-up hangs
  off SET_INTERFACE alt 1) and sets data-interface protocol 0x01 (NTB).
  Style/API mirrors `usbd_cdc_ecm.c` (weak recv/send-done + link_event
  hooks; ESP port = USB-ISR context). Enabled in the CMakeLists ESP
  branch. Consumer: `usb_net_device` (NCM → esp_netif + DHCP server).
  BENCH-PASSED 2026-07-08: enumerates + starts on Windows 11 26200
  first try, DHCP lease over USB, `J2534 DRIVER TEST PASS` end-to-end
  through the cable. Sync note: if upstream ever ships a device NCM
  class, ours will collide on file name, diff before adopting theirs.

- **`class/cdc/usbh_cdc_ncm.c`: WICAN FIX (2026-07-13), diverges from
  upstream:** `usbh_cdc_ncm_eth_output()` now submits a ZLP after any
  NTB whose `wBlockLength` is an exact multiple of the bulk-out MPS
  (NCM 1.0 §3.2.2). Upstream never terminates exact-multiple transfers,
  so the device's OUT read never completes and the frame is silently
  swallowed: found live as "DHCP over NCM host mode never leases"
  (DISCOVER = 384 B = 6×64 FS packets exactly) with the ESPNetLink NCM
  dongle, while odd-sized ARP/ICMP passed. Full report:
  `BUG_NCM_HOST_TX_ZLP.md`. Candidate for upstreaming. When syncing a
  newer upstream, re-check this function.

- **`class/cdc/usbh_cdc_ecm.c`: WICAN FIX (2026-07-26), diverges from
  upstream:** same bug class, found by the planned post-NCM audit,
  `usbh_cdc_ecm_eth_output()` had NO exact-multiple handling at all
  (the driver applies the rule on RX only, :300). ECM has no length
  framing (the transfer boundary IS the frame delimiter) so an
  ethernet frame of exactly N×MPS (64/128/…/1536 B on the FS-only
  S3 port) was silently swallowed by the device. Fix mirrors the NCM
  ZLP. Not yet exercised live (needs a CDC-ECM adapter/tether on the
  host port); audit + fix reviewed against the NCM report. Candidate
  for upstreaming.

- **`class/vendor/net/usbh_asix.c`: WICAN FIX (2026-07-26), diverges
  from upstream:** operator-precedence bug in the TX padding guard,
  `if (!(buflen + 4) % MPS)` parses as `(!(buflen+4)) % MPS` == 0,
  i.e. always FALSE, so the 4-byte `00 00 ff ff` terminator for
  exact-multiple transfers (the ASIX equivalent of the ZLP rule) was
  dead code. Fixed to `((buflen + 4) % MPS) == 0`. Same silent-drop
  class as the NCM bug, on the adapter wican ships support for. Found
  in the same audit; not yet exercised live at an exact-multiple
  length. Candidate for upstreaming. (usbh_rndis audited CLEAN: it
  uses the +1-pad-byte variant, valid because RNDIS frames carry their
  own MessageLength; device-side usbd_cdc_ecm/ncm/rndis all ZLP
  correctly.)

- `class/hub/usbh_hub.c` `usbh_hub_deinitialize()` (2026-10-01): the
  `usb_osal_enter_critical_section()` around `usb_hc_deinit()` is gone.
  On this port that is `taskENTER_CRITICAL()` (interrupts off) and
  `usb_hc_deinit()` blocks inside it (`usb_osal_msleep(200)`, then
  `esp_intr_free()` in `usb_hc_low_level_deinit()` which waits on a
  cross-core IPC semaphore); blocking with interrupts masked corrupted
  the FreeRTOS task lists and every `usbh_deinitialize()` ended in an
  "Interrupt wdt timeout on CPU1" panic (hit on the sleep bench: the
  sleep prepare sequence stops the USB host; a physical unplug takes the
  same path). `usb_hc_deinit()` masks the controller's global interrupt
  as its first instruction, so nothing races it. Reproduce with the
  bench CLI `sleep test 20` with a device on the connector.

- `core/usbh_core.c` / `core/usbh_core.h` (2026-10-07): two application
  hooks for the device itself, whatever its class:
  `usbh_device_mount_done_callback(hport, product)` at the end of
  `usbh_enumerate()` (after the class drivers were loaded, driver or no
  driver) and `usbh_device_unmount_done_callback(hport)` at the start of
  `usbh_hubport_release()`. Weak no-ops in the core; `usb_eth_host`
  (`usb_eth_host_device.c`) overrides them to record idVendor, idProduct,
  the device and first-interface classes and the product string, which
  `/api/usb` reports as `device`. Needed because the class-driver hooks
  only fire for interfaces a driver binds: a u-blox GNSS receiver bound as
  CDC-ACM or a memory stick nobody binds were invisible to the status
  surfaces, and Quick Setup's USB step names what is on the connector.
  The product string is "" on this port (`CONFIG_USBHOST_GET_STRING_DESC`
  stays off: in this core a failed string read fails the enumeration).


- `class/wireless/usbh_rndis.c`, `class/cdc/usbh_cdc_ncm.c`,
  `class/cdc/usbh_cdc_ecm.c`, `class/vendor/net/usbh_asix.c` (2026-10-12,
  WICAN FIX, diverges from upstream): the class rx thread runs above the
  hub thread's priority (`CONFIG_USBHOST_PSC_PRIO + 1`). When the
  disconnect kills its bulk-in urb it loops back to `find_class` and polls
  the connect status with a control transfer on the port, while
  `usbh_hubport_release()` deletes that port's mutex right after the class
  disconnect returns; the thread's `usb_osal_mutex_give()` then ran on a
  deleted mutex (`assert failed: spinlock_acquire ... lock->count == 0`,
  task `usbh_rndis_rx`, the ESPNetLink VBUS-cycled by
  `POST /api/espnetlink/repair` while the wire carried the VPN and HTTP
  POSTs: the garage bench's g6 leg, the crash note decoded by
  `tools/crash_decode.py`). Fix in each driver: a `disconnecting` flag
  raised at the start of the disconnect, the thread leaves at `find_class`
  when the port is no longer connected or the flag is up
  (`usbh_<class>_rx_should_stop()`), and the disconnect waits for the
  thread to exit (`usbh_<class>_wait_rx_thread()`, 10 ms polls, 3 s bound,
  longer than `CONFIG_USBHOST_CONTROL_TRANSFER_TIMEOUT` = 500 ms) before
  the class stop, the memset and the hub's mutex delete. rtl8152 shares the
  template but is compiled out. Candidate for upstreaming. Proof: the
  repair hammer of the garage bench (TESTING.md).

- `class/wireless/usbd_rndis.c` (2026-10-17, WICAN FIX, from the ESPNetLink
  bench of 2026-10-16, diverges from upstream): `rndis_bulk_out` returns when
  `g_rndis_rx_data_buffer` is NULL instead of reading the packet header
  through it. A warm reboot with the host's RNDIS link up leaves a transfer
  in flight; the next image took its OUT completion in ISR context about 5 s
  after boot, before the bridge had armed a read (LoadProhibited at 0,
  coredump-verified on the dongle). This port has no class-owned receive
  buffer (`CONFIG_USBDEV_RNDIS_USING_LWIP` is off; `usb_net_device` arms the
  read once configured), so the completion is dropped rather than re-armed
  as on the dongle. Candidate for upstreaming in the LWIP form.

- `port/dwc2/usb_dc_dwc2.c` + `common/usb_dc.h` (2026-10-17, WICAN
  ADDITION, same origin): `usb_dc_soft_disconnect(busid)` sets DCTL.SDIS
  (D+ pull-up off) and nothing else: the registers stay clocked.
  `usb_host_manager` calls it from an `esp_restart` shutdown handler in the
  device role, so a warm reboot looks like an unplug to the PC and the next
  image enumerates with no transfer in flight. `usb_dc_deinit()` was
  deliberately not used there: it frees the PHY and gates the controller
  clock while other tasks may still touch the stack in the restart window
  (a register access on a gated bus hung the dongle until a power cycle).
  The bit survives a soft reset: a restart into ROM download mode must not
  call it (no such path on WiCAN). Proof: `tools/testbench/usb/
  usb_detach_bench.py` (`.\test.ps1 usbdetach`).
