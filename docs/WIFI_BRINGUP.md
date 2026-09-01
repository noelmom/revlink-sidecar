# ESP32-C6 radio coexistence bring-up

This document records the accepted onboard-radio profile for the Waveshare
ESP32-P4-WIFI6-DEV-KIT. It is a hardware acceptance profile, not yet the
shipping network manager.

## Resource ownership

| Subsystem | Controller | Signals |
| --- | --- | --- |
| ESP32-C6 Wi-Fi/BLE coprocessor | P4 SD/MMC host, 4-bit SDIO | CLK 18, CMD 19, D0-D3 14-17, reset 54 |
| microSD | SPI3 | CLK 43, MOSI/CMD 44, MISO/D0 39, CS/D3 42 |
| SH1106 OLED | SPI2 | CLK 23, MOSI 22, CS 21, D/C 20, reset 2 |
| AccessPort | USB 2.0 high-speed host | USB-A OTG host port |

The ESP32-P4 has one SD/MMC controller. The C6 and native four-bit microSD
cannot own it simultaneously. The radio profile therefore keeps the radio on
its board-routed SDIO link and moves only the removable card to SPI3. Storage
layout, device namespaces, immutable objects, manifests, and sync behavior do
not change.

## Build profiles

The default profile explicitly disables ESP-Hosted and remote Wi-Fi:

```text
# CONFIG_ESP_WIFI_REMOTE_ENABLED is not set
# CONFIG_ESP_HOSTED_ENABLED is not set
# CONFIG_REVLINK_WIFI_SCAN_ACCEPTANCE is not set
# CONFIG_REVLINK_SD_SPI_TRANSPORT is not set
```

The bounded radio profile is selected only through
`sdkconfig.wifi-scan.defaults`:

```text
CONFIG_REVLINK_WIFI_SCAN_ACCEPTANCE=y
CONFIG_REVLINK_SD_SPI_TRANSPORT=y
CONFIG_ESP_WIFI_REMOTE_ENABLED=y
CONFIG_ESP_HOSTED_ENABLED=y
```

The integrated coordinator acceptance profile layers
`sdkconfig.wifi-join.defaults` and
`sdkconfig.network-runtime.defaults` on top. It enables the RAM-only station
adapter, fallback SoftAP, development console, OLED, SPI3 microSD, USB host,
and runtime sync while keeping AccessPort writes disabled:

```sh
idf.py -C firmware/esp32p4 \
  -B firmware/esp32p4/build-network-runtime \
  -D "SDKCONFIG=$PWD/firmware/esp32p4/build-network-runtime/sdkconfig" \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.oled.defaults;sdkconfig.wifi-scan.defaults;sdkconfig.wifi-join.defaults;sdkconfig.network-runtime.defaults' \
  build
```

Build both profiles when changing storage, radio, or managed-component
dependencies:

```sh
source "$HOME/.espressif/frameworks/esp-idf-v6.0.2/export.sh"

idf.py -C firmware/esp32p4 \
  -B build-radio-off \
  -D SDKCONFIG="$PWD/build-radio-off/sdkconfig" \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.oled.defaults' \
  reconfigure build

idf.py -C firmware/esp32p4 \
  -B build-wifi-scan \
  -D SDKCONFIG=sdkconfig.wifi-scan \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.oled.defaults;sdkconfig.wifi-scan.defaults' \
  reconfigure build
```

The radio-off binary must contain no C6 scan or ESP-Hosted runtime strings.
The radio-on configuration must show all four profile options enabled.

## Live acceptance

The combined profile passed on the physical board on 2026-07-27:

1. A true board power cycle allowed the existing card to enter SPI mode.
2. The C6 initialized over 4-bit SDIO at 40 MHz with 512-byte blocks.
3. The 64 GB microSD mounted over SPI3 without formatting and retained the
   existing acceptance marker and cached dataset.
4. A bounded anonymous scan found six access points. Only count, strongest
   RSSI, and channel were retained; no identities or credentials were logged.
5. The AccessPort enumerated at USB high speed with 512-byte file endpoints.
6. A read-only incremental sync found 33 candidates, verified 33 cache hits,
   downloaded zero files, and left zero pending.
7. The polite `0x05` close received `0x35`; the AccessPort detached,
   re-enumerated, and returned to available.
8. Writes, deletes, formatting, and automatic sync remained disabled.

## Ephemeral station-join acceptance

The controlled connection profile passed live on 2026-07-27:

- the SSID and password entered through the local UART console were never
  echoed or logged;
- `esp_wifi_set_storage(WIFI_STORAGE_RAM)` kept station configuration out of
  NVS and microSD;
- the ESP32-C6 associated with the selected development network;
- DHCP completed;
- a fixed `example.com` DNS lookup completed, proving bidirectional IP
  traffic through the hosted radio; and
- AccessPort writes remained uncompiled and automatic sync remained off.

The successful terminal result was:

```text
C6 NETWORK ACCEPTANCE PASSED: association, DHCP, and DNS traffic
wifi acceptance: PASS (association + DHCP + DNS)
```

This test uses a separate `wifi-join` profile and the local-only
`wifi-join` console command:

```sh
source "$HOME/.espressif/frameworks/esp-idf-v6.0.2/export.sh"
idf.py -C firmware/esp32p4 \
  -B build-wifi-join \
  -D SDKCONFIG=sdkconfig.wifi-join \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.oled.defaults;sdkconfig.wifi-scan.defaults;sdkconfig.wifi-join.defaults' \
  set-target esp32p4 build
```

ESP-Hosted still reports `Host [2.12.0] > Co-proc [0.0.0]`, but the actual
association, DHCP, and DNS path passed. Do not update the C6 solely to remove
that warning; revisit its firmware only if a required RPC fails reproducibly.

## Coordinator and fallback-hotspot acceptance

The first integrated product-network slice passed live on 2026-07-27:

- the platform-neutral coordinator started above the P4/C6 radio adapter;
- with no RAM-only station record, it selected fallback mode without scanning
  continuously;
- it waited in `hotspot-starting` until an explicit local credential was
  supplied;
- credential input was hidden, remained in RAM, and was excluded from logs
  and public snapshots;
- the C6 started a WPA2 SoftAP named
  `RevLink-<last six AP-MAC digits>` on channel 6;
- the default ESP-IDF AP netif started its private DHCP service;
- the coordinator reached `hotspot-ready`; and
- the 64 GB microSD, OLED, USB host, and attached AccessPort remained active,
  with device writes disabled.

The development console adds these local-only acceptance commands:

```text
wifi-status
wifi-hotspot
wifi-fallback
wifi-join
```

`wifi-status` deliberately exposes states, configuration flags, client count,
transfer lock, and error code without exposing a saved network identity or
credential. Credentials are not retained across reset in this profile.

## Reset caveat

SPI mode is selected during card initialization. A CPU reset can leave the
removable card powered in its previous native SD mode, and the on-chip LDO
software cycle did not reliably reset it on this development board. When
switching an already-running card between native SD and SPI profiles, use a
true board power cycle. Normal boots within one profile do not change the
filesystem or format the card.

## Remaining work

- Exercise longer-lived traffic before enabling a persistent client or
  access point.
- Live-accept the integrated coordinator's station selection, sticky
  reconnect, wrong-password fallback, link-loss fallback, explicit retry,
  force-hotspot, and transfer-lock failure matrix.
- Add credential provisioning only behind an authenticated local control
  boundary; never log SSIDs, BSSIDs, or passwords.
- Implement and accept the onboarding HTTP service, captive DNS behavior,
  mDNS, encrypted persistent credential store, and first-boot/reset flow.
- Add BLE only as a separate authenticated adapter and acceptance gate. Its
  intended scope is discovery, provisioning, connectivity recovery, and
  compact status; all file transfer remains on Wi-Fi.
