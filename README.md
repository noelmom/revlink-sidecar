# RevLink Sidecar

**Use your AccessPort without a laptop.**

The COBB AccessPort stores your datalogs and maps over USB, but it only talks
to a Windows PC or a Mac. That means the moment you want to pull a log at the
track, at a meet, or in your own driveway, you need to go find a computer.

RevLink Sidecar is an ESP32-P4 that sits between the AccessPort and your
phone. Plug the AccessPort into the Sidecar, open a web page on your phone,
and your datalogs and maps are there — listed, downloadable, and graphable,
with no PC, no cloud account, and no internet connection.

Everything runs on the device. Your files land on a microSD card you own.

```
  AccessPort ──USB-A host──> ESP32-P4 Sidecar ──Wi-Fi──> your phone's browser
                                    │
                                 microSD
```

---

## What it does today

- **Automatic sync on attach.** Plug in the AccessPort and new datalogs and
  maps copy to microSD incrementally — only what changed.
- **Local web portal.** Served from the device itself. Joins your home or shop
  Wi-Fi when it can, and falls back to its own `RevLink-<id>` hotspot with a
  captive portal when it can't.
- **Datalog viewer.** Single and split views, numeric Y axes, searchable
  channels, reusable presets (Air/Fuel, Boost, Knock, Timing, Thermal),
  pointer inspection with exact values.
- **Vehicle Health.** A deterministic, local, rule-based evidence pass over
  logs you have already synced. No AI, no phone-home — it shows you the
  evidence it used.
- **Multi-device.** Keeps separate datasets per AccessPort, so a shop can
  service several cars without mixing files.
- **Stage a map without the device.** Save a `.ptm` while the AccessPort is
  unplugged; it is pinned to that car and survives a power cycle. Transfer it
  by hand when the device is next connected, or let it apply automatically
  after the next sync — that part is off until you turn it on.
- **Direct download and share.** Pull individual CSVs, or hand them to your
  phone's native share sheet.
- **OLED status.** Optional 1.3" SH1106 shows attach, sync, and network state
  at a glance.
- **Offline by design.** No account, no telemetry, no cloud dependency.

## What it deliberately does not do

This project reads and writes *files*. It is not a tuning tool.

- **No ECU flashing.** Not now, not planned.
- **No real-time tuning or live ECU writes.**
- **Map writes arrive locked** and stay off until you enable them. Copying a
  `.ptm` onto the AccessPort's storage is not the same as installing it — that
  remains a deliberate action on the AccessPort itself.
- **File deletion arrives locked too**, behind a *separate* switch from map
  writes — agreeing to one is never agreeing to the other. Only files directly
  inside `maps/` and `datalog/`, and there is no undo. See
  [SAFETY.md](SAFETY.md).
- **No public internet exposure.** The portal has no transport encryption and
  is meant for a trusted local network.

## Using it

One button does everything. On the Waveshare board it is `BOOT`; on the printed
case it is the left side button.

| Gesture | What it does |
| --- | --- |
| Double-press | Shows how to reach the portal — a Wi-Fi QR code when the Sidecar is running its own hotspot, or its `revlink-<id>.local` address when it has joined your network |
| Press again | Dismisses it |
| Hold 2 seconds | Safe shutdown: cancels work, unmounts the card, sleeps. `RST` or a power cycle wakes it |
| Two double-presses within 20 s | Authorises formatting a card that responded but would not mount — see [POWER_MANAGEMENT.md](docs/POWER_MANAGEMENT.md) |

The display gestures need the optional OLED. Everything else works without one.

## Hardware

| Part | Notes |
| --- | --- |
| Waveshare **ESP32-P4-NANO** | Primary target. USB-A OTG high-speed host, 16 MB flash, 32 MB PSRAM. [Buy][aff-nano] |
| Waveshare **ESP32-P4-WIFI6-DEV-KIT** (SKU 32054) | Supported, but build it yourself — no image is published for it. P4 revision 3.x silicon vs the Nano's 1.x, and the binaries are not interchangeable. |
| microSD card | Stores all synced files. Any capacity. [Buy][aff-sd] |
| 1.3" SH1106 OLED (optional) | SPI, not I2C. Status display. [Buy][aff-oled] |
| USB-C power bank | The reference build runs off an Anker Nano ([buy][aff-bank]). The Sidecar powers the AccessPort as a USB host, so give it a real 5 V supply — never source VBUS from a GPIO. |
| 3D-printed enclosure | [`hardware/nano-enclosure/`](hardware/nano-enclosure/) |

[aff-nano]: https://amzn.to/462r0q8
[aff-sd]: https://amzn.to/4h381C1
[aff-oled]: https://amzn.to/46BiJJQ
[aff-bank]: https://amzn.to/4gxIFfp

> **Affiliate disclosure.** The **Buy** links above are Amazon affiliate links.
> As an Amazon Associate I earn from qualifying purchases, at no extra cost to
> you. They point at the exact parts this was built and tested on, which is why
> they are here rather than a generic search. Buy them anywhere you like —
> nothing in this project depends on where the hardware came from.

**Why ESP32-P4 and not an S3?** The S3's native USB host is full-speed only,
and the AccessPort's 512-byte bulk endpoints require high speed. The S3 was
tried and retired — the full write-up is in
[`docs/ESP32S3_USB_POWER_BRINGUP.md`](docs/ESP32S3_USB_POWER_BRINGUP.md).

> [!NOTE]
> **Enclosure status.** This geometry has been printed and assembled — it is
> the case in the photos on [revlinkgarage.com](https://revlinkgarage.com/).
> The lid conceals the Ethernet jack, closes the
> donor case's switch slot and spare USB-A opening, and adds the centered OLED
> window plus two guided side buttons for RST and BOOT.
>
> One known refinement remains: the lid has **no internal mounting posts for
> the OLED**, so the display is retained by the window edge and its wiring.
> Adding four posts is a good first contribution — see
> [`hardware/nano-enclosure/tools/`](hardware/nano-enclosure/tools/) for a
> dependency-free way to verify a revision actually changed what you think it
> changed.
>
> **Battery build.** A separate, lower enclosure for the Nano plus the
> Adafruit 6106 charger and an LP103454 cell — 120 × 59 × 23.6 mm — is in
> [`hardware/nano-enclosure/battery-enclosure/`](hardware/nano-enclosure/battery-enclosure/).
> It conceals the Nano's USB-C, so **flash before assembly**. Its geometry is
> not print-ready until the charger board has been measured (#23).

## Install

**Easiest — flash from your browser:
[revlinkgarage.com/flash](https://revlinkgarage.com/flash/)**

Open it in Chrome or Edge, plug the board in over USB-C, and click Flash. No
toolchain, no command line. It reads the board's silicon revision first and refuses to write an
image built for the other P4 variant.

The published image can read your AccessPort and, once you turn it on, copy a
`.ptm` map file back onto it. Writing arrives **locked**: you enable it in
Settings, only `maps/*.ptm` is accepted, and every write is read back and
checked against its SHA-256.

It can also delete files from `maps/` and `datalog/`, so a full AccessPort can
be cleared without a computer. That is a second switch, locked by default and
separate from writes, with no undo.

See [`web/flash/`](web/flash/) to host the page yourself.

## Build

Requires **ESP-IDF v6.0.2**, pinned in
[`firmware/esp32p4/idf-version.txt`](firmware/esp32p4/idf-version.txt).

```bash
source "$HOME/.espressif/frameworks/esp-idf-v6.0.2/export.sh"
```

Nano (P4 revision 1.x) — build in its own directory:

```bash
NANO_BUILD="$PWD/firmware/esp32p4/build-nano"
idf.py -C firmware/esp32p4 -B "$NANO_BUILD" \
  -D "SDKCONFIG=$NANO_BUILD/sdkconfig" \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.oled.defaults;sdkconfig.wifi-scan.defaults;sdkconfig.wifi-join.defaults;sdkconfig.network-runtime.defaults;sdkconfig.onboarding.defaults;sdkconfig.nano.defaults' \
  set-target esp32p4 build
```

Do not flash a Dev Kit image onto a Nano. Full build and flash notes:
[`firmware/esp32p4/README.md`](firmware/esp32p4/README.md).

## Tests

The protocol, sync, storage, network, power, and safety logic are all built as
plain C11 with no ESP-IDF dependency, so they run on your laptop:

```bash
./scripts/ci-local.sh
```

That runs 19 host suites, 7 Node-based portal tests, the web flasher tests,
and the enclosure geometry baseline — about 20 seconds, and it needs only a C
compiler. If you don't have CMake, the script compiles the tests directly with
the same flags.

Add `--full` to also build the firmware three ways: writes enabled, deletion
compiled in, and writes compiled out. Building every configuration is what
catches code that only compiles under one of them. That needs ESP-IDF v6.0.2
plus CMake and Ninja.

```bash
./scripts/ci-local.sh --full
```

## Repository layout

```
firmware/
  esp32p4/          ESP32-P4 application, portal, storage, Kconfig, sdkconfig overlays
  components/       Platform-neutral C components (protocol, sync, network, status, …)
  test/             Host-side test suites — no hardware required
hardware/
  nano-enclosure/   3D-printable case: STL/3MF, source geometry, print notes
docs/               Architecture, safety model, networking, storage, bring-up
```

## Documentation

| Document | What it covers |
| --- | --- |
| [FIRMWARE_ARCHITECTURE.md](docs/FIRMWARE_ARCHITECTURE.md) | Component boundaries, device lifecycle, stable contracts |
| [SINGLE_ACCESSPORT_SAFETY.md](docs/SINGLE_ACCESSPORT_SAFETY.md) | Why two attached AccessPorts fail closed instead of guessing |
| [MAP_WRITE_ACCEPTANCE.md](docs/MAP_WRITE_ACCEPTANCE.md) | The gated round-trip procedure for map writes |
| [FILE_DELETE.md](docs/FILE_DELETE.md) | Removing a file from the AccessPort, and why it is gated apart from writes |
| [ACCEPTANCE_LOG.md](docs/ACCEPTANCE_LOG.md) | Dated bench records for the ESP32-P4 target. History, not current state |
| [FIRMWARE_UPDATE.md](docs/FIRMWARE_UPDATE.md) | How firmware gets onto the board, and why there is no OTA |
| [POWER_MANAGEMENT.md](docs/POWER_MANAGEMENT.md) | BOOT-button behaviour, safe shutdown, and deep sleep |
| [WIFI_BRINGUP.md](docs/WIFI_BRINGUP.md) | The ESP32-C6 radio link over SDIO, and what it took to bring up |
| [OLED_BRINGUP.md](docs/OLED_BRINGUP.md) | SH1106 wiring and the status screens |
| [ACCESSPORT_TIME_METADATA.md](docs/ACCESSPORT_TIME_METADATA.md) | What the AccessPort's timestamps mean, and what RevLink records instead |
| [ESP32S3_USB_POWER_BRINGUP.md](docs/ESP32S3_USB_POWER_BRINGUP.md) | Why the ESP32-S3 was retired: full-speed USB cannot do 512-byte bulk |
| [ESP32P4_DEMO.md](docs/ESP32P4_DEMO.md) | Runbook for demonstrating the Sidecar |
| [OPERATIONAL_LOGGING.md](docs/OPERATIONAL_LOGGING.md) | What is logged, what is not, and the two audit logs that ship |
| [STAGED_MAPS.md](docs/STAGED_MAPS.md) | Uploading a map with no AccessPort attached, and applying it on the next sync |
| [PRODUCT_NETWORKING.md](docs/PRODUCT_NETWORKING.md) | Client-first Wi-Fi with fallback hotspot and captive portal |
| [ACCESSPORT_STORAGE_BEHAVIOR.md](docs/ACCESSPORT_STORAGE_BEHAVIOR.md) | How the device's filesystem actually behaves |
| [SUPPORTED_ACCESSPORTS.md](docs/SUPPORTED_ACCESSPORTS.md) | Which hardware is verified, and how families are identified |
| [ACCESSPORT_CATALOG.md](docs/ACCESSPORT_CATALOG.md) | The part-number table: what it is, where it comes from, how to add to it |
| [VEHICLE_HEALTH.md](docs/VEHICLE_HEALTH.md) | The deterministic diagnostic layer |
| [SIDECAR_SD_BACKUP.md](docs/SIDECAR_SD_BACKUP.md) | Backing up and restoring the card |

## Status

Actively developed, and in regular use on a real car. Read-only sync (listing,
incremental download, checksum verification, atomic microSD publication) has
been accepted on real hardware, and so has writing: maps have been transferred
to an attached AccessPort many times, each verified by read-back, including
unattended transfers after a sync. Deletion is accepted too.

All three ship in the published image. Writes and deletes remain **gated** —
not because the paths are unproven, but because they are the owner's decision
to make. See [SAFETY.md](SAFETY.md) for exactly what is and is not proven.

## Contributing

Issues and PRs welcome — especially enclosure revisions, additional AccessPort
families, and portal UX. Please read [CONTRIBUTING.md](CONTRIBUTING.md) and
[SAFETY.md](SAFETY.md) first; this project touches hardware people depend on,
and the safety gates are not incidental.

## License

- Code (`firmware/`, scripts): **Apache-2.0** — [LICENSE](LICENSE)
- Hardware (`hardware/`): **CC BY-SA 4.0** — [LICENSE-hardware](LICENSE-hardware)

## Legal

RevLink Sidecar is an independent, unofficial project. It is not affiliated
with, endorsed by, or sponsored by COBB Tuning. "COBB" and "AccessPORT" are
trademarks of their respective owners and are used here only to identify the
hardware this project interoperates with. See [NOTICE](NOTICE).

Modifying your vehicle may affect its warranty, emissions compliance, and
legal road status in your jurisdiction. That is your responsibility. This
software is provided without warranty of any kind.
