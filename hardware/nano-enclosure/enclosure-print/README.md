# RevLink Sidecar ESP32-P4 Nano enclosure

> [!NOTE]
> This geometry has been printed and assembled successfully. The revised lid
> conceals the Ethernet jack, closes the donor case's switch slot and spare
> USB-A opening, and adds the OLED window and guided side buttons.
>
> Remaining refinement: add four internal OLED mounting legs/posts to the lid
> so the display PCB is positively located rather than retained by the window
> edge. Measure the physical OLED PCB, connector, solder joints, and wire exit
> before exporting new geometry.

This directory contains the Nano enclosure derived from the supplied
55 × 55 mm case.

## Open refinements

- [ ] Add four internal mounting legs/posts around the OLED opening.
- [ ] Confirm the supports retain the OLED PCB without touching the active
  display, connector, solder joints, or wiring.
- [ ] Regenerate the lid STL/3MF and record before/after `stl_inspect.py`
  output in the pull request.

## Revised lid

`revlink-sidecar-nano-top-31mm-oled.stl` and its matching `.3mf`:

- retain the donor lid's 55 × 55 mm footprint, mating edge, retaining clips,
  and connector clearances;
- increase the lid itself to exactly **31.0 mm** tall;
- retain the original **2.0 mm** roof thickness instead of stretching it;
- use a solid, smooth exterior roof;
- place a rounded **15.5 × 30.2 mm** portrait OLED window at the exact center
  of the lid; and
- expose only the required USB-A connector, with the extra upper opening
  closed;
- conceal the installed Ethernet jack behind a **0.8 mm** exterior wall with
  internal connector clearance;
- close the donor's large switch slot and provide guided side controls beside
  USB-C for the onboard **RST** and **BOOT** switches;
  and
- are closed, consistently wound manifold solids.

The OLED opening exposes the documented lit area of the existing 1.3-inch
SH1106 module. The larger 35.4 × 33.5 mm OLED PCB mounts behind the opening.

With the supplied 5.6 mm bottom, the modeled assembled enclosure height is
36.6 mm.

## Printed RST and BOOT buttons

`revlink-sidecar-nano-usbc-side-buttons.3mf` contains two identical,
pre-arranged side buttons: one for RST and one for BOOT. The matching
`revlink-sidecar-nano-usbc-side-button.stl` contains one universal button;
duplicate it once in the slicer.

Install both buttons from the USB-C edge before mating the lid and bottom.
Each button has:

- a low-profile **3.8 × 3.0 mm** rounded exterior cap;
- a short guided shaft aligned to the 2 × 2 mm side actuator; and
- a small retention flange to keep it captive after the fit-check.

Print the buttons in PETG for the slight compliance needed by the retention
flange. Verify free movement and switch return before fully closing the
enclosure.

## Printing

- Prefer PETG for an indoor fit-check and ASA for vehicle use.
- Start with 0.20 mm layers, four perimeters, five top/bottom layers, and
  25–35% infill.
- The supplied button STL and 3MF are already oriented cap-down. Print them at
  0.12–0.16 mm layers.
- Print one inexpensive fit-check before committing to the final material.
- Center the physical OLED's lit area behind the window before fixing the
  module in place.
- The STL is unitless by definition and is authored in millimeters. The 3MF
  records millimeter units explicitly.

## Rebuilding

The reproducible builder requires Python 3.11+, `numpy`, `trimesh`,
`manifold3d`, and `lxml`.

```sh
python build_oled_lid.py \
  --source source/esp32-p4-nano-top-solid.stl \
  --stl-output revlink-sidecar-nano-top-31mm-oled.stl \
  --3mf-output revlink-sidecar-nano-top-31mm-oled.3mf \
  --button-output revlink-sidecar-nano-usbc-side-button.stl \
  --button-3mf-output revlink-sidecar-nano-usbc-side-buttons.3mf
```
