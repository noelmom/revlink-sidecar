# Enclosure validation tools

## `stl_inspect.py`

Reports triangle count, bounding box, watertightness, and **genus** for a
binary STL. No dependencies — plain Python 3.

Genus is the useful number: it counts through-holes in a closed shell. Use it
to prove that a revision actually closed an opening rather than just
re-exporting the same geometry.

```bash
python3 stl_inspect.py ../enclosure-print/print-package/*.stl
```

Current baseline:

| Part | Genus | Meaning |
| --- | --- | --- |
| `source/esp32-p4-nano-bottom.stl` (donor) | 5 | Original case bottom |
| `01-…-nano-bottom.stl` (shipped) | 5 | Unchanged from donor |
| `source/esp32-p4-nano-top-solid.stl` (donor) | 6 | Original case lid |
| `02-…-top-oled-usbc-controls.stl` (shipped) | 3 | Revised lid — three openings closed |
| `battery-enclosure/…/01-…-battery-shell.stl` | 4 | Battery shell: two button bores, microSD slot, charger USB-C (USB-A is a top-open notch, so it is not counted) |
| `battery-enclosure/…/02-…-battery-lid-oled.stl` | 11 | Battery lid: OLED window plus ten M2.5 clearance holes |
| `battery-enclosure/…/03-…-battery-side-buttons-pair.stl` | 0 | Two solid plungers (two shells) |

### Measured bottom-shell openings

Sliced at z = 1.0 mm (the shipped `01` bottom, which is topologically
identical to the donor part):

| Wall | Span | Width |
| --- | --- | --- |
| Y− (`y = -2.5`) | `x` 3.0 → 18.0 | 15.0 mm |
| Y+ (`y = 52.5`) | `x` 19.7 → 27.1 | 7.4 mm |
| Y+ (`y = 52.5`) | `x` 32.0 → 40.0 | 8.0 mm |
| Y− (`y = -2.5`) | `x` 31.0 → 44.5 | 13.5 mm — **microSD**, present only below `z ≈ 0.3`, so a `z = 1.0` slice does not show it |

The fourth row was missed when this table was first written and is the
reason the bottom's genus is 5 rather than 4: the card slot on the Nano's
underside (`x` 32 → 43.5, flush with the USB-C edge) is open to the outside
in the shipped case, so the card can be removed assembled.

The bottom is carried over from the donor case unchanged; the revised lid is
what conceals the Ethernet jack in the assembled enclosure. If you close or
reshape any of these bottom openings, the genus should drop accordingly —
that is the check this tool exists for.

Note that genus cannot see solid internal additions: adding the four OLED
mounting posts will raise the triangle count while leaving genus unchanged.
Check those visually in the slicer and against the physical display PCB.
