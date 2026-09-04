# RevLink Sidecar battery enclosure

> [!WARNING]
> **Not print-ready yet.** The Adafruit 6106 charger's outline and hole
> positions come from Adafruit's published board layout, but its part heights
> and USB-C receptacle type were never published and are **provisional**. Work
> through the [measurement checklist](#measurement-checklist) against the
> physical board, update the `PROVISIONAL` block at the top of
> `build_battery_enclosure.py`, rebuild, and only then send geometry to
> PCBWay. Issue #23 tracks this.

One shell, one lid, two side buttons. Holds the **ESP32-P4-NANO**, the
**1.3-inch SH1106 OLED**, the **Adafruit 6106** charger/boost board and an
**EEMB LP103454** cell (54 × 34 × 10.3 mm).

| | Battery enclosure | Original 55 mm case |
| --- | --- | --- |
| Footprint | **120.0 × 59.0 mm** | 55 × 55 mm |
| Assembled height | **23.6 mm** | 36.6 mm |
| Cell + charger | yes | no room |
| Nano USB-C | concealed (optional service opening) | exposed |

![Plan view](preview-layout.png)

Authored from scratch: the builder loads no donor file and the directory
carries no upstream attribution. See [Licensing](#licensing).

## Layout

Three floor-level bays in a row along X, in the Nano's own PCB coordinate
frame (PCB `x` 0–50, `y` 0–50, underside `z` 0):

```
 +Y face:  [USB-A] [RJ45 blind]          [charger USB-C]
           +------------------+----+---------+----+---------------------+
           |                  |    |  6106   |    |     cell 34 x 54    |
           |   ESP32-P4-NANO  |rib |  19x29  |rib |   OLED pocket above |
           |      50 x 50     |    |  (spare |    |   window 15.5x30.2  |
           |                  |    |  -Y end)|    |                     |
           +------------------+----+---------+----+---------------------+
 -Y face:  [RST][BOOT] [USB-C blind] [microSD]
```

- **Nano bay** `x` −0.5..50.5, `y` −1.8..50.6. Four Ø5 posts on the PCB's
  own holes. USB-A and the blind RJ45 face +Y; USB-C (blind), RESET, BOOT
  and the microSD slot face −Y.
- **Charger bay** `x` 52.5..72.55, `y` −4.4..50.6. The 6106 is rotated −90°
  so its USB-C faces the +Y wall beside the USB-A, its JST battery connector
  faces the cell, and its 5 V terminal block faces the spare −Y end of the
  bay (25 × 20 mm free — the future fuel gauge from #17 fits there, next to
  `P1`).
- **Cell bay** `x` 74.55..111.65, `y` −4.4..50.6. The cell lies flat on the
  floor, located by the rib and a 2 mm ledge along the right wall. The OLED
  sits in a pocket frame on the lid underside directly above it; the window
  is centred on the bay.

Why this arrangement:

- The 6106's 5 V output feeds the Nano header directly (`P2` pins 1/3, see
  `docs/POWER_MANAGEMENT.md`), so nothing is plugged into the Nano's USB-C and
  it can be fully blind.
- Six of the OLED's seven wires land on `P1`, the right-hand header, so the
  display belongs on the right of the board. The cell goes under it because
  it is the only part with nothing tall on it.
- The charger sits between the board and the cell so every lead is short and
  crosses at most one rib: cell → JST (one rib), terminal block → `P2` (one
  rib), OLED → `P1` (two ribs, over the top).
- The cell is on the far side of the case from the ESP32-C6 antenna (top-left
  corner of the Nano).

## Stack-up

`z` is the Nano frame; the outside bottom is at −5.0.

| `z` (mm) | |
| --- | --- |
| −5.0 … −3.0 | floor, 2.0 |
| −3.0 … 0.0 | Ø5 / Ø4.6 posts, 3.0 (the Nano's deepest underside part is 2.25) |
| 0.0 … 1.6 | both PCBs |
| 1.6 … 16.6 | 15 mm M2.5 male/female standoffs — they clamp the PCBs *and* carry the lid |
| 15.8 | top of the vertical USB-A receptacle (0.8 clearance) |
| 14.1 | rib tops (2.5 below the lid: a wire pass along the whole rib) |
| 7.3 | top of the cell |
| 11.0 … 13.0 | OLED solder joints / wire tails (2 mm allowance) |
| 13.0 … 16.6 | OLED module in the lid pocket, glass against the roof |
| 16.6 … 18.6 | lid, 2.0 |

The height is set by the Nano's **vertical USB-A receptacle** (14.2 mm above
the PCB) plus floor, posts, PCB, clearance and lid: **23.6 mm**. Nothing else
comes close; the cell column has 9.3 mm free above the cell, of which the
OLED uses 5.6.

### What was traded away

- **Footprint.** 120 × 59 against 55 × 55. Side-by-side costs area; the issue
  asked for lower rather than taller and that is what this is.
- **Ten M2.5 screws.** Every standoff doubles as a lid support, so walls stay
  2 mm with no bosses intruding on the PCBs. Two more screws go into a
  5.85 mm right-hand wall over the cell.
- **Reflashing.** The Nano's USB-C is behind a 4.6 mm wall. See
  [Flashing](#flashing-happens-before-assembly).
- **microSD** stays removable but is behind 4.6 mm of wall: an ejected card
  should reach the 1 mm finger scoop; tweezers may be needed. Confirm on the
  real socket (checklist item 9).
- **Dupont housings do not fit.** Header pins top out at `z` 10.5 and the lid
  is at 16.6; a 14 mm crimp housing will not. Solder the four power/I²C wires
  to the header pins (top or tails), or use bare crimps.
- **The OLED is off-centre** relative to the whole case (it is centred on the
  cell bay, right third).

## Openings

| Feature | Position | Size |
| --- | --- | --- |
| USB-A | +Y face, `x` 19.2–27.6 | 8.4 wide, top-open notch from `z` −0.5, closed by the lid |
| RJ45 | +Y face, `x` 27.5–44.5 | concealed behind a 1.1 mm face, 0.5 clearance |
| Charger USB-C | +Y face, centre `x` 62.5, `z` 3.23 | 13.0 × 7.0 r1.5, the USB-C plug-overmold envelope, nose 1.36 behind the face |
| RESET / BOOT | −Y face, `x` 7.75 / 13.25, `z` 3.25 | Ø2.0 bores for the printed plungers |
| microSD | −Y face, `x` 31–44.5, `z` −3.0–0.3 | slot plus a 1.0 mm deep finger scoop |
| Nano USB-C | −Y face | blind; `NANO_USB_C_SERVICE_OPENING = True` cuts a 13 × 7 service opening |
| OLED window | lid, centre (93.1, 23.1) | 15.5 × 30.2 r1.0, the lit area |
| Wire passes | both ribs, 2.5 mm under the lid; one 6 × 6 notch per rib | for the 5 V pair and the cell lead |

## Hardware

| Qty | Part |
| --- | --- |
| 8 | M2.5 male/female brass standoff, 15 mm body, 6 mm male thread, 4 mm AF hex |
| 8 | M2.5 × 5 lid screws into the standoffs |
| 2 | M2.5 × 6 lid screws into the right-hand wall |
| 2 | printed side buttons (`03-…-side-buttons-pair`), PETG |
| 1 | 3 mm closed-cell foam pad, ~34 × 50, between cell and OLED (insulates and preloads the module against the roof) |
| — | thin double-sided tape for the OLED glass to the roof; Kapton over the OLED solder side |

Post holes are Ø2.05, 4.6 deep: tap M2.5 in a machined part, or let the
standoff's male thread form its own in MJF nylon. If you prefer heat-set
inserts, set `POST_HOLE_DIAMETER_MM` to the insert's hole (≈3.5) and
`NANO_POST_DIAMETER_MM` / `CHARGER_POST_DIAMETER_MM` to ≈6.

## Manufacturing

Designed primarily for **PCBWay MJF (PA12 nylon)** or **SLA**, which is what
"3D printing" means at a service bureau rather than a desktop FDM machine:

- 2.0 mm walls and floor, 1.1 mm RJ45 face, 1.2 mm OLED frame — all above
  MJF's 1 mm minimum and SLA's 0.8 mm.
- No snap fits, no print-in-place, no bridges. The USB-A notch is top-open
  so no wall spans it.
- Everything is a fastener: standoffs and M2.5 screws.

**CNC (aluminium 6061 or POM)** works with the same files, because the
geometry was drawn to be machinable:

- The shell is three top-open pockets with 1.5 mm internal corner radii
  (Ø3 end mill), posts and a ledge — one setup from the top. Side openings
  are two further setups (+Y and −Y faces). No undercuts, no draft.
- The lid is a 2 mm plate with through features from the top and a 3.6 mm
  pocket frame from below. For CNC either machine it from 5.6 mm stock, or
  make the lid a plain 2 mm plate and bond a separate frame ring.
- Tap the ten M2.5 holes. The plungers stay printed (PETG/SLA) regardless —
  their retention flange relies on a little compliance.
- Aluminium makes the cell bay a Faraday enclosure; the C6 antenna is in the
  Nano bay at the far end, but expect to re-check Wi-Fi range.

FDM at home also works: PETG or ASA, 0.2 mm layers, four perimeters; print the
shell open side up and the lid pocket side up (the window needs no support).

## Assembly

1. Press both plungers into the −Y bores from outside until the flange snaps
   through.
2. **Flash the Nano first** (below). Solder the four power/I²C wires and the
   seven OLED wires to the headers; Dupont housings do not fit.
3. Fit the Nano and the 6106 on their posts with the eight standoffs. Route
   the 5 V pair through the notch in the first rib to `P2`.
4. Lay the cell in its bay, lead end toward the charger, and plug the JST
   through the notch in the second rib. Foam pad on top.
5. Tape the OLED into the lid pocket, glass to the roof, wires out through a
   frame notch, across both ribs to `P1`/`P2`.
6. Lid on, ten screws.

### Flashing happens before assembly

The Nano's own USB-C is inside the shell. Flash the firmware **before** step
3. To reflash over USB later, either:

- take the lid off, unscrew the four Nano standoffs and lift the board clear
  of the −Y wall (leave ~40 mm of slack in its wiring), or
- rebuild the shell with `NANO_USB_C_SERVICE_OPENING = True`, which cuts a
  13 × 7 mm opening for the port on the −Y face.

`docs/FIRMWARE_UPDATE.md` covers the planned browser update path, which will
remove the need for either.

## Measurement checklist

Ten minutes with calipers. Update the constants named in brackets, rebuild,
re-run `check_fit.py`, and paste the results into the PR.

**Adafruit 6106**

1. PCB outline, width × length, and thickness. Expect 29.21 × 19.05 × 1.6.
   [`CHARGER_PCB_SIZE_MM`, `CHARGER_PCB_THICKNESS_MM`]
2. Mounting-hole centres from two adjacent edges and their diameter. Expect
   2.54 / 26.67 along the long edge, 2.54 / 16.51 along the short, Ø2.5.
   [`CHARGER_HOLES_MM`, `CHARGER_HOLE_DIAMETER_MM`]
3. USB-C receptacle: centre position along the short edge (expect 9.525),
   how far the shell overhangs the PCB edge (expect ~1.1), and **whether it
   is top-mount or mid-mount** — then its shell height above the PCB top
   (expect 3.26 if top-mount, ~1.7 if mid-mount). This moves the wall cutout.
   [`CHARGER_USB_C_CENTER_MM`, `CHARGER_USB_C_OVERHANG_MM`, `CHARGER_USB_C_HEIGHT_MM`]
4. Tallest part above the PCB top (probably the green terminal block; expect
   ≤ 8.6) and the JST PH height. [`CHARGER_MAX_PART_HEIGHT_MM`]
5. Whether the 1 × 8 header is fitted. If pins are soldered downward their
   tails must be ≤ 3 mm or trimmed, or the posts get taller.
   [`POST_HEIGHT_MM`]
6. With a 4 mm AF standoff in each hole, confirm ≥ 0.3 mm to the inductor and
   to the USB-C shell at the two holes nearest them (the layout suggests
   ~0.4 mm to the inductor).

**SH1106 OLED module**

7. Thickness from glass face to PCB back, without pins (expect ~3.6).
   [`OLED_MODULE_THICKNESS_MM`]
8. Lit-area centre offset from the PCB centre, X and Y, and which edge has
   the pads. Typical modules are offset ~2.5 mm away from the pad edge.
   [`OLED_ACTIVE_OFFSET_MM`, `OLED_WIRE_EXIT_WIDTH_MM`]
9. Solder/wire height below the PCB back after wiring (must be ≤ 2.0).

**ESP32-P4-NANO**

10. How far the microSD card protrudes past the PCB edge when latched and
    after eject. The socket face is 4.6 mm from the outer wall, 3.6 to the
    scoop floor. [`MICROSD_SCOOP_DEPTH_MM`]
11. RESET/BOOT actuator tips at −0.5 mm past the edge, centre `z` 3.25
    (from the board model — a quick sanity check with the plungers fitted).

**Cell**

12. Which short end the lead leaves and its length: it needs ~60 mm to reach
    the JST through the rib notch at `y` ≈ 36.

## Verification

`tools/stl_inspect.py` on the shipped files:

```
01-revlink-sidecar-battery-shell.stl
  triangles 5042   vertices 2515   shells 1
  bbox  120.00 x 59.00 x 21.60 mm
  watertight True  (boundary edges 0, non-manifold 0)
  euler -6   genus 4   <- through-holes

02-revlink-sidecar-battery-lid-oled.stl
  triangles 2638   vertices 1299   shells 1
  bbox  120.00 x 59.00 x 5.60 mm
  watertight True  (boundary edges 0, non-manifold 0)
  euler -20   genus 11   <- through-holes

03-revlink-sidecar-battery-side-buttons-pair.stl
  triangles 1564   vertices 786   shells 2
  bbox  11.80 x 3.00 x 6.75 mm
  watertight True  (boundary edges 0, non-manifold 0)
  euler 4   genus 0   <- through-holes
```

Shell genus 4 = two plunger bores + microSD slot + charger USB-C; the USB-A
notch is open to the top so it is not a hole. Lid genus 11 = window + ten
screw holes. Buttons: two solid bodies.

`check_fit.py` booleans the shell and lid against the Nano board model, a
cell block, a charger envelope, the OLED and the eight standoffs and reports
0.000 mm³ overlap for every pair, and fires rays down every port axis. It
passes. Run it after any constant changes.

## Rebuilding

Python 3.11+, `numpy`, `trimesh`, `shapely`, `manifold3d`, `lxml`
(`pip install "trimesh[easy]" manifold3d`).

```sh
python build_battery_enclosure.py      # writes print-package/*.stl and *.3mf
python check_fit.py                    # interference and port-axis checks
python ../tools/stl_inspect.py print-package/*.stl
```

The builder prints the derived layout (bay extents, charger hole positions,
wall thicknesses) so the numbers in this README can be checked against it.
Prefer the 3MF files for a print service; they record millimetre units.

## Licensing

This directory is CC BY-SA 4.0 like the rest of `hardware/`, with **no
upstream attribution**: it is not a derivative of MartinFaustMayer's case.
The Nano dimensions were measured from the donor's board model (planar
sections of `../enclosure-print/source/esp32-p4-nano-board.stl`); those are
facts about the Waveshare board, not copied geometry, and `check_fit.py` uses
that model only as a test fixture. The 6106 outline and hole positions were
read from Adafruit's published layout
([adafruit/Adafruit-bq25185-with-5V-Boost-PCB](https://github.com/adafruit/Adafruit-bq25185-with-5V-Boost-PCB),
commit `2ce979f`); no Adafruit file is included. See `../../../NOTICE`.
