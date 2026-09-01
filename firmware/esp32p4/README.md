# RevLink ESP32-P4 firmware

This target is the consumer-product hardware acceptance path for the Waveshare
`ESP32-P4-WIFI6-DEV-KIT` (SKU 32054) and `ESP32-P4-NANO`. They share the same
RevLink application and peripheral adapters but require separate binary builds
because the Dev Kit uses P4 revision 3.x silicon and the current Nano uses P4
revision 1.x silicon. The Raspberry Pi/Linux implementation remains the
protocol oracle, advanced-feature reference, and recovery environment—not the
shipping hardware direction.

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

The runtime coordinator exposes saved auto-sync policy, manual sync, progress
snapshots, and cooperative cancel to the embedded local API. Its bounded
read-only USB batch still permits at most four new files, 16 MiB per batch, and
8 MiB per file. A normal sync automatically opens clean continuation batches
until the complete safe inventory is verified or a batch makes no forward
progress. Guarded map transfer is available only after persistent owner
consent is enabled in Settings; new and factory-reset Sidecars default locked.
Startup-screen transfer uses the same guarded service and fixed framebuffer
target but still awaits its first live P4 write/read-back acceptance. Deletion
and every other device write remain unavailable.

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
The normal production defaults leave this adapter disabled. For the current
bench display build:

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
  -p /dev/cu.usbmodem5B910553771 flash monitor
```

The first USB milestone is descriptor-only enumeration of VID/PID
`1a84:0121` at high speed with interface `0`, bulk OUT `0x03`, bulk IN `0x82`,
and 512-byte maximum packets. No proprietary request is permitted before that
gate passes.

## Live stage-one acceptance

Stage one passed on the physical development board on 2026-07-26:

- ESP32-P4 revision 3.1 at 400 MHz;
- 16 MB DIO flash at 80 MHz;
- 32 MB hex PSRAM at 200 MHz, including the ESP-IDF memory test;
- byte-exact offline AccessPort protocol-vector self-test;
- high-speed OTG 2.0 host installation on peripheral 0;
- root-port power disabled; and
- AccessPort storage-write capability disabled.

The application image was 288,352 bytes and left 73% of the initial 1 MB
factory partition free.

## Live microSD acceptance

The onboard TF slot passed destructive storage acceptance on 2026-07-26:

- card: `SD64G`, SDHC, 124,669,952 sectors at 512 bytes;
- bus: SDMMC 4-bit at 20 MHz;
- board wiring: CLK GPIO43, CMD GPIO44, D0-D3 GPIO39-GPIO42;
- power: ESP32-P4 on-chip SD LDO channel 4;
- formatted capacity: 63,815,352,320 bytes;
- acceptance write: 1,048,576 deterministic bytes;
- acceptance SHA-256:
  `fdb4e820eb31447d69d6af8468cd491538c4cc68485844e1394593e86d2e5920`;
- flush, atomic rename, unmount, remount, read-back, and checksum all passed;
  and
- the final non-formatting firmware preserved the 161-byte acceptance marker
  across a hard reset.

The mounted layout is:

```text
/sdcard/revlink/
├── devices/
└── system/
    ├── acceptance/sd-card-ok.txt
    ├── config/
    ├── recovery/
    └── updates/
```

`CONFIG_REVLINK_SD_FORMAT_ACCEPTANCE` is an explicit, destructive, one-time
gate and is disabled in the firmware left on the board. Normal boots never
format on mount failure. The architecture-refactored normal image is 393,488
bytes and leaves 62% of the initial 1 MB factory partition free.

### Missing or unreadable card recovery

Storage failure is fail-closed without making the Sidecar unreachable:

- a missing or electrically unresponsive card shows **SD CARD MISSING** and
  never offers a format action;
- a card that responds but has an unreadable or unsupported filesystem shows
  **SD UNREADABLE**;
- the first BOOT-button double press opens an explicit all-data-loss warning;
- a second BOOT-button double press within 20 seconds authorizes the format;
- allowing the countdown to expire returns to the initial warning without
  modifying the card; and
- a successful format provisions the RevLink layout and restarts the Sidecar.

Wi-Fi, the local portal, the display, and the physical recovery controller
continue running in degraded mode while storage is unavailable. Sync and
storage-backed device writes remain disabled until the card mounts and the
RevLink layout is successfully provisioned. Firmware never interprets a
missing/unresponsive card as permission to format; filesystem-format
acceptance must use a dedicated disposable card rather than a customer's
existing cache.

Full-card development backup and restore policy is documented in
[`../../docs/SIDECAR_SD_BACKUP.md`](../../docs/SIDECAR_SD_BACKUP.md). A
verified backup is required before storage-schema changes, destructive
storage acceptance, and live unreadable-card format testing.

For normal users, **Settings → Backup & restore** creates a portable,
versioned `.revlink-backup` containing every device dataset. Import validates
the complete archive and every file digest before enabling a non-destructive
merge: missing files are restored atomically, identical files are skipped, and
conflicts are reported without overwriting the Sidecar. Device identity,
network credentials, firmware, and transient recovery state are excluded.

## Live onboard-radio and storage coexistence acceptance

The development board has one ESP32-P4 SD/MMC host shared by two onboard
devices: the ESP32-C6 radio link and the TF socket. RevLink keeps two explicit
build profiles instead of allowing managed-component defaults to choose the
owner:

| Profile | ESP32-C6 transport | microSD transport |
| --- | --- | --- |
| normal/default | disabled | SDMMC 4-bit, GPIO39-GPIO44 |
| `wifi-scan` | SDIO 4-bit, GPIO14-GPIO19, reset GPIO54 | SPI3, CLK 43, MOSI 44, MISO 39, CS 42 |

The OLED remains on SPI2 (`CLK 23`, `MOSI 22`, `CS 21`, `D/C 20`,
`RESET 2`) and AccessPort USB remains on the independent high-speed USB host.
Both profiles compile independently. The normal image explicitly disables
ESP-Hosted and remote Wi-Fi and contains no C6 radio startup path; merely
fetching those managed components cannot claim the SD/MMC controller.

A third `wifi-join` acceptance profile adds a local-only, RAM-only join
command on top of `wifi-scan`. It is not a provisioning implementation and
is excluded from the normal image.

A fourth `network-runtime` profile composes the platform-neutral network
coordinator with the same C6 adapter. It adds a RAM-only WPA2 fallback SoftAP,
explicit status/recovery console commands, and sync transfer locking.

The `onboarding` profile adds a local setup page on port 80, bounded captive
DNS and captive-probe routes, a unique mDNS hostname, and a hardware-RNG
hotspot password shown only on the OLED. A station credential is saved as one
versioned NVS record only after a successful association and is reused after
normal restarts and firmware updates. A failed candidate never replaces the
last working record. The rotating setup-hotspot credential remains RAM-only,
and every HTTP credential buffer is zeroized after use. The setup password
field has an accessible local Show/Hide control. After setup, `/` and
`/index.html` serve the Sidecar portal, `/setup` reopens onboarding, and
captive-probe success remains isolated to the platform probe paths. Production
hardware must enable encrypted NVS/flash encryption and secure boot before
this credential store is considered a shipping security boundary. AccessPort
writes remain excluded from this acceptance image.

Build the isolated radio profile with:

```sh
source "$HOME/.espressif/frameworks/esp-idf-v6.0.2/export.sh"
idf.py -C firmware/esp32p4 \
  -B build-wifi-scan \
  -D SDKCONFIG=sdkconfig.wifi-scan \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.oled.defaults;sdkconfig.wifi-scan.defaults' \
  set-target esp32p4 build
```

Live coexistence acceptance passed on 2026-07-27 after a true board power
cycle:

- ESP-Hosted initialized the onboard ESP32-C6 over 4-bit SDIO at 40 MHz with
  a 512-byte block size;
- the existing 64 GB card mounted on SPI3 without formatting and preserved
  its acceptance marker and cached objects;
- one bounded anonymous scan detected six networks and retained only count,
  strongest RSSI, and strongest channel—no SSID, BSSID, credential, or
  connection was stored or logged;
- the AccessPort simultaneously enumerated at high speed with 512-byte file
  endpoints;
- one read-only incremental sync reconciled 33 candidates as 33 verified
  cache hits, downloaded zero bytes, sent subtype `0x05`, received subtype
  `0x35`, and cleanly re-enumerated the AccessPort; and
- automatic sync and every AccessPort write remained disabled.

A card already initialized in native SD mode may require a true board power
cycle before the SPI profile can select SPI mode. A CPU-only reset did not
reliably reset the powered removable card. See
[`WIFI_BRINGUP.md`](../../docs/WIFI_BRINGUP.md)
for the accepted resource map and repeatable checks.

The RAM-only station-join acceptance also passed on 2026-07-27. The onboard
C6 associated, completed DHCP, and resolved a fixed DNS name without logging
or persisting the network identity or credential. AccessPort writes remained
uncompiled and auto-sync remained off. The existing C6 version warning is
therefore diagnostic information, not by itself a reason to replace working
coprocessor firmware.

The integrated coordinator/fallback-hotspot profile passed its first live gate
on 2026-07-27. With no saved network in RAM, the coordinator selected fallback
mode, waited safely for a local hidden credential, started the C6 as
`RevLink-<last eight AP-MAC digits>`, enabled the default private AP/DHCP
netif,
and reached `hotspot-ready`. The OLED, SPI3 microSD, USB host, and attached
AccessPort remained operational. The password was neither persisted nor
logged, auto-sync remained off, and AccessPort writes remained disabled.

Build it with:

```sh
source "$HOME/.espressif/frameworks/esp-idf-v6.0.2/export.sh"
idf.py -C firmware/esp32p4 \
  -B firmware/esp32p4/build-network-runtime \
  -D "SDKCONFIG=$PWD/firmware/esp32p4/build-network-runtime/sdkconfig" \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.oled.defaults;sdkconfig.wifi-scan.defaults;sdkconfig.wifi-join.defaults;sdkconfig.network-runtime.defaults' \
  build
```

Build the onboarding acceptance image with:

```sh
source "$HOME/.espressif/frameworks/esp-idf-v6.0.2/export.sh"
idf.py -C firmware/esp32p4 \
  -B firmware/esp32p4/build-onboarding \
  -D "SDKCONFIG=$PWD/firmware/esp32p4/build-onboarding/sdkconfig" \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.oled.defaults;sdkconfig.wifi-scan.defaults;sdkconfig.wifi-join.defaults;sdkconfig.network-runtime.defaults;sdkconfig.onboarding.defaults' \
  build
```

The project uses `partitions.csv` instead of ESP-IDF's stock 1 MiB single-app
layout. The 16 MiB flash now contains a 4 MiB immutable factory/recovery image,
two 4 MiB OTA release slots, redundant OTA selection data, and a bounded crash
partition. Updated images boot pending verification and roll back if they
restart before an explicit application health confirmation. The update
transport and health-confirmation service remain deliberately unimplemented;
see [`../../docs/FIRMWARE_UPDATE.md`](../../docs/FIRMWARE_UPDATE.md).

The onboarding image passed live hardware acceptance on 2026-07-27:

- the private SoftAP and DHCP reached ready state;
- a real phone/tablet joined with the OLED-only credential and opened the
  captive setup page;
- an intentionally wrong station password restored the same hotspot and
  credential, allowing automatic client rejoin;
- valid credentials associated with the target LAN, obtained DHCP, served
  HTTP, and originally published `http://revlink-<identity-v1-id>.local`;
- identity v2 now publishes `http://revlink-e639e871.local` on the development
  board, persists an independent 128-bit `r1_...` Sidecar ID in NVS, reports
  the full hardware MAC only through local diagnostics, and leaves the
  AccessPort cache namespace unchanged;
- the 64 GB card mounted without formatting; and
- the attached AccessPort remained available at high speed with 512-byte file
  endpoints.

A read-only 33-file incremental sync also accepted the transfer boundary. A
mode-change request issued immediately after the sync became `queued` returned
`ESP_ERR_INVALID_STATE`; the sync completed as 33 verified cache hits, closed
the AccessPort session with `0x05`/`0x35`, and returned the guard to idle.
Auto-sync stayed off and writes stayed compiled out.

### Partially accepted beta safety gate: multiple AccessPorts

The USB adapter now counts live devices that match the complete supported
endpoint contract and refuses a transaction unless exactly one is eligible.
Work is pinned to the selected handle and attachment generation. A second unit
latches a `multiple-devices` conflict, cooperatively cancels an active
read-only sync, blocks manual/automatic work and recovery, and is reported in
the portal and OLED. Automatic sync remains disarmed after a full detach until
the customer deliberately starts a new sync.

The production image and the platform-neutral conflict lifecycle tests pass.
The host suite also proves that reversed USB address/enumeration order produces
the same refusal and that late detach/conflict events cannot replace a newer
eligible-device topology. Production topology events carry a monotonic
transport-owned revision; revision zero remains reserved for platform-neutral
legacy fixtures.
The powered-hub/two-AccessPort idle attach and full-detach recovery slice
passed live on 2026-07-29: the second unit latched the conflict without
starting a sync, removal of only one unit stayed fail-closed, full detach
cleared the latch, and a single reattach recovered without rebooting. The
remaining hardware matrix in
[`../../docs/SINGLE_ACCESSPORT_SAFETY.md`](../../docs/SINGLE_ACCESSPORT_SAFETY.md)
is still required before beta. Guarded writes are compiled into the normal
image, remain owner-disabled on a new or factory-reset Sidecar, and retain
separate target-pinning and abort acceptance gates.

The onboarding build now embeds the first functional local product portal.
After choosing direct mode or completing Wi-Fi setup, `/` shows live device,
AccessPort, sync, storage, and network state plus the bounded cached-file
inventory. The inventory identifies datalogs, maps, the current startup
framebuffer, and screenshots explicitly without changing the authoritative
storage projection. Its safe control surface is deliberately limited to:

- `GET /api/portal/status`
- `GET /api/portal/devices`
- `GET /api/portal/files`
- `POST /api/portal/device/select` with a validated cached namespace key
- `POST /api/portal/notes` with a cached version digest and note
- `POST /api/portal/log-map` with exact cached datalog and map digests
- `POST /api/portal/sync`
- `POST /api/portal/sync/cancel`
- `POST /api/portal/auto-sync` with `enabled=true` or `enabled=false`
- `POST /api/portal/writes` with `enabled=true` or `enabled=false`
- `POST /api/portal/maps/stage`
- `POST /api/portal/maps/apply`
- `GET /api/portal/startup/profiles`
- `POST /api/portal/startup/profiles`
- `GET /api/portal/startup/profile?id=<profile-id>`
- `POST /api/portal/startup/apply`

Mutating portal requests require `X-RevLink-Portal: 1`. Sync controls pass
through the transport-neutral control service; notes and optional
datalog-to-map links are stored by immutable SHA-256 version in the selected
device's `annotations.tsv`. Annotation v2 retains a parser for existing v1
note-only files. A map link is accepted only when both digests identify the
correct file kinds in the same selected device namespace. The portal reads
description, declared vehicle, vendor/version, IDs, checksum/lock fields, and
feature declarations by decrypting only the public header of the immutable
cached `.ptm` in the browser. It does not decrypt or present the private tuning
payload, and a matching vehicle declaration is explicitly not a flash-safety
guarantee. The isolated map
path stages one `.ptm` on microSD, requires persistent owner consent, refuses
existing destinations, pins connected identity and topology, and verifies a
fresh read-back byte-for-byte. Delete remains a separate unavailable
capability. The storage adapter publishes an immutable projection capped at 128
current-manifest records so the HTTP worker never owns the mutable sync
manifest or a USB handle.

The Images workspace stores reusable startup profiles under the selected
AccessPort's microSD namespace, not a live USB session. Source images are
cropped and positioned in an exact 240 × 320 preview, converted in-browser to
153,600-byte little-endian RGB565, and saved without changing the AccessPort.
Supported screenshots are backed up into the same device-scoped dataset during
every normal incremental sync. A cached screenshot can be promoted through the
same crop editor into a reusable startup profile without modifying or removing
the original screenshot backup.
Each unique startup framebuffer is also snapshotted automatically into the
reusable library on both a new download and a verified incremental cache hit.
This makes the previously active screen recoverable after a later replacement.
One-click apply is identity-pinned to the selected and attached device, fixed
to `images/startup_screen.fb`, blocked during synchronization, requires the
persistent Settings write toggle plus replacement confirmation, and uses the
same acknowledged upload and byte-identical read-back service as maps. The
route is implemented but has not received live startup-screen write
acceptance. Delete remains unavailable.

The UI separates the live attachment from the dataset being viewed.
`attachedAccessPort` comes only from the current USB identity handshake, while
the compatibility `accessPort` field and file inventory represent the
selected microSD namespace. A global responsive **Viewing dataset** selector
lists up to 16 verified cached namespaces and labels the selected one as
**Connected now** or **Cached dataset**. Selection is refused during active
USB/sync work, clears browser file selections and an open log viewer, and
never affects the OLED or directs a transaction to a different physical unit.

The normal runtime also performs an identity-only read as soon as one eligible
AccessPort is available when auto-sync is not already doing a full sync. It
uses the four authoritative identity mini requests and clean acknowledged
session close, but skips root/file listing and downloads. This lets a newly
attached donor identify itself in the Device card without forcing a backup.
True physical detach clears the live identity; the expected polite
re-enumeration after close does not.

The identity-only path passed live on 2026-07-28 with a second donor unit:
`AP3-VLK-002 / VLK0207629 / v1.7.5.0-25674`. The P4 completed all four identity
requests, sent `0x05`, received `0x35`, released the interface, and classified
the resulting detach/attach as polite software re-enumeration. With the
production hardware profile, the existing microSD mounted normally and the
catalog exposed two verified namespaces. Live selector acceptance switched
from the donor's 2-file `AP3-VLK-002` cache to the 34-file `AP3-SUB-004` cache
and back while `attachedAccessPort` remained the donor. The operation changed
only the local selected-namespace pointer; AccessPort writes and deletes
were not invoked. A prior `ESP_ERR_TIMEOUT` was limited to a diagnostic
flash using the minimal profile's incompatible SD transport, not a card
failure.

The product firmware now classifies the authoritative identity against an
exact, generated-at-review-time catalog of 64 parts across 12 families. The
accepted `AP3-SUB-004` and donor `AP3-VLK-002` identities both resolve through
that catalog. Matching is exact and case-sensitive: an unknown or future part
is still exposed in `attachedAccessPort` for troubleshooting, but the USB
worker closes the session before namespace selection, listing, or downloading.
Catalog membership grants only the reviewed read-only file path; it does not
enable live telemetry, map decoding, upload, deletion, or firmware operations.
The API exposes catalog revision, family, and compatibility fields for both
the attached device and selected cached dataset. See
[`docs/SUPPORTED_ACCESSPORTS.md`](../../docs/SUPPORTED_ACCESSPORTS.md).
Full local CI passed with 14/14 host suites, and the production image then
identified the attached donor as exact part `AP3-VLK-002`, family
`Volkswagen / Audi`, before completing the normal acknowledged close.

The first live portal sync passed on 2026-07-27 over the board's LAN station
connection. The API identified the attached high-speed AccessPort, projected
its authoritative identity, compared 33 datalogs, verified all 33 as
incremental cache hits, completed with no platform error, closed the file
session, and returned the device to `available`. The inventory reported and
returned all 33 records. Writes and deletes remained compiled out throughout
the run.

The version-scoped notes and manifest-v2 timestamp path passed live on
2026-07-27. The portal saved a note against the accepted
`datalog35.csv` SHA-256, rebooted the board, re-established the authoritative
device namespace with a 33-file all-skip incremental sync, and returned the
same note. An empty note then removed it. The note update used network-trusted
UTC. Existing manifest-v1 entries initially migrate at timestamp zero rather
than receiving fabricated historical dates. The normal portal image now
accepts the local browser's UTC only while no RTC or network anchor exists,
and automatically repairs zero-valued current/history records when bounded UTC
becomes available. Browser time cannot overwrite an existing anchor; later
network time may supersede it.
Large file-inventory JSON and note-form buffers are heap allocated so the
6 KiB HTTP task stack remains bounded.

A later owner-directed maintenance pass assigned the explicit placeholder
`2026-07-26T19:00:00Z` to the 33 zero-valued legacy current/history records on
the development card. The 34th record already had trusted time and remained
unchanged. The gated `sdkconfig.metadata-backfill.defaults` profile exposes the
local UART command only in a temporary maintenance image; the normal product
image keeps `REVLINK_LOCAL_METADATA_BACKFILL_ACCEPTANCE` disabled. The command
updates only zero-valued microSD metadata, never cached bytes or AccessPort
storage, and a second pass updated zero records.

The same zero-only repair primitive is now part of the normal runtime. It runs
after RTC restoration, network synchronization, or the first accepted local
browser time observation. A programming-port reset without the supported RTC
battery was observed as a power-on reset and did not retain system time, so
the rechargeable RTC-battery cold-power test remains required for unattended
offline auto-sync.

The hardened path passed live on 2026-07-27. After joining the development
LAN, the first bounded browser-clock observation returned HTTP 200, a second
valid observation returned HTTP 409, and an out-of-range observation returned
HTTP 400. The portal then projected all 34 cached versions with zero
unavailable Initial sync values. The board remained stable after moving the
large inventory projection off the main task's stack; AccessPort writes stayed
compiled out.

The owner-confirmed datalog chronology was then repaired locally in a temporary
maintenance image. The 33 current versions were sequenced at 30-second
intervals in the observed order `datalog28..58`, then `datalog1`, then
`datalog2`; matching history records were selected by both path and SHA-256.
Normal firmware was restored afterward and the maintenance option is disabled.
The portal now offers Newest, Oldest, filename, and file-size sorting, with an
inline explanation that AccessPort datalog filenames can wrap or be reused.

The root route now also checks the live network coordinator. A UART or future
non-onboarding station connection that reaches `client-ready` opens the product
portal even if the transient onboarding state remains `idle`; an unconfigured
fallback hotspot still shows setup. This was accepted live with
`client-ready/idle`, where `/` served **Ready when your car is.**

The complete portal acceptance pass also verified:

- the FAT volume reports 63,815,352,320 total bytes through
  `esp_vfs_fat_info`, rather than an unsupported POSIX `statvfs` projection;
- the root document and API responses explicitly close HTTP connections, and
  the LWIP socket pool is sized to 16;
- 80 concurrent/repeated status requests completed without `EMFILE` or an
  HTTP accept failure;
- auto-sync could be enabled and disabled through the portal and was left
  disabled;
- cancellation during the identity handshake was classified as `cancelled`
  with platform error zero, sent `0x05`, received `0x35`, and returned the
  AccessPort to `available`; and
- a normal sync immediately after cancellation completed with 33 cache hits,
  zero pending files, and no platform error.

Portal identity is rehydrated from microSD during every boot; it is not keyed
by the Sidecar SSID, hostname, or MAC suffix. The storage root keeps a
FAT-safe `last-device` pointer to the last authoritative AccessPort namespace.
That pointer selects only the portal's offline projection—the next real sync
still reads the attached AccessPort's true identity before selecting writable
session state. An upgrade without the pointer adopts a legacy namespace only
when exactly one valid device namespace exists; if several exist, it refuses
to guess until a real sync identifies the connected device.

This migration passed live on 2026-07-27 after the local URL changed to
`revlink-e639e871.local`. Two consecutive resets restored the existing
`AP3-SUB-004` identity, firmware `v1.7.6.0-28968`, vehicle, and all 34 cached
files without downloading, copying, renaming, or merging any stored object.
Writes and deletes remained compiled out.

The ready OLED uses that same bounded identity projection. Its identified
copy is `<vehicle> / <part> CONNECTED / READY TO SYNC`; before any identity is
available it falls back to `ACCESSPORT / DEVICE CONNECTED / READY TO SYNC`.
The first physical enumeration after boot may retain the restored
last-authoritative identity. A later true device replacement during the same
boot clears it until the replacement completes an identity handshake.

Persistent credentials, factory reset, force-hotspot recovery, production
authentication, and the final setup UX remain pending.

The 2026-07-28 stale-link watchdog closes a separate failure mode that normal
disconnect callbacks missed. While client-ready and transfer-idle, the P4
probes its DHCP gateway every 15 seconds. Three consecutive failures restart
the hosted radio stack and retry the selected RAM-only network; the existing
reconnect timeout then restores the fallback hotspot. If both recovery paths
fault, one RTC-guarded reboot is allowed, and that allowance rearms only after
60 seconds of stable operation.

The flashed acceptance image retains the generated setup credential only in
volatile runtime/display state after hiding it in client mode. A network-state
observer restores the same OLED credential if the coordinator returns to
hotspot mode, while setup-start failure explicitly zeroizes it. This recovery
path passed live link-loss acceptance: after a connected iPhone hotspot was
disabled, RevLink attempted one bounded same-network reconnect, restored its
own hotspot/DHCP after the 30-second budget, and redisplayed the same
credential without a restart.

The current source adds an OLED reconnect projection driven by the
coordinator snapshot rather than a second timer: `WIFI LOST`, remaining
seconds, elapsed-budget progress, and `HOTSPOT NEXT`. Sync and USB-safety
messages keep priority. This display change is host-tested and builds in the
onboarding image, but still requires the link-loss hardware acceptance listed
in `TODO.md`.

The iPhone association required **Maximize Compatibility** so the phone
advertised a compatible 2.4 GHz hotspot. The onboarding UI must surface that
instruction for iPhone users.

A five-rapid-press `BOOT` gesture is recorded only as a future preferred-Wi-Fi
retry candidate. Do not implement it independently of the complete button map
and safety policy.

The prototype credential is eight characters from a lowercase, ambiguity-free
31-symbol alphabet. It meets the WPA2 minimum, needs no Shift key, avoids
`0/O` and `1/I/L`, and provides about 40 bits of per-boot randomness. The
production credential must become a securely stored stable per-device secret,
not a value derived from the public MAC address.

## Live high-speed USB acceptance

Descriptor-only AccessPort enumeration passed on 2026-07-26:

- the USB-A port measured 5.00 V at no load with the OTG selector in `HOST`;
- the AccessPort attached directly to the high-speed root port;
- negotiated speed: high speed;
- identity: VID/PID `1a84:0121`, vendor-specific device class;
- interface `0` exposed bulk OUT `0x03` and bulk IN `0x82`;
- both file-channel endpoints reported 512-byte maximum packets;
- all four standard configuration descriptors were scanned and matched; and
- the first descriptor-only gate did not claim an interface or submit a
  proprietary transfer.

The descriptors also exposed the separate 64-byte `0x02` OUT / `0x81` IN
pair. This confirms that the earlier ESP32-S3 full-speed result saw only the
small-packet channel because it could not negotiate high speed; it does not
make that channel a substitute for the proven file channel.

The next interface lifecycle gate also passed on 2026-07-26. The adapter
claimed interface `0`, allocated all four endpoint pipes, released the
interface cleanly, and submitted zero bulk transfers. The application core
observed the deterministic state sequence:

```text
waiting -> inspecting -> available -> session-active -> available
```

The acceptance-only claim setting was disabled again after the test.
AccessPort storage writes remained disabled throughout.

Physical hot-unplug/reconnect lifecycle acceptance also passed on 2026-07-26.
The adapter observed the disconnect, closed the device cleanly, returned the
application state to `waiting`, then re-enumerated the same AccessPort at high
speed with the 512-byte file endpoints and returned to `available`.

The first separately gated transaction passed later that day:

- exactly one capture-verified 50-byte empty-path `0x1626` request was sent on
  bulk OUT `0x03`;
- no retry was permitted;
- the bounded bulk IN `0x82` response was 438 bytes;
- the response record checksum and `0x1601` opcode validated;
- the strict listing parser consumed the complete payload and found 8 root
  entries; and
- interface `0` released with `ESP_OK`.

No file content, upload, delete, live-data, reset, or disconnect transaction
was sent, and write capability remained disabled. The acceptance flag was
then disabled and the normal descriptor-only firmware was rebuilt, flashed,
and verified on the board. The reusable test overlay is
`sdkconfig.root-list.defaults`, but it is intentionally not part of normal
boots.

The next separately gated, read-only milestone also passed on 2026-07-26.
The adapter issued one `datalog` listing request, assembled its bounded
multi-transfer response, parsed 33 files, and deterministically chose the
smallest safe CSV candidate. It then issued one download request for
`datalog35.csv`, streamed the response through the incremental protocol
decoder into an atomic microSD cache sink, and verified the exact declared
size and SHA-256 before publishing the cache entry:

```text
size: 46568 bytes
sha256: bff4f73f8aef276d354c8277bdc6547433f6f15a164ba41ebe2df1ff736e2688
```

That digest is byte-identical to the established Linux acceptance baseline.
Because the same verified file already existed on the card, the cache sink
deduplicated the incoming temporary file without overwriting the accepted
copy. That early acceptance adapter used a hash-suffixed conflict copy; the
current multi-device adapter supersedes it with full-SHA immutable objects and
an explicit version journal. The transfer used one `0x1620` listing request
and one `0x1621` download request, with no request retries and no upload,
delete, live-data, reset, or disconnect command. Interface `0` released with
`ESP_OK`.

After acceptance, the normal descriptor-only image was rebuilt, flashed, and
boot-verified with `CONFIG_REVLINK_USB_DOWNLOAD_ACCEPTANCE` disabled. The
reusable overlay is `sdkconfig.download.defaults`; it is not part of normal
boots.

## Live incremental file synchronization acceptance

The one-file path was promoted into a bounded, persistent incremental file
sync and accepted on the physical board on 2026-07-26. Datalog acceptance
preceded the map reader; the same read-only scheduler now handles both
collections. The implementation:

- lists the `datalog` and `maps` directories once per session;
- accepts only direct-child `.csv`/`.csv.gz` datalogs and `.ptm` maps;
- orders safe files deterministically by size and then path;
- downloads at most four new files and 16 MiB per USB batch, automatically
  continuing with another clean bounded batch while safe files remain;
- rejects individual files larger than 8 MiB;
- performs no request retries;
- records device path, raw device timestamp, size, SHA-256, and local cache
  name in a 128-entry bounded manifest;
- verifies the cached file's size and SHA-256 before skipping it;
- treats a zero device timestamp conservatively and re-reads the file;
- publishes each file through a `.part` file and exact-size/SHA-256 check;
- requires the true AccessPort serial before selecting storage;
- isolates each device under a serial-derived hashed namespace;
- stores bytes once under their full SHA-256 object name;
- appends every new `(remote path, SHA-256)` version to bounded history; and
- advances the current manifest only after object and history are durable.

FAT does not replace an existing destination with `rename()`. The manifest
therefore uses an owned three-file journal (`current`, `.tmp`, `.bak`) rather
than relying on overwrite rename semantics. An interrupted first attempt left
the previously valid manifest and completed data file intact. The corrected
run loaded that manifest, verified and skipped `datalog35.csv`, deduplicated
the preserved `datalog44.csv`, indexed it, and downloaded three more files:

```text
candidates: 33
downloaded: 4
verified skips: 1
downloaded bytes: 679836
remaining: 28
manifest entries: 5
```

After a hard application reset, the next run loaded all five entries,
SHA-verified and skipped all five files, then downloaded the next four:

```text
candidates: 33
downloaded: 4
verified skips: 5
downloaded bytes: 1225482
remaining: 24
manifest entries: 9
```

Both sessions released interface `0` with `ESP_OK`. No upload, delete,
live-data, reset, or disconnect transaction was sent, and AccessPort write
capability remained disabled. The reusable acceptance overlay is
`sdkconfig.incremental-sync.defaults`; it is disabled in normal builds. The
board was restored to the normal descriptor-only image after the test and
boot-verified with all transaction gates and device writes disabled.

On 2026-07-27 a 33-entry datalog directory exposed a receive-size regression.
The AccessPort delivered the listing as a continuous multi-packet bulk
transfer, while the P4 submitted only a single 512-byte receive and the host
controller correctly reported `USB_TRANSFER_STATUS_OVERFLOW`. The bounded
receiver now uses its existing 16 KiB transfer buffer while retaining the
32 KiB listing cap, 8 MiB per-file cap, and 16 MiB session cap.

Live regression acceptance then passed with the replacement data cable:

```text
identity: AP3-SUB-004 / SUB0406661 / v1.7.6.0-28968
candidates: 33
downloaded: 1
verified skips: 32
downloaded bytes: 6043051
pending: 0
manifest entries: 33
session-close acknowledgement: yes
interface release: ESP_OK
```

`datalog34.csv` was published as an immutable object only after exact-size and
SHA-256 verification. The normal production image was rebuilt with the fix,
reflashed, and boot-verified with auto-sync off, all acceptance gates off,
and device writes disabled.

Read-only map backup passed live acceptance on 2026-07-27. The first combined
session reconciled 34 candidates: all 33 existing datalogs were verified
cache hits and one 67,173-byte PTM map was downloaded, exact-size checked,
SHA-256 verified, and published under the same device namespace. A second
session downloaded zero bytes and verified all 34 objects. After flashing the
ledger-name migration build and resetting the board, the next session again
reported `34 downloaded=0 skipped=34 pending=0 platformError=0`. The portal
projected 33 records with kind `datalog` and one with kind `map`. Auto-sync
remained off, the public write flag remained false, and no upload or delete
route existed. Twenty consecutive full inventory responses retained all 34
records without a reset. A temporary note was saved against the map's
immutable digest, read back through the portal, and cleared, proving the
existing version-scoped annotation path is file-type independent.

## Multi-device and filename-rotation acceptance

The incremental path now reads the four capture-verified identity responses
before listing files. It uses the true device serial—not the placeholder USB
descriptor serial—to select:

```text
/sdcard/revlink/devices/<24-hex-device-key>/
  identity.txt
  annotations.tsv
  current/inventory.manifest
  history/inventory.versions
  objects/datalogs/<full-sha256>.csv[.gz]
  objects/maps/<full-sha256>.ptm
  tmp/
```

Firmware upgrades migrate the earlier `current/datalogs.manifest` and
`history/datalogs.versions` primary ledgers into the unified inventory names
when the new names are absent. The on-disk record format and device namespace
remain unchanged, so already cached datalogs are verified rather than
redownloaded.

Each AccessPort therefore has independent current state, sequence numbers,
objects, and history. Reusing a filename with different bytes appends a new
history version and updates only the current pointer; it never replaces the
old object. Observing identical bytes again is idempotent. The pre-identity
`accessport-acceptance` cache is intentionally left untouched rather than
silently assigned to a device.

On 2026-07-26 the P4 read true identity `AP3-SUB-004 / SUB0406661`, selected
key `d2313ecdf618801b8d5b1270`, verified four existing objects, and downloaded
four new immutable objects. After reset it loaded eight current/history
entries, verified and skipped all eight, then downloaded the next four:

```text
first:  candidates=33 downloaded=4 skipped=4 bytes=1064339 pending=25
reset:  candidates=33 downloaded=4 skipped=8 bytes=1682877 pending=21
```

Both sessions released interface `0` with `ESP_OK`. Writes and deletes stayed
disabled. The board was restored to the normal descriptor-only image and
boot-verified with all transaction gates disabled.

## Attachment-scoped synchronization acceptance

The initial acceptance scheduler was boot-global: after one bounded sync, a
physical reconnect could not start another session until the board rebooted.
That guard is now owned by the enumerated physical attachment instead.
Creating a new USB device record creates one fresh bounded opportunity;
releasing the file session clears the selected storage namespace; and
detaching destroys the record.

The live reconnect test passed on 2026-07-26 without rebooting the P4:

```text
first attachment:  downloaded=4 skipped=16 pending=13 current=20
second attachment: downloaded=4 skipped=20 pending=9  current=24
```

The first session released interface `0` and namespace
`d2313ecdf618801b8d5b1270`. The AccessPort was physically unplugged, the
adapter returned to `waiting`, and the same firmware accepted it at a new USB
address. It re-read true serial `SUB0406661`, reopened the correct namespace,
SHA-verified and skipped all 20 current objects, downloaded the next four,
released interface `0`, and cleared the namespace again. A transient root-port
reset warning during detach did not prevent clean re-enumeration.

Host lifecycle coverage also attaches a second distinct AccessPort identity
after the first detach. The core returns to `waiting`, accepts the replacement,
and never carries the previous device's storage selection into the new
session.

## Normal runtime synchronization acceptance

The event-driven runtime coordinator passed live on the ESP32-P4 on
2026-07-26. A test-only overlay enabled auto-sync at attach while leaving all
legacy acceptance gates and device writes disabled. The normal application
flow transitioned:

```text
device: waiting -> inspecting -> available -> session-active -> available
sync:   idle -> queued -> running -> completed
```

It read true serial `SUB0406661`, selected its isolated namespace, loaded 24
current/history entries, SHA-verified and skipped all 24 cached objects,
downloaded four new immutable objects, then reported:

```text
candidates=33 downloaded=4 skipped=24 bytes=8008571 pending=5
```

Interface `0` and the storage namespace both released cleanly. The board was
then restored to the production image and boot-verified with runtime sync
enabled, auto-sync off, all acceptance gates off, and device writes disabled.
The reusable live-test overlay is `sdkconfig.runtime-sync.defaults`.

Normal runtime control is exposed by `main/revlink_runtime.h`:

- save/enable or disable auto-sync;
- request one manual bounded sync;
- cooperatively cancel queued or active sync; and
- read the current policy and progress snapshot.

The auto-sync preference is the only sync setting stored in NVS. Per-device
current state, immutable objects, and version history remain on microSD.

## Software session close and drive-idle return

The normal AccessPort exit path is application-level and depends on session
context. RevLink must complete the normal initialization and file operation,
send mini subtype `0x05`, receive subtype `0x35`, release interface `0`, and
then accept the device's soft detach and fresh high-speed re-enumeration.

This path passed five consecutive live cycles on 2026-07-27. Each cycle:

- identified `AP3-SUB-004` / `SUB0406661`;
- reconciled 33 datalogs as cache hits;
- sent `0x05` and received `0x35`;
- released interface `0` with `ESP_OK`;
- observed USB removal and a new high-speed device address; and
- automatically started the next cycle without touching D+/D- or VBUS.

After cycle five, the acceptance firmware stopped scheduling work. The board
was then restored to the normal image and boot-verified with runtime sync
enabled, auto-sync off, every acceptance mode off, and device writes disabled.
The reusable test overlay is `sdkconfig.session-cycle.defaults`.

## Guarded map transfer

The normal production profile includes the guarded map-transfer subsystem.
Owner consent defaults off after provisioning or reset, is persisted in NVS
after an explicit Settings change, and remains revocable at any time.
`sdkconfig.map-write.defaults` still provides a focused donor acceptance
overlay pinned to part `AP3-VLK-002`.

On 2026-07-29 the image uploaded
`maps/ZZ_TEST ROUNDTRIP v400.ptm` to donor serial `VLK0207629`. A later
read-only directory listing and download verified 41,469 bytes with SHA-256
`fa08e4ed1569c71fb4faed87c2bf23bee82453d81933602a24277ff37d9b2eaa`.
The final acceptance step is to verify the same listing and digest after a true
AccessPort power cycle. See `../../docs/MAP_WRITE_ACCEPTANCE.md`.

A context-free `0x05`, cleanup after a cold session that never initialized,
and logical root-port shutdown are not equivalent to this sequence. They can
leave the AccessPort displaying `connected to your computer`. Do not add an
unconditional bus reset or `SET_CONFIGURATION(0)` after an acknowledged
close: the P4 already detects the device's own soft detach correctly, and an
extra host-side reset would discard the strongest lifecycle evidence.

The bounded failure path was also accepted live. A test overlay deliberately
omitted the first close after a completed read-only sync. The application
queued one dedicated close recovery, its initialized probe timed out against
the stale session, and its cleanup `0x05` received `0x35`. Interface release,
device removal, and fresh high-speed enumeration all followed without a data
switch or power interruption. The recovery does not list or transfer files,
does not run for a data-phase failure or cancellation, and cannot retry
itself. The reusable overlay is `sdkconfig.close-recovery.defaults`.

The Waveshare board still cannot electrically isolate its USB-A data pair or
VBUS under firmware control. The ordered TS3USB221 and a protected VBUS switch
remain useful optional fault-injection and defense-in-depth hardware, but
neither is required for the accepted normal prototype flow.
