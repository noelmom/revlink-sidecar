# RevLink SH1106 OLED bring-up

This is the reviewed harness for the existing 1.3-inch, seven-pin SPI SH1106
OLED and the Waveshare **ESP32-P4-NANO**, which is the board the published
firmware targets.

Header pin numbers below are for that board. The ESP32-P4-WIFI6-DEV-KIT places
the same GPIOs on different header positions, so do not carry these pin numbers
across to it.

## Safety

- Disconnect all power before moving jumpers.
- Power the OLED from **3V3**, not 5V.
- Match the printed labels on both boards; do not rely on wire color.
- Leave the microSD card and USB host wiring unchanged.
- If any label differs from the table, stop and verify the module before
  applying power.

## Wiring

`P1` is the 2x13 header away from RESET, `P2` the one nearest it. Pin 1 is at
the top of both, and odd pins are the column nearest the board edge.

| OLED label | GPIO | Header pin | Function |
| --- | --- | --- | --- |
| `GND` | — | `P1` 9 | Common ground |
| `VCC` | — | `P1` 17 | Display power, 3V3 |
| `CLK` | `23` | `P1` 7 | SPI clock |
| `MOSI` | `22` | `P1` 16 | SPI data |
| `RES` | `2` | **`P2` 11** | Display reset |
| `DC` | `20` | `P1` 13 | Data/command select |
| `CS` | `21` | `P1` 15 | Chip select |

**`RES` is the only wire on `P2`.** Six of the seven land on `P1`, so a display
that stays dark after a reconnection is worth checking there first.

Ground and 3V3 are the two rows with a choice. `P1` carries ground on pins 6, 9,
14, 20 and 25, and 3V3 on pins 1 and 17; the table picks 9 and 17 to leave pins
1, 3, 5 and 6 contiguous for the fuel gauge in #17, which needs 3V3, `SDA`,
`SCL` and a ground of its own.

```text
OLED SH1106                       ESP32-P4 header

 GND  o------------------------------o GND      P1 pin 9
 VCC  o------------------------------o 3V3      P1 pin 17
 CLK  o------------------------------o GPIO23   P1 pin 7
 MOSI o------------------------------o GPIO22   P1 pin 16
 RES  o------------------------------o GPIO2    P2 pin 11
 DC   o------------------------------o GPIO20   P1 pin 13
 CS   o------------------------------o GPIO21   P1 pin 15
```

The selected GPIOs avoid the accepted microSD bus (`39`–`44`), ESP32-C6 SDIO
link (`14`–`19`), audio signals (`9`–`13`), ESP32-P4 strapping pins
(`34`–`38`), and the USB host peripheral.

## Expected first boot

1. The portal-style RevLink mark animates in beside the `REVLINK` wordmark,
   with the running firmware version below it as `V0.2.5` and a progress bar
   filling underneath. It holds for 1.5 s.
2. With no AccessPort attached the screen settles on
   `NO DEVICE / ACCESSPORT OFFLINE / CONNECT TO SYNC`.
3. Attaching one moves it to `CHECKING / ACCESSPORT / USB HIGH SPEED`, then to
   the ready screen, which shows the vehicle and part number rather than fixed
   text.
4. A sync shows `SYNCING / BACKING UP LOGS / KEEP CONNECTED` with progress.
5. A finished sync ends on `BACKUP COMPLETE / SAFE TO DISCONNECT`.

The display is output-only. It cannot authorize a write, initiate a sync, or
bypass any safety gate.

## Every screen

The status screens share a layout: a header that alternates between `REVLINK`
and `SIDECAR`, a Wi-Fi icon and network badge top-right when a network is up,
then a headline, a detail line and a footer. The headline drops from the large
font to the small one automatically if it is too wide.

The badge says where the portal is: `LOCAL` while the Sidecar is running its
own hotspot, the network's SSID once it has joined one, or `WIFI` if the SSID
is unavailable. A long SSID is truncated to fit.

| State | Headline | Detail | Footer |
| --- | --- | --- | --- |
| Booting | `STARTING` | `SYSTEM CHECK` | `REVLINK` |
| No device | `NO DEVICE` | `ACCESSPORT OFFLINE` | `CONNECT TO SYNC` |
| Inspecting | `CHECKING` | `ACCESSPORT` | `USB HIGH SPEED` |
| Ready | vehicle, or `DEVICE CONNECTED` | part number | `READY TO SYNC` |
| Sync queued | `SYNC QUEUED` | `PREPARING BACKUP` | `PLEASE WAIT` |
| Syncing | `SYNCING` | `BACKING UP LOGS` | `KEEP CONNECTED` |
| Session open | `CONNECTED` | `SESSION ACTIVE` | `KEEP CONNECTED` |
| Cancelling | `FINISHING` | `CLOSING SAFELY` | `KEEP CONNECTED` |
| Recovering | `RECOVERING` | `SAFE USB CLOSE` | `KEEP CONNECTED` |
| Complete | vehicle, or `SYNC COMPLETE` | `BACKUP COMPLETE` | `SAFE TO DISCONNECT` |
| Wi-Fi dropped | `WIFI LOST` | `RETRY <n> SEC`, or `RETRYING` with no countdown | `HOTSPOT NEXT` |
| USB fault | `ATTENTION` | `USB NEEDS CHECK` | `OPEN REVLINK` |
| Sync fault | `ATTENTION` | `SYNC NEEDS CHECK` | `OPEN REVLINK` |
| Two AccessPorts | `MULTIPLE DEVICES` | `UNPLUG ALL DEVICES` | `THEN RECONNECT ONE` |
| Unknown | `ATTENTION` | `STATUS UNKNOWN` | `OPEN REVLINK` |

Four screens replace that layout entirely rather than sitting inside it:

| Screen | Shown when | Contents |
| --- | --- | --- |
| Hotspot details | The fallback hotspot comes up | `REVLINK SETUP`, the SSID, `PASSWORD`, the password |
| Wi-Fi QR | BOOT double-press while the hotspot is up | A scannable `WIFI:T:WPA;…` join code |
| Local address | BOOT double-press while joined to a network | `OPEN IN BROWSER`, the hostname, `.local`, `PRESS TO CLOSE` |
| Storage recovery | The card is missing or will not mount | See below |

## Storage recovery screens

These are the only screens that ask for input, and they are deliberately
awkward because the outcome is destructive.

| Step | Screen |
| --- | --- |
| No card | `SD CARD MISSING` / `INSERT A CARD` / `THEN RESTART` |
| Card unreadable | `SD UNREADABLE` / `DOUBLE-PRESS BOOT` / `TO FORMAT` |
| Awaiting confirmation | `ERASE SD CARD?` / `ALL DATA WILL BE LOST` / `DOUBLE-PRESS BOOT` / `CONFIRM IN <n> SEC` |
| Formatting | `FORMATTING SD` / `DO NOT POWER OFF` |
| Done | `FORMAT COMPLETE` / `STORAGE READY` / `RESTARTING` |
| Failed | `FORMAT FAILED` / `CHECK SD CARD` / `THEN TRY AGAIN` |
| Other error | `STORAGE ERROR` / `CHECK SD CARD` / `THEN RESTART` |

Formatting requires two separate double-presses, the second within the
countdown. One accidental double-press cannot erase a card.

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

The application image was about 1.34 MB in a 4 MiB app slot.

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
