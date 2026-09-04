# Firmware update and recovery architecture

RevLink Sidecar uses the ESP32-P4 development board's 16 MiB flash as a
firmware and recovery device. Datalogs, maps, images, manifests, and other
user-owned files remain on microSD.

## Partition contract

| Partition | Offset | Size | Purpose |
|---|---:|---:|---|
| `nvs` | `0x009000` | 24 KiB | Device policy and settings |
| `otadata` | `0x00f000` | 8 KiB | Redundant OTA boot selection |
| `phy_init` | `0x011000` | 4 KiB | Radio PHY initialization |
| `factory` | `0x020000` | 4 MiB | Stable recovery image |
| `ota_0` | `0x420000` | 4 MiB | Normal release slot A |
| `ota_1` | `0x820000` | 4 MiB | Normal release slot B |
| `coredump` | `0xc20000` | 64 KiB | Reserved bounded crash evidence |

The region from `0xc30000` through the end of flash remains unallocated. It is
reserved for the production security and credential design, including any
future encrypted-NVS keys or recovery metadata. It must not become general
application storage.

The coredump partition is reserved by the layout but flash coredumps are not
enabled yet. Before enabling them, define retention, local export, redaction,
and erase behavior so a crash record cannot become an accidental source of
credentials or user data.

## Release lifecycle

1. A factory-programmed board boots the stable `factory` image.
2. A verified update is written only to the inactive OTA slot.
3. The bootloader marks the new slot pending verification and boots it once.
4. The new image runs bounded health checks.
5. Only after the critical services are healthy does the application call
   `esp_ota_mark_app_valid_cancel_rollback()`.
6. A crash, watchdog reset, power loss, or reboot before confirmation causes
   the bootloader to return to the previous working image.

The update transport is not implemented yet. Until it is, firmware is flashed
locally through the development interface. Do not add a browser upload route
that writes arbitrary bytes directly to an application partition.

In the battery enclosure (`hardware/nano-enclosure/battery-enclosure/`) the
Nano's USB-C is concealed, so **flashing happens before assembly**. Reflashing
over USB means lifting the board out of the shell, or rebuilding the shell
with its optional service opening — one more reason the browser update path
matters for that build.

**The order these are built in is load-bearing.** Rollback is already armed —
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` in `sdkconfig.defaults` — while step
5 has no implementation, so nothing in the tree calls
`esp_ota_mark_app_valid_cancel_rollback()`. That is harmless only because
published images flash to `factory`, which is not subject to pending
verification, and `ota_0`/`ota_1` are allocated but unused.

Ship a transport before the health gate and every update boots once and
reverts. The symptom is an update that appears to install and then silently
does not take, with the cause sitting in the bootloader's state machine rather
than in the transport that appears to be at fault. Build the gate first, or
with it. Tracked in #21.

## Required health gate

The platform-neutral decision layer is implemented in
`firmware/components/revlink_update/`. It tracks a bounded post-boot timeout,
reports the first blocking subsystem, accepts explicitly recoverable microSD
absence, treats the display as optional when it is not configured, and rejects
an unsafe/default-allow transfer policy immediately. Its terminal result is
sticky so a late service recovery cannot mark an already timed-out image
healthy.

This component does not select an OTA partition or mark an image valid. The
ESP-IDF adapter that supplies real subsystem observations and calls
`esp_ota_mark_app_valid_cancel_rollback()` remains unimplemented.

An OTA release must not be confirmed merely because `app_main()` started. The
health gate must prove, within a bounded timeout:

- NVS initialized without destructive recovery;
- the microSD repository mounted or reported a clear recoverable absence;
- the status display task is alive when the display is configured;
- the network coordinator reached a valid client, hotspot, or intentional
  offline state;
- the local portal can answer its internal health request;
- the USB runtime is waiting, available, or deliberately disabled rather than
  fault-looping; and
- the shared transfer/write safety policy remains default-deny.

The AccessPort does not need to be physically attached for an update to become
healthy.

The host suite covers delayed readiness, required and optional displays,
recoverable storage absence, timeout, elapsed-time saturation, invalid
observations, and immediate terminal rejection when the safety policy is not
default-deny.

## Recovery and factory reset

Rollback and factory reset are separate operations:

- **Rollback** automatically returns to the previously working OTA image.
- **Recovery** intentionally selects the immutable factory image.
- **Factory reset** may additionally erase selected settings and credentials.

No boot-time factory-reset GPIO is enabled on the development board. The final
gesture must be chosen only after the shutdown, eject, Wi-Fi recovery, and
factory-reset button map and GPIO allocation are approved. A reset must never
erase the microSD dataset silently.

## Security gates before remote updates

Before enabling network-delivered firmware:

- define signed release artifacts and version metadata;
- require cryptographic signature verification before selecting an image;
- use HTTPS or an authenticated local upload channel;
- reject downgrades according to a documented policy;
- preserve power-loss safety and inactive-slot-only writes;
- record the result without logging credentials;
- add a user-visible update and rollback state; and
- complete repeatable update, interruption, corruption, and rollback tests.

Secure Boot, flash encryption, anti-rollback eFuses, and encrypted NVS are
production provisioning decisions. Do not burn irreversible eFuses during
prototype acceptance.

## Prototype acceptance

The layout was flashed to the Waveshare ESP32-P4 board and boot-accepted on
2026-07-27. The bootloader selected the factory image at `0x20000`; the
1.07 MB image left 74% of each 4 MiB application slot free. The existing
63.8 GB microSD mounted without formatting, the fallback hotspot returned,
the high-speed USB host initialized, and the attached AccessPort reported
available. Auto-sync remained off and AccessPort writes remained locked.
