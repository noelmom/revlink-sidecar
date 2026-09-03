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
USB-C port, with an AccessPort attached:

| Condition | Current | Voltage | Power |
| --- | --- | --- | --- |
| Steady, connected and syncing | 0.12 – 0.16 A | 4.86 V | 0.58 – 0.77 W |
| Cold boot, AccessPort attached, initial sync | 0.20 A | 4.85 V | 0.96 W |
| **Highest seen — cold boot after an unplug/replug** | **0.28 A** | **4.74 V** | **1.32 W** |

The 4.74 V in that last row is an artefact of the measurement path, not the
supply — see below.

After the initial sync it settles back to the steady range.

That is the whole system: ESP32-P4 with 32 MB PSRAM, the ESP32-C6 radio over
SDIO, microSD, and the AccessPort as a downstream USB device. It is far lower
than the figure this section was written in anticipation of, and it changes
what "enough peak current" has to mean — a 1 A supply has roughly six times the
headroom needed for steady-state operation.

0.28 A is only **28% of the TPS61023's 1 A ceiling**, and about 0.2C on a
2000 mAh cell, so current is not the constraint. Runtime is roughly 5 hours if
it somehow held at that peak and 8 or more in practice.

### The constraint is voltage, not current

Those four points make a load line, and it slopes:

```text
0.12 A -> 4.86 V
0.16 A -> 4.86 V
0.20 A -> 4.85 V
0.28 A -> 4.74 V
```

That is roughly **1.4 ohm** of effective series resistance between the boost
and the meter. USB VBUS is specified as 4.75 – 5.25 V, so at 0.28 A the rail is
already a hair under the floor, and extrapolating the same slope gives about
4.6 V at 0.35 A and 4.4 V at 0.5 A.

**It was the wiring, not the board.** 1.4 ohm was always too high to be the
boost converter's regulation. Feeding the 5 V rail directly at the header —
P2 pin 1 for 5 V and pin 3 for GND — removes the USB meter, a USB-C adapter,
the cable and two connectors from the path, and **the sag disappears**.

So the load line above measures a test rig, not the supply. Record it anyway,
because it is a useful reminder of how much a convenient measurement setup can
invent: a rail that looked marginal against the USB minimum was actually fine,
and the instrument was the problem.

Two things follow. Take bench readings **at the board**, not at an inline meter
several connectors upstream. And in a permanent install, feed the header
directly rather than through a USB-C lead, for the same reason it fixed the
measurement — every connector in the path costs voltage at exactly the moment
current peaks.

The AccessPort's VBUS derives from this same rail, so whatever the rail sags
to, the attached device sees less. That is why this mattered enough to chase.

### Cold boot from battery works

This was the open question, because a boost converter that cannot start into
the load makes the whole approach unusable. The Adafruit TPS61023 board is
documented as stalling on a load that draws full current immediately, above
roughly 200 mA.

The measured cold-boot peak is **exactly 200 mA**, and the Sidecar starts
anyway — the P4 evidently ramps as PSRAM and the radio come up rather than
slamming to full draw at the instant power appears, which is the case the
caution is about.

That is a pass, but it is a pass at the boundary rather than with margin.
Worth repeating **at a low state of charge**: as the cell droops the boost
draws more input current for the same output, so a start that succeeds on a
full cell is not proof of one on a nearly flat cell.

One thing still unmeasured: **a map write or delete under way.** Those hold the
USB host busy in a different pattern to a read sync and have not been measured
separately.

### Power path, confirmed

Applying USB-C power to the charger board **starts charging the cell while the
Nano stays running**. There is no drop-out, reset or re-enumeration at the
moment external power arrives, which is the simultaneous-charge-and-load and
seamless-handoff behaviour this section requires.

Only that direction has been tested. **Removing** external power and having the
cell take over without interrupting the P4 — the transition that actually
matters for a car that has just been switched off, and the one the state model
below depends on — has not been confirmed. Test it mid-sync, not idle: an
interruption there is the case with something to lose.

### Wiring the header

The 5 V rail is exposed on both 2x13 headers. On **P2**, the one nearest RESET,
pin 1 is 5 V and pin 3 is GND — adjacent in the same column, so it is a clean
two-wire tap. Pin 2 beside it is `ESP_LDO_VO4`, which is not a ground.

On **P1** the equivalents are pin 2 or pin 4 for 5 V and pin 6 for GND.

Disconnect USB-C before feeding the header. The board supplies `VCC_5V` through
a DIO7003 load switch fed from the USB-C port; injecting on the header bypasses
that switch, so two sources would be contending with nothing arbitrating. It
also means the board's own over-current protection is not in the path, so the
supply side owns that responsibility.

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
