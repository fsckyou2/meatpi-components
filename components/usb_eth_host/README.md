# usb_eth_host

CherryUSB **host** + one esp_netif for a USB-Ethernet device (service
layer, driven by `usb_host_manager`). Class glue per driver
(`usb_eth_cdc_ncm.c` → ifkey `u1`, `usb_eth_cdc_ecm.c` `u0`,
`usb_eth_rndis.c` `u2`, `usb_eth_asix.c` `u3`, `usb_eth_rtl8152.c` `u4`,
RTL8152 compiled out) overrides the WEAK `usbh_*_run/stop` hooks and
attaches the netif (`usb_eth_netif_glue.c`: DHCP client or static).
Routing policy: the USB netif becomes the default route only with
`prefer_as_default_route`; a subnet overlap with the WiFi STA suspends the
USB IP until the STA moves; the lease's DNS (or the gateway) is installed
as lwIP's resolver 0 and re-asserted by a 5 s guard.

## API

| Function | One-liner |
|---|---|
| `usb_eth_host_start(cfg)` / `_stop()` / `_is_started()` | Bring the CherryUSB host up/down (VBUS ensured ON at start: no power-cycle since 2026-08-24, the connector mux provides the DWC2 connect edge; rail off on stop). |
| `usb_eth_host_driver_is_allowed(d)`, `_driver_to_str(d)` | Runtime driver mask. |
| `usb_eth_host_get_active_driver(&d)`, `_get_active_ifkey(buf,len)` | Which class is bound and its netif key (→ `esp_netif_get_handle_from_ifkey`). |
| `usb_eth_host_get_active_device_ids(&vid,&pid)` | (2026-08-24) `idVendor`/`idProduct` of the device behind the active driver: the ESPNetLink is `303A:4007`. False when none. |
| `usb_eth_host_get_attached_device(&dev)` | (2026-10-07) the enumerated device itself, whatever its class: ids, the device and first-interface classes, the product string ("" on this port). From CherryUSB's mount / unmount hooks (`usb_eth_host_device.c`, MeatPi additions to the vendored core, see `cherryusb/PROVENANCE.md`); a u-blox receiver bound as CDC-ACM or a memory stick nobody binds had no ids before. False when nothing is enumerated. |
| `usb_eth_host_rndis_get_link(&up)` | RNDIS carrier state. |
| `usb_eth_host_get_netif_config()`, `_netif_apply(ifkey,cfg)` | The netif config in force / re-apply to a live netif. |

Callbacks in `usb_eth_host_config_t`: `on_eth_ip_up` / `on_eth_ip_lost`
are a **balanced pair**: `lost` fires on `IP_EVENT_ETH_LOST_IP`, on the
overlap suspend, **and from the driver-stopped path** (since 2026-08-24:
lwIP posts no LOST_IP when a netif is simply stopped and destroyed
(USB unplug or the ESPNetLink cutting its data lines) so `usb_host_manager`
kept reporting "uplink up"). Context: the system event task or the USB
host task; never block in them.

## Dependencies

`cherryusb` (host core + classes; `platform/idf/usbh_net.c` must stay
excluded or its strong hooks preempt these), `esp_netif`, `esp_timer`,
`lwip`, `esp_driver_gpio`, `log_manager`. Started by `usb_host_manager`.

## Settings

None (configured through `usb_eth_host_start()` by `usb_host_manager`).

## Memory footprint (estimated)

- CherryUSB host: the psc task (`CONFIG_USBHOST_PSC_STACKSIZE`) + per
  class RX thread (NCM: 2048 B), internal; exist only while a device is
  attached.
- `eth shared buf`: 4624 B internal DMA while attached.
- Static: ~0.2 KB `.bss` (active driver/ifkey/VID/PID, DNS guard, flags).

## Testing

Live only (USB DMA + CherryUSB): the USB-Ethernet bench (`.\test.ps1
usbeth`), the ESPNetLink bench, and `espnetlink_link`'s cut/recovery
sequence exercise attach, IP up/lost, overlap suspend and DNS restore.

Disconnect ordering (2026-10-12): each class rx thread leaves as soon as
its port disconnects and the class disconnect waits for it before the
netif glue stops and the hub releases the port (`cherryusb/PROVENANCE.md`,
the 2026-10-12 entry: a VBUS cycle of the ESPNetLink while the wire
carried traffic aborted in the thread's control transfer). The proof is
`POST /api/espnetlink/repair` repeated while the wire carries traffic (the
garage bench's g6 leg and the repair hammer).
