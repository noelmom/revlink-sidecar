# RevLink ESP32-P4 firmware

This target is the consumer-product hardware acceptance path for the Waveshare
`ESP32-P4-WIFI6-DEV-KIT` (SKU 32054) and `ESP32-P4-NANO`. They share the same
RevLink application and peripheral adapters but require separate binary builds
because the Dev Kit uses P4 revision 3.x silicon and the current Nano uses P4
revision 1.x silicon. Only the Nano has a published image; build the Dev Kit
yourself.

## Pinned environment

- Target: `esp32p4`
- ESP-IDF: `v6.0.2`
- Flash: 16 MB
- Stacked PSRAM: 32 MB, hex mode at 200 MHz
- Programming console: Type-C UART through the onboard WCH bridge
- AccessPort port: USB-A OTG 2.0 high-speed host, with the board's OTG jumper
  set to `HOST`

### ESP32-P4-NANO build

The Nano preserves the accepted RevLink prototype wiring:

- OLED: SPI2, CLK GPIO23, MOSI GPIO22, CS GPIO21, D/C GPIO20, RESET GPIO2;
- microSD: GPIO39 through GPIO44;
- ESP32-C6 Wi-Fi/BT link: SDIO GPIO14 through GPIO19, reset GPIO54; and
- AccessPort: onboard USB-A OTG 2.0 high-speed host.

The Nano overlay enables the SH1106 controller's 180-degree orientation for
the compact enclosure. Rendering stays in the shared framebuffer; the
board-specific overlay changes only the controller's segment and COM scan
directions.

Do not flash the Dev Kit's revision-3.x image onto the Nano. Build the Nano in
its own clean directory with the revision-1.x overlay:

```sh
source "$HOME/.espressif/frameworks/esp-idf-v6.0.2/export.sh"
NANO_BUILD="$PWD/firmware/esp32p4/build-nano"
idf.py -C firmware/esp32p4 \
  -B "$NANO_BUILD" \
  -D "SDKCONFIG=$NANO_BUILD/sdkconfig" \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.oled.defaults;sdkconfig.wifi-scan.defaults;sdkconfig.wifi-join.defaults;sdkconfig.network-runtime.defaults;sdkconfig.onboarding.defaults;sdkconfig.nano.defaults' \
  set-target esp32p4 build
```

## Normal runtime and safety defaults

The normal build powers and enumerates the proven high-speed USB host, but it
does not start a file session unless the saved auto-sync preference is enabled
or the application receives an explicit manual request:

- `CONFIG_REVLINK_USB_ROOT_PORT_POWER` is enabled.
- `CONFIG_REVLINK_RUNTIME_SYNC` is enabled.
- `CONFIG_REVLINK_SYNC_AUTO_ON_ATTACH_DEFAULT` is disabled.
- `CONFIG_REVLINK_USB_INTERFACE_CLAIM_ACCEPTANCE` is disabled.
- `CONFIG_REVLINK_USB_ROOT_LIST_ACCEPTANCE` is disabled.
- `CONFIG_REVLINK_USB_DOWNLOAD_ACCEPTANCE` is disabled.
- `CONFIG_REVLINK_USB_INCREMENTAL_SYNC_ACCEPTANCE` is disabled.
- `CONFIG_REVLINK_ALLOW_DEVICE_WRITES` is enabled for guarded map transfer.
- `CONFIG_REVLINK_ALLOW_DEVICE_DELETES` is enabled in the Nano profile for
  guarded deletion inside `maps/` and `datalog/`.

The runtime coordinator exposes saved auto-sync policy, manual sync, progress
snapshots, and cooperative cancel to the embedded local API. Its bounded
read-only USB batch still permits at most four new files, 16 MiB per batch, and
8 MiB per file. A normal sync automatically opens clean continuation batches
until the complete safe inventory is verified or a batch makes no forward
progress. Guarded map transfer is available only after persistent owner
consent is enabled in Settings; new and factory-reset Sidecars default locked.
Startup-screen transfer uses the same guarded service and fixed framebuffer
target but still awaits its first live P4 write/read-back acceptance. Deletion
of files inside `maps/` and `datalog/` is compiled into the Nano profile from
0.2.2 and sits behind its own consent, separate from the write consent and
never implied by it.

The transport-neutral `revlink_control` service exposes those runtime
operations as status, auto-sync, manual-sync, and cancel commands. It returns
an immutable post-command snapshot and contains no HTTP, JSON, Wi-Fi, USB, or
storage implementation. The embedded portal's local HTTP adapter translates
requests into this contract rather than reaching into the USB stack.

The platform-neutral `revlink_usb_control` component is compiled into the P4
application and host-tested independently. The current P4 adapter does not
drive external GPIO because the isolation modules have not completed bench
bring-up. Do not assign USB control to GPIO39 through GPIO44; those pins are
reserved by the accepted microSD interface.

### Prototype status display

The optional `CONFIG_REVLINK_STATUS_OLED` adapter drives the existing
1.3-inch SH1106 SPI OLED. The screen shows the RevLink boot animation,
AccessPort availability, sync progress, recovery, completion, and actionable
fault states. Product state and copy come from the platform-neutral
`revlink_status` component; the SH1106 framebuffer and SPI transport remain a
replaceable P4 adapter.

Physical display acceptance passed on 2026-07-27: the splash and live status
rendered at the correct orientation and size while an attached AccessPort
advanced from inspection to ready state.

Use the reviewed prototype wiring in
[`OLED_BRINGUP.md`](../../docs/OLED_BRINGUP.md).
`sdkconfig.oled.defaults` is part of the shipped Nano fragment list, so the
published image includes this adapter; the panel itself is optional and the
firmware runs without one. To build the display overlay on its own:

```sh
source "$HOME/.espressif/frameworks/esp-idf-v6.0.2/export.sh"
idf.py -C firmware/esp32p4 \
  -B firmware/esp32p4/build-oled \
  -D SDKCONFIG=sdkconfig.oled \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.oled.defaults' \
  set-target esp32p4 build
```

### Development UART control

The OLED bench overlay also enables `CONFIG_REVLINK_DEV_CONSOLE`. This
programming-UART adapter exercises the same transport-neutral control service
that future authenticated HTTP and BLE adapters will use:

```text
status
sync
cancel
auto on
auto off
help
```

The grammar is fixed and fail-closed. It has no command for device paths,
protocol opcodes, upload, delete, formatting, or arbitrary shell execution.
`auto on` and `auto off` update the saved automatic-sync preference; the
reviewed bench image is left with that preference off.

Live acceptance passed on 2026-07-27. A manual `sync` issued over the UART
selected the authoritative AccessPort namespace, reconciled 33 files as
verified cache hits, sent the polite subtype `0x05`, received subtype `0x35`,
and returned the re-enumerated device to `available`. A subsequent `status`
reported `completed`, progress `33/33`, auto-sync off, and writes locked. The
nonblocking console was also left idle for several seconds without prompt
repainting or interfering with enumeration.

### Development-board soft power

The Waveshare board's onboard `BOOT` button is GPIO35. After firmware boot, a
double press shows a full-screen Wi-Fi join QR when the RevLink hotspot is
active. When the Sidecar is already connected to a preferred Wi-Fi network,
the same double press shows its `revlink-<suffix>.local` browser address for
20 seconds instead. One additional short press dismisses either overlay.
The hotspot QR has a 30-second timeout and contains only the volatile
per-boot hotspot credential; generation is synchronous with the QR library's
input logging suppressed, and the payload is zeroized after its display
bitmap is captured. An active sync dismisses the URL overlay so transfer
safety status always wins.

The display also treats physical device state as newer than a terminal sync
result. Unplugging the AccessPort after a successful backup replaces
`<vehicle> / BACKUP COMPLETE / SAFE TO DISCONNECT` with
`NO DEVICE / ACCESSPORT OFFLINE / CONNECT TO SYNC`; a USB fault similarly
replaces stale completion with the attention view.

Once the read-only identity handshake has supplied the installed vehicle, the
ready view shows `<vehicle> / ACCESSPORT CONNECTED / READY TO SYNC`. Before
that authoritative identity is available, `ACCESSPORT` is used as the safe
headline fallback. The known vehicle is preserved through the expected
software re-enumeration after a clean session close, but is cleared for a
genuinely new physical attachment.

A two-second hold requests a demo-safe logical shutdown:

1. block new manual and automatic sync requests;
2. cooperatively cancel a queued or active sync and wait for a terminal state;
3. stop the logical USB root port;
4. abort any incomplete cache writer, release the selected device namespace,
   unmount the microSD card, and release its LDO; and
5. enter deep sleep.

Wake by pressing `RST` or cycling external power. `BOOT` is not configured as
a wake source on this development board.

This is a convenient prototype control, not physical power-off. The current
board cannot isolate USB D+/D- or VBUS in firmware. A properly initialized
session can return the AccessPort to Gauges by sending the polite subtype
`0x05`, receiving subtype `0x35`, and releasing the interface. An interrupted
or incomplete session may leave the AccessPort in PC mode, but a stale
read-only session has been recovered by reinitializing the complete session
and then performing the acknowledged polite close. The `revlink_usb_link`
board adapter still reports physical-isolation capabilities explicitly so an
optional future data/VBUS switch can be evaluated as defense in depth rather
than as the normal session-close path or a firmware dependency.

Idle-device live acceptance passed on 2026-07-27. With an enumerated
AccessPort and no active sync, the two-second hold blocked new work, disabled
the logical root port, produced a clean USB detach, unmounted and powered down
the microSD card, and entered deep sleep. Active-sync cancellation acceptance
remains pending.

## Architecture

The P4 firmware consumes platform-neutral components from
`firmware/components/`.
The protocol, lifecycle state machine, safety policy, and application
orchestration have no ESP-IDF dependency. The ESP-IDF USB host is an adapter
that emits normalized lifecycle events, and this target's `main/` directory is
the sole active firmware composition root plus the P4 SDMMC adapter.

See [`docs/FIRMWARE_ARCHITECTURE.md`](../../docs/FIRMWARE_ARCHITECTURE.md) for
the dependency rules and extension workflow.

## Build and flash

```sh
source "$HOME/.espressif/frameworks/esp-idf-v6.0.2/export.sh"
idf.py -C firmware/esp32p4 set-target esp32p4
idf.py -C firmware/esp32p4 build
idf.py -C firmware/esp32p4 \
  -p <your serial port> flash monitor
```

The first USB milestone is descriptor-only enumeration of VID/PID
`1a84:0121` at high speed with interface `0`, bulk OUT `0x03`, bulk IN `0x82`,
and 512-byte maximum packets. No proprietary request is permitted before that
gate passes.

## Hardware acceptance records

The dated bench records for this target — USB high-speed bring-up, microSD
coexistence with the onboard radio, incremental sync, multi-device safety,
session close, and guarded map transfer — are in
[`../../docs/ACCEPTANCE_LOG.md`](../../docs/ACCEPTANCE_LOG.md). They are
historical entries, not a description of the current build.
