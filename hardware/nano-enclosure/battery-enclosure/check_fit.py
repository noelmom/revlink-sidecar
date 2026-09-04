#!/usr/bin/env python3
"""Interference check for the battery enclosure.

Booleans the generated shell and lid against the Nano board model, a
cell-sized block, a charger envelope, the OLED module and the eight brass
standoffs, and reports any overlapping volume. Also fires rays down every
port axis to prove the openings are clear. Run after build_battery_enclosure.py.

The Nano board model is the donor's CC BY 4.0 file in ../enclosure-print/source;
it is used here as a test fixture only.
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import trimesh

sys.path.insert(0, str(Path(__file__).parent))
import build_battery_enclosure as B  # noqa: E402

HERE = Path(__file__).parent
PKG = HERE / "print-package"
BOARD_MODEL = HERE.parent / "enclosure-print" / "source" / "esp32-p4-nano-board.stl"
EPS = 0.01  # lift test bodies off coplanar contact faces


def overlap(a: trimesh.Trimesh, b: trimesh.Trimesh, name: str) -> float:
    r = trimesh.boolean.intersection([a, b], engine="manifold")
    v = 0.0 if r is None or r.is_empty else abs(float(r.volume))
    status = "OK" if v < 0.05 else f"INTERFERENCE {np.round(r.bounds, 2).tolist()}"
    print(f"  {name:36s} {v:8.3f} mm^3  {status}")
    return v


def clear_ray(mesh: trimesh.Trimesh, origin, direction, name: str, expect=None) -> bool:
    hits = mesh.ray.intersects_location(ray_origins=[origin], ray_directions=[direction])[0]
    ys = sorted(round(float(h[1]), 2) for h in hits)
    ok = (not hits.size) if expect is None else (ys == expect)
    print(f"  {name:36s} hits {ys or 'none'}  {'OK' if ok else 'BLOCKED'}")
    return ok


def main() -> int:
    L = B.compute_layout()
    shell = trimesh.load_mesh(PKG / "01-revlink-sidecar-battery-shell.stl", process=True)
    lid = trimesh.load_mesh(PKG / "02-revlink-sidecar-battery-lid-oled.stl", process=True)
    board = trimesh.load_mesh(BOARD_MODEL, process=True)
    trimesh.repair.fill_holes(board)
    board.apply_translation((0.0, 0.0, EPS))

    c = B.BATTERY_CLEARANCE_MM / 2
    cell = B.box(L.battery.x0 + c, L.battery.y0 + c, L.floor_z + EPS,
                 L.battery.x0 + c + B.BATTERY_SIZE_MM[0], L.battery.y0 + c + B.BATTERY_SIZE_MM[1],
                 L.floor_z + B.BATTERY_SIZE_MM[2])

    ox, oy = L.charger_origin
    charger = B.box(ox, oy - B.CHARGER_PCB_SIZE_MM[0], EPS, ox + B.CHARGER_PCB_SIZE_MM[1], oy,
                    B.CHARGER_PCB_THICKNESS_MM + B.CHARGER_MAX_PART_HEIGHT_MM)
    # The envelope is a solid block; carve out its own mounting-hole columns.
    charger = trimesh.boolean.difference(
        [charger, B.union([B.cylinder_z(x, y, -1.0, 12.0, 2.6) for x, y in L.charger_holes])],
        engine="manifold",
    )
    usb_c = B.box(L.charger_usb_c_x - B.CHARGER_USB_C_WIDTH_MM / 2, oy - 1.0, B.CHARGER_PCB_THICKNESS_MM,
                  L.charger_usb_c_x + B.CHARGER_USB_C_WIDTH_MM / 2, oy + B.CHARGER_USB_C_OVERHANG_MM,
                  B.CHARGER_PCB_THICKNESS_MM + B.CHARGER_USB_C_HEIGHT_MM)

    oled_z0 = L.top_z - B.OLED_MODULE_THICKNESS_MM
    oled = B.box(L.oled_center[0] - B.OLED_PCB_SIZE_MM[0] / 2, L.oled_center[1] - B.OLED_PCB_SIZE_MM[1] / 2,
                 oled_z0 - 2.0, L.oled_center[0] + B.OLED_PCB_SIZE_MM[0] / 2,
                 L.oled_center[1] + B.OLED_PCB_SIZE_MM[1] / 2, L.top_z - EPS)

    # 4 mm AF hex standoffs, modelled at their across-corners diameter.
    standoffs = [B.cylinder_z(x, y, B.NANO_PCB_THICKNESS_MM + EPS, L.top_z - EPS, 2.31)
                 for x, y in (*L.nano_holes, *L.charger_holes)]

    print("Overlap volumes")
    worst = 0.0
    worst = max(worst, overlap(shell, board, "shell vs Nano board model"))
    worst = max(worst, overlap(lid, board, "lid vs Nano board model"))
    worst = max(worst, overlap(shell, cell, "shell vs cell"))
    worst = max(worst, overlap(lid, cell, "lid vs cell"))
    worst = max(worst, overlap(shell, charger, "shell vs charger envelope"))
    worst = max(worst, overlap(lid, charger, "lid vs charger envelope"))
    worst = max(worst, overlap(shell, usb_c, "shell vs charger USB-C shell"))
    worst = max(worst, overlap(lid, oled, "lid vs OLED module"))
    worst = max(worst, overlap(shell, oled, "shell vs OLED module"))
    worst = max(worst, overlap(oled, cell, "OLED module vs cell"))
    for i, s in enumerate(standoffs):
        worst = max(worst, overlap(shell, s, f"shell vs standoff {i}"))
        worst = max(worst, overlap(board, s, f"Nano board vs standoff {i}"))

    print("Port axes")
    ok = True
    for x, z in B.SWITCH_CENTERS_XZ_MM:
        ok &= clear_ray(shell, [x, 0.0, z], [0, -1, 0], f"switch bore at x={x}")
    sd = B.MICROSD_SOCKET_X_MM
    ok &= clear_ray(shell, [sum(sd) / 2, 0.0, -0.75], [0, -1, 0], "microSD card path")
    ok &= clear_ray(shell, [sum(B.USB_A_X_MM) / 2, 40.0, 8.0], [0, 1, 0], "USB-A axis")
    ok &= clear_ray(shell, [L.charger_usb_c_x, 40.0, B.CHARGER_PCB_THICKNESS_MM + B.CHARGER_USB_C_HEIGHT_MM / 2],
                    [0, 1, 0], "charger USB-C axis")
    face = round(B.ETHERNET_NOSE_Y_MM + B.ETHERNET_CLEARANCE_MM, 2)
    ok &= clear_ray(shell, [sum(B.ETHERNET_X_MM) / 2, 40.0, 8.0], [0, 1, 0], "Ethernet (must be blind)",
                    expect=[face, round(L.y_max, 2)])

    print(f"cell top to OLED underside (incl. 2 mm solder): {oled_z0 - 2.0 - (L.floor_z + B.BATTERY_SIZE_MM[2]):.2f} mm")
    if worst >= 0.05 or not ok:
        print("FAIL")
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
