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

### Measured bottom-shell openings

Sliced at z = 1.0 mm (the shipped `01` bottom, which is topologically
identical to the donor part):

| Wall | Span | Width |
| --- | --- | --- |
| Y− (`y = -2.5`) | `x` 3.0 → 18.0 | 15.0 mm |
| Y+ (`y = 52.5`) | `x` 19.7 → 27.1 | 7.4 mm |
| Y+ (`y = 52.5`) | `x` 32.0 → 40.0 | 8.0 mm |

The bottom is carried over from the donor case unchanged; the revised lid is
what conceals the Ethernet jack in the assembled enclosure. If you close or
reshape any of these bottom openings, the genus should drop accordingly —
that is the check this tool exists for.

Note that genus cannot see solid internal additions: adding the four OLED
mounting posts will raise the triangle count while leaving genus unchanged.
Check those visually in the slicer and against the physical display PCB.
