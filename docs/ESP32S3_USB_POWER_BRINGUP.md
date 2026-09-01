# ESP32-S3 USB host power bring-up

> **Archived feasibility record — do not use as an active build or bring-up
> plan.** The ESP32-S3 target was retired on 2026-07-26 after its full-speed
> USB controller failed the required endpoint gate. ESP32-P4 is the sole
> consumer-firmware target. Do not spend time maintaining S3 compatibility or
> designing a companion high-speed host for it.

The generic ESP32-S3-N16R8 development board has separate `COM` and native
`USB` USB-C connectors. The native connector routes USB D+/D- to the ESP32-S3,
but its ability to act as a safe 5 V downstream power source is unverified.

Do not use the AccessPort as the first electrical load. A visible 5 V reading
with no load does not prove that the connector can safely power it.

## Live result — 2026-07-25

A known-good powered USB adapter successfully supplied the AccessPort and
passed USB data to the ESP32-S3 native host. The AccessPort booted normally
and enumerated at full speed as VID/PID `1a84:0121`.

The descriptor-only firmware read all four standard configuration
descriptors. Every full-speed descriptor exposed only `0x02` OUT and `0x81`
IN with 64-byte maximum packets. None exposed the proven file-channel
endpoints `0x03` OUT and `0x82` IN. No interface was claimed and no
proprietary or storage request was sent.

This result proved the prototype power path, D+/D- path, ESP32-S3 host role,
and basic enumeration. It also failed the file-channel feasibility gate for
the ESP32-S3 native full-speed controller. The subsequent ESP32-P4 experiment
provided the required high-speed host. The S3 is not retained as a
Wi-Fi/application controller.

## Known facts

- The native connector is the only connector suitable for the ESP32-S3
  internal USB host controller.
- The board has no visibly identified USB high-side power switch,
  current-limit controller, or over-current signal.
- The board is an unidentified DevKitC-style clone, so the official Espressif
  schematic is only a reference, not proof of this board's wiring.
- A read-only query on the development Pi reports `MaxPower 2mA` in all four
  AccessPort USB configurations. This descriptor value is not credible as the
  device's actual operating current and must not be used to size the supply.
- An inline USB meter measurement on 2026-07-25 captured the following
  AccessPort states on a known-good source:
  - disconnected baseline: `4.97 V`, `0.00 A`, `0.00 W`;
  - startup/help screen: `4.98 V`, `0.12 A`, `0.58 W`;
  - running/menu screen: `4.98 V`, `0.08 A`, `0.39 W`; and
  - later menu state: `4.93 V`, `0.04 A`, `0.19 W`.
  The largest captured draw was therefore about 120 mA. Still treat this as an
  observed operating value rather than a guaranteed inrush maximum because
  the meter/video sampling rate may miss short transients.

## Required equipment

Preferred:

- USB-C breakout or known-good USB power meter that exposes VBUS and GND;
- adjustable USB electronic load;
- inline USB current meter for measuring the AccessPort on the Pi; and
- current-limited 5 V bench supply or protected powered USB source.

A multimeter can establish continuity and no-load voltage, but it cannot prove
the port is safe under startup load.

## Test sequence

### 1. Map the unpowered board

Disconnect both USB connectors and everything on the headers.

1. Check continuity/diode behavior between native-USB VBUS and the board's 5 V
   header.
2. Check between native-USB VBUS and COM-port VBUS.
3. Check that native-USB GND and board GND are common.
4. Record both meter polarity readings; a diode path in one direction matters.

Do not inject voltage during continuity or diode mode.

### 2. Measure no-load VBUS

Power only the `COM` connector. Leave the AccessPort disconnected.

1. Measure native-USB VBUS to GND.
2. Reverse the USB-C plug orientation and repeat.
3. Pass range: 4.75–5.25 V in both orientations.

Zero volts means the onboard connector cannot source VBUS in this arrangement.
Five volts permits load testing but is not yet an approval to attach the
AccessPort.

### 3. Apply a protected dummy load

Use an electronic load, beginning at zero current:

1. 50 mA for 30 seconds;
2. 100 mA for 60 seconds;
3. 250 mA for 60 seconds; and
4. only after measuring the real AccessPort current, its observed startup peak
   plus at least 25% margin.

For the current measured unit, 150 mA is 25% above the largest captured
120 mA reading. The development path should nevertheless pass the existing
250 mA step, and a protected 5 V source rated for at least 500 mA is the
minimum practical prototype target. A 1 A current-limited source provides
additional bring-up margin without changing the AccessPort's actual demand.

At every step record:

- VBUS at the native connector;
- load current;
- COM-source input current;
- ESP32 resets or serial disconnects; and
- unexpected heating at the connectors, diodes, traces, or regulator.

Stop immediately if VBUS drops below 4.75 V, the board resets, the source
current-limits, or any component heats rapidly. Do not jump directly to a
500 mA load.

### 4. Measure the AccessPort on the known-good Pi

Place an inline USB meter between the Pi and AccessPort and record:

- unplugged baseline;
- startup peak;
- steady idle current;
- screen-on current; and
- peak during directory listing and file transfer.

The USB descriptor's reported 2 mA is not a substitute for these measurements.

### 5. First AccessPort power-only test

Only after the dummy-load test exceeds the measured AccessPort peak:

1. use a current-limited source and inline meter;
2. leave ESP32 USB-host transactions disabled;
3. connect the AccessPort and observe startup;
4. confirm VBUS stays in range and the ESP32 does not reset; and
5. disconnect at the first sign of brownout or heating.

This proves power-up only. It does not prove USB host role signaling,
enumeration, or data reliability.

### 6. Enumeration test

Before the AccessPort, flash the read-only USB-host enumeration firmware and
test with an inexpensive low/full-speed USB peripheral. Then connect the
AccessPort and record:

- VID/PID `1a84:0121`;
- negotiated speed;
- active configuration;
- interface `0`;
- bulk OUT `0x03` and bulk IN `0x82`; and
- active endpoint maximum packet sizes.

No storage request should be transmitted during the first enumeration test.

This test was completed on 2026-07-25. VID/PID and full-speed enumeration
passed, but the required `0x03/0x82` endpoint pair was absent from all four
full-speed configuration descriptors. Do not proceed to an interface claim
or root-directory request on this native host.

## Expected product solution

Even if the development board happens to backfeed 5 V successfully, that is
not a production-quality host supply. The product design should provide a
dedicated protected 5 V VBUS path with:

- a current-limited high-side/load switch;
- reverse-current blocking;
- controlled enable;
- over-current/fault reporting;
- local bulk and high-frequency decoupling; and
- enough startup-current margin for the AccessPort.

For prototype USB data testing, use a properly powered host adapter, power
injector, or powered hub rather than relying on an unidentified board trace.
