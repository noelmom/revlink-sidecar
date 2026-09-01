# Web flasher

A static page that flashes a RevLink Sidecar release over Web Serial. No
build step — plain HTML, CSS, and ES modules. Serve the directory over HTTPS
(or `localhost`) and it works.

```bash
python3 -m http.server 8777 --directory web/flash
```

It is self-contained and origin-relative, so the same directory can be
published on GitHub Pages, dropped onto revlinkgarage.com, or embedded in an
iframe.

## Layout

```
index.html                      markup and copy
style.css                       styling, light and dark
flash.js                        Web Serial + esptool-js driver
gate.js                         board-compatibility gate (no browser deps)
gate.test.mjs                   tests for the gate and the release manifest
vendor/esptool-js-0.6.1.js      vendored flashing library (Apache-2.0)
firmware/<release>/             binaries + manifest.json + SHA256SUMS
```

## Why the library is vendored

`esptool-js` is committed here rather than loaded from a CDN. This page writes
to people's hardware; a CDN that can substitute the script can substitute what
gets written. Vendoring also means the page keeps working offline and when a
CDN is blocked.

Upgrading it is deliberate: replace the file, bump the import in `flash.js`,
record the new SHA-256 below, and re-test against a real board.

```
esptool-js 0.6.1
sha256  ef7d5a237d3f273ecf546bcee65dddad90bd82cf02f22a980d1537e0cd79a152
```

## The board gate

The ESP32-P4-NANO and the ESP32-P4-WIFI6-DEV-KIT both report as `ESP32-P4`
but use pre-v3 and v3+ silicon, which ESP-IDF treats as mutually incompatible
firmware targets. `gate.js` reads the silicon revision and **refuses** a
mismatch before writing anything.

It fails closed. An unreadable revision, an unexpected chip family, or a
missing manifest field all refuse; only a positive match enables the button.
`gate.test.mjs` covers each case, and runs in `scripts/ci-local.sh`.

## Integrity

Every part is fetched, size-checked, and SHA-256 verified against
`manifest.json` before a byte reaches the board. A truncated download or a
tampered mirror fails before flashing, not halfway through it.

`gate.test.mjs` additionally verifies that the published binaries match their
recorded digests, that flash regions do not overlap, and that the published
image really is the read-only build — by asserting the write-path log strings
are absent from the binary, rather than trusting the manifest's own flag.

## Cutting a new release

```bash
# 1. Build the profile you intend to publish.
source "$HOME/.espressif/frameworks/esp-idf-v6.0.2/export.sh"
B="$PWD/build-release/nano-readonly"
D='sdkconfig.defaults;sdkconfig.oled.defaults;sdkconfig.wifi-scan.defaults;sdkconfig.wifi-join.defaults;sdkconfig.network-runtime.defaults;sdkconfig.onboarding.defaults;sdkconfig.nano.defaults;sdkconfig.release-readonly.defaults'
idf.py -C firmware/esp32p4 -B "$B" \
  -D "SDKCONFIG=$B/sdkconfig" -D "SDKCONFIG_DEFAULTS=$D" \
  set-target esp32p4 build

# 2. Copy the four artefacts into web/flash/firmware/<version>-<profile>/
#    as bootloader.bin, partition-table.bin, ota-data-initial.bin,
#    and revlink-sidecar.bin.
# 3. Regenerate manifest.json (offsets come from the build's flash_args)
#    and SHA256SUMS.
# 4. Point RELEASE in flash.js at the new directory.
# 5. Run ./scripts/ci-local.sh — the manifest and binary assertions must pass.
# 6. Flash a real board before publishing.
```

Offsets for the current partition table:

| Part | Offset |
| --- | --- |
| `bootloader.bin` | `0x2000` |
| `partition-table.bin` | `0x8000` |
| `ota-data-initial.bin` | `0xF000` |
| `revlink-sidecar.bin` | `0x20000` |

## Browser support

Web Serial is Chromium-only — Chrome, Edge, Opera, Arc, Brave. Safari and
Firefox do not implement it, and the page says so plainly instead of failing
mysteriously.

## Not yet verified

The Nano's programming console runs through an onboard WCH USB-serial bridge.
Whether that bridge enumerates without a vendor driver on a **clean** macOS
and a **clean** Windows install has not been tested. If a driver turns out to
be required, that needs to be stated prominently on this page before the
project is announced anywhere.
