# RevLink power management

## Switched USB as the vehicle-on signal

The preferred prototype input is a USB source that is powered only while the
vehicle is in ACC or RUN, such as a switched radio or console USB output.
RevLink must not rely on that input as its only supply. A battery and
power-path controller keep the system alive when external USB disappears.

Use the power-path controller's USB-present or power-good output as the
firmware signal. If the selected controller does not expose one, add a
protected 5 V presence detector whose output is safe for a 3.3 V ESP32-P4
low-power GPIO. Never connect USB VBUS directly to a GPIO.

```text
switched vehicle USB
    -> protected charger and power-path controller
       -> system rail
       -> battery charging
       -> USB-present / power-good -> ESP32-P4 low-power wake GPIO
```

The controller must support simultaneous charging and system load, seamless
handoff to the battery, reverse-current protection, and enough peak current
for the P4, C6 radio, microSD, and AccessPort USB host path.

## Measured draw

Measured 2026-09-02 with an inline USB meter on the 5 V rail feeding the Nano's
USB-C port, with an AccessPort attached and connecting, and during a sync:

```text
4.86 V    0.12 – 0.16 A    0.58 – 0.77 W
```

That is the whole system: ESP32-P4 with 32 MB PSRAM, the ESP32-C6 radio over
SDIO, microSD, and the AccessPort as a downstream USB device. It is far lower
than the figure this section was written in anticipation of, and it changes
what "enough peak current" has to mean — a 1 A supply has roughly six times the
headroom needed for steady-state operation.

For scale, on a 3.7 V 2000 mAh (7.4 Wh) cell through a boost at ~88%
efficiency, that draw is about **0.12C** and gives roughly **8–10 hours**.

Two things this measurement does **not** cover, and neither should be inferred
from it:

- **Cold-boot inrush.** This was taken at steady state. Bringing up PSRAM and
  the radio from cold is a transient an averaged USB meter will not show, and
  it is the figure that decides whether a given boost converter can start the
  board at all. Some boost converters stall on a load that draws full current
  immediately; the Adafruit TPS61023 board is documented as one of them, above
  roughly 200 mA.
- **A map write or delete under way.** Those hold the USB host busy in a
  different pattern to a read sync and have not been measured separately.

### Reference parts

A combination confirmed to power the Nano, tracked in issue #11:

| | |
| --- | --- |
| Charger, power path and boost | Adafruit 6106 — bq25185 with a TPS61023, 5 V at 1 A max |
| Cell | EEMB LP103454, 3.7 V 2000 mAh, 54 × 34 × 10.3 mm |

Take care ordering: Adafruit also sells a bq25185 board with a **3.3 V buck**
instead of the 5 V boost. That variant cannot power the Nano and cannot supply
USB-compliant VBUS to the AccessPort.

The cell does not fit the current 55 × 55 mm enclosure — it is 54 mm long, so
the interior is short by about 3 mm at 2 mm walls, before the board or the
display. A battery build needs a new enclosure regardless, to house the charger.

## Deterministic state model

```text
external power detected
    -> cancel pending sleep
    -> wake or boot
    -> start normal RevLink services

external power lost
    -> enter configurable grace period
    -> continue from battery
    -> finish or safely cancel active work
    -> flush and unmount storage as required
    -> stop USB, radio, display, and network services
    -> enter deep sleep

external power returns
    -> GPIO wake
    -> initialize hardware
    -> restore normal services
```

Recommended initial grace period: five minutes. The timer may be made
configurable later, but sleep must never begin while a file transaction,
device write, database commit, or firmware update is active.

If external power disappears during a write, the battery must provide enough
hold-up time to complete or explicitly abort the operation. A low-battery
threshold must take the safe-shutdown path without beginning new work.

## Development-board manual shutdown

Until switched-power sensing and the battery power path exist, the accepted
manual demo control is a two-second hold of the Waveshare board's `BOOT`
button on GPIO35. The firmware blocks new syncs, cooperatively cancels active
work, stops the logical USB host, unmounts microSD, and enters deep sleep.

This is soft power only. It does not remove the board supply and it cannot
physically isolate the current board's USB-A data or VBUS. Press `RST` or
cycle power to wake. The production board should use the same lifecycle
sequence, but trigger it from external-power loss or a dedicated action button
and use its physical USB link-control adapter.

## Current BOOT button bindings

One button, on GPIO35, labelled `BOOT` on the Waveshare board and brought out
to the left-hand side button on the printed case. Presses count as a group when
they land within 650 ms of each other.

| Gesture | What happens |
| --- | --- |
| **Double-press** | Shows how to reach the portal. If the Sidecar is running its own hotspot you get a scannable Wi-Fi QR code; if it has joined a network instead, you get its `revlink-<id>.local` address. |
| **Any press while that is showing** | Dismisses it and returns to the normal status screen. |
| **Two double-presses within 20 s** | Authorises formatting a microSD that responded but would not mount. Deliberately awkward — see the storage recovery section. |
| **Hold 2 s** | Safe shutdown: blocks new syncs, cancels active work cooperatively, stops the USB host, unmounts the card, then deep sleep. |

Waking is `RST` or a power cycle; the hold is soft power and does not remove
the board supply.

All of the display gestures need the optional OLED fitted. Without one the
firmware still works and the button still shuts down on a hold, but the QR and
the address have nowhere to appear — use the address the portal shows, or
mDNS.

The double-press exists because "what is this thing's address" is the question
every new user asks first, and answering it on the device is faster than
explaining mDNS.

The QR is a standard `WIFI:T:WPA;S:<ssid>;P:<password>;` join code built on the
device, so any phone camera handles it — there is no app and nothing is looked
up remotely. It necessarily contains the hotspot password, which is the point:
the hotspot is a local fallback for joining the Sidecar itself, and anyone able
to read the screen is already holding the device.

Accepted on hardware: the code renders on the SH1106 and scans from a phone.

## Planned two-button controls

Do not implement this behavior until the development-board button wiring has
been reviewed. The intended product interaction is:

- **Power/sleep:** hold for five seconds to request safe shutdown or sleep.
- **Eject:** deliberately close the current AccessPort host session and return
  the device to drive-idle/Gauges.

Neither action may interrupt an active sync, transfer, device write, delete,
atomic cache/manifest commit, or session-close recovery. A press during one of
those states must be ignored and surfaced through the display/status layer;
it must not silently cancel work. Eject must use the accepted initialized
session lifecycle and acknowledged `0x05`/`0x35` close. It must not substitute
a context-free close, USB reset, or abrupt root-port shutdown.

The current development firmware still uses a two-second `BOOT` hold. The
planned five-second threshold and separate eject control are roadmap
requirements, not current behavior. The onboard `RST` switch may be
hardwired reset-only, so a dedicated GPIO input may be required for the
prototype and final board.

The future button-map review must also consider a five-rapid-press `BOOT`
gesture that requests a bounded retry of preferred saved Wi-Fi networks. This
is a candidate, not a current binding. If retained, its press window,
acknowledgment, cancellation, and safety-lock behavior must be designed
together with shutdown, eject, recovery, and factory-reset gestures so short
presses cannot accidentally trigger destructive or disruptive actions.

## Prototype acceptance

1. Verify the chosen vehicle USB outlet actually turns off in the intended
   ignition state; some outlets remain powered or shut down after a delay.
2. Confirm the power-path controller carries the full active load without a
   reset while external power is connected.
3. Remove external USB and verify uninterrupted battery operation.
4. Confirm the grace timer starts once, cancels if power returns, and never
   interrupts active work.
5. Enter deep sleep and verify the USB-present signal wakes the P4.
6. Repeat across rapid ignition cycling and low-battery conditions.
