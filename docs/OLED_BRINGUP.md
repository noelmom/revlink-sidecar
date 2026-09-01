# RevLink SH1106 OLED bring-up

This is the reviewed temporary harness for the existing 1.3-inch, seven-pin
SPI SH1106 OLED and the Waveshare ESP32-P4-WIFI6-DEV-KIT.

## Safety

- Disconnect all power before moving jumpers.
- Power the OLED from **3V3**, not 5V.
- Match the printed labels on both boards; do not rely on wire color.
- Leave the microSD card and USB host wiring unchanged.
- If any label differs from the table, stop and verify the module before
  applying power.

## Wiring

| OLED label | ESP32-P4 header label | Function |
| --- | --- | --- |
| `GND` | `GND` | Common ground |
| `VCC` | `3V3` | Display power |
| `CLK` | `23` | SPI clock |
| `MOSI` | `22` | SPI data |
| `RES` | `2` | Display reset |
| `DC` | `20` | Data/command select |
| `CS` | `21` | Chip select |

```text
OLED SH1106                       ESP32-P4 header

 GND  o------------------------------o GND
 VCC  o------------------------------o 3V3
 CLK  o------------------------------o GPIO23
 MOSI o------------------------------o GPIO22
 RES  o------------------------------o GPIO2
 DC   o------------------------------o GPIO20
 CS   o------------------------------o GPIO21
```

The selected GPIOs avoid the accepted microSD bus (`39`–`44`), ESP32-C6 SDIO
link (`14`–`19`), audio signals (`9`–`13`), ESP32-P4 strapping pins
(`34`–`38`), and the USB host peripheral.

## Expected first boot

1. The portal-style RevLink mark animates in.
2. `REVLINK / SYSTEM ONLINE` completes its progress bar.
3. The screen settles on `READY / CONNECT ACCESSPORT`.
4. Attaching an AccessPort changes the display to inspection or ready state.
5. A manual or automatic sync displays deterministic progress.
6. A successful backup ends on `DRIVE READY / BACKUP COMPLETE`.

The display is output-only. It cannot authorize a write, initiate a sync, or
bypass any safety gate.

## Bench build

The overlay is `firmware/esp32p4/sdkconfig.oled.defaults`. It enables runtime
read-only sync, USB root-port power, and the OLED while leaving device writes,
all acceptance tests, and SD formatting disabled.

```sh
source "$HOME/.espressif/frameworks/esp-idf-v6.0.2/export.sh"
idf.py -C firmware/esp32p4 \
  -B firmware/esp32p4/build-oled \
  -D SDKCONFIG=sdkconfig.oled \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.oled.defaults' \
  set-target esp32p4 build
```

## Live acceptance

The complete display path passed on the physical prototype on 2026-07-27:

- the firmware flashed and passed SHA verification;
- the SH1106 initialized on the reviewed SPI pinout;
- the RevLink splash and status layout rendered at the correct orientation
  and usable size;
- the 64 GB microSD remained mounted without formatting;
- the attached AccessPort enumerated at high speed with 512-byte file
  endpoints and drove the display from `inspecting` to `available`; and
- device writes, automatic sync, destructive acceptance tests, and SD
  formatting remained disabled.

The application image occupied about half of the 1 MiB app partition.

## One-run sync demonstration

`firmware/esp32p4/sdkconfig.oled-sync.defaults` combines the accepted display
with the normal bounded read-only runtime sync and the accepted PC-mode exit
path. It forces one sync regardless of saved policy, renders its progress,
performs the acknowledged `0x05`/`0x35` close, and stops the logical USB root
port. It does not enable AccessPort writes, deletes, formatting, or live-data
commands.

This complete visual workflow passed on the physical prototype on 2026-07-27:

```text
device: waiting -> inspecting -> available -> session-active -> available
sync:   queued -> running -> completed
logs:   33 candidates, 33 verified cache hits, 0 downloads, 0 pending
close:  subtype 0x05 sent, subtype 0x35 acknowledged
USB:    software detach observed, logical root port stopped with ESP_OK
```

The normal OLED image was restored afterward with auto-sync disabled and all
acceptance gates and device writes disabled.

## Live UART-driven sync acceptance

The normal OLED bench image also exposes the fixed development control
grammar over the programming USB UART. On 2026-07-27 the board accepted
`status`, then completed a manually requested `sync`, then returned this
terminal snapshot:

```text
status=ok device=available sync=completed auto=off progress=33/33
downloaded=0 skipped=33 pending=0 writes=locked shutdown=no
```

The sync performed the same acknowledged `0x05`/`0x35` close as the one-run
visual demonstration, but kept the normal USB host active. The AccessPort
software-re-enumerated at high speed and returned to the descriptor-ready
`available` state. The console remained quiet while idle after its
nonblocking input loop was accepted.
