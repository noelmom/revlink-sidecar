#!/usr/bin/env python3
"""Interference check for the battery enclosure.

Booleans the generated shell and lid against the Nano board model, a
cell-sized block, the charger envelope (PCB, parts, JST and USB-C overhangs,
screw heads and solder tails underneath), the OLED module and the four brass
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


def charger_envelope(L: B.Layout) -> trimesh.Trimesh:
    """Everything the 6106 occupies, in the case frame."""
    c = L.charger
    zu, zt, zp = L.charger_pcb_z, L.charger_pcb_top_z, L.charger_part_top_z
    pcb = B.box(c.x0, c.y0, zu, c.x1, c.y1, zt - EPS)
    # Parts above the PCB, minus the columns where the lid bosses land.
    parts = trimesh.boolean.difference(
        [B.box(c.x0, c.y0, zt + EPS, c.x1, c.y1, zp),
         B.union([B.cylinder_z(x, y, zt - 1.0, zp + 1.0, B.CHARGER_BOSS_DIAMETER_MM / 2 + 0.1)
                  for x, y in L.charger_holes])],
        engine="manifold",
    )
    jst = B.box(c.x0 - B.CHARGER_JST_OVERHANG_MM, L.charger_jst_y - B.CHARGER_JST_WIDTH_MM / 2, zt + EPS,
                c.x0 + 0.5, L.charger_jst_y + B.CHARGER_JST_WIDTH_MM / 2, zp)
    usb_c = B.box(L.charger_usb_c_x - B.CHARGER_USB_C_WIDTH_MM / 2, c.y0 - B.CHARGER_USB_C_OVERHANG_MM, zt + EPS,
                  L.charger_usb_c_x + B.CHARGER_USB_C_WIDTH_MM / 2, c.y0 + 1.0, zt + B.CHARGER_USB_C_HEIGHT_MM)
    dk, k = B.CHARGER_SCREW_HEAD_MM
    heads = [B.cylinder_z(x, y, zu - k, zu - EPS, dk / 2) for x, y in L.charger_holes]
    tails = B.box(c.x0, c.y0, zu - B.CHARGER_UNDERSIDE_DEPTH_MM, c.x1, c.y1, zu - EPS)
    return B.union([pcb, parts, jst, usb_c, *heads, tails])


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

    charger = charger_envelope(L)

    # The module is centred on its PCB, which is offset from the lit centre the
    # window is cut around - modelling it at oled_center would place it 2 mm out.
    oled_z0 = L.top_z - B.OLED_MODULE_THICKNESS_MM
    pcx = L.oled_center[0] - B.OLED_ACTIVE_OFFSET_MM[0]
    pcy = L.oled_center[1] - B.OLED_ACTIVE_OFFSET_MM[1]
    oled = B.box(pcx - B.OLED_PCB_SIZE_MM[0] / 2, pcy - B.OLED_PCB_SIZE_MM[1] / 2,
                 oled_z0 - 2.0, pcx + B.OLED_PCB_SIZE_MM[0] / 2,
                 pcy + B.OLED_PCB_SIZE_MM[1] / 2, L.top_z - EPS)
    # Its four mounting holes, so the locating posts pass through rather than
    # reading as interference.
    oled = B.difference(oled, [
        B.cylinder_z(hx, hy, oled_z0 - 2.0 - 1.0, L.top_z + 1.0, B.OLED_HOLE_DIAMETER_MM / 2)
        for hx, hy in B.oled_hole_positions(L)
    ])

    # 4 mm AF hex standoffs, modelled at their across-corners diameter.
    standoffs = [B.cylinder_z(x, y, B.NANO_PCB_THICKNESS_MM + EPS, L.top_z - EPS, 2.31) for x, y in L.nano_holes]

    print("Overlap volumes")
    worst = 0.0
    worst = max(worst, overlap(shell, board, "shell vs Nano board model"))
    worst = max(worst, overlap(lid, board, "lid vs Nano board model"))
    worst = max(worst, overlap(shell, cell, "shell vs cell"))
    worst = max(worst, overlap(lid, cell, "lid vs cell"))
    worst = max(worst, overlap(shell, charger, "shell vs charger envelope"))
    worst = max(worst, overlap(lid, charger, "lid vs charger envelope"))
    worst = max(worst, overlap(board, charger, "Nano board vs charger envelope"))
    worst = max(worst, overlap(lid, oled, "lid vs OLED module"))
    worst = max(worst, overlap(shell, oled, "shell vs OLED module"))
    worst = max(worst, overlap(oled, cell, "OLED module vs cell"))
    for i, s in enumerate(standoffs):
        worst = max(worst, overlap(shell, s, f"shell vs standoff {i}"))
        worst = max(worst, overlap(board, s, f"Nano board vs standoff {i}"))
        worst = max(worst, overlap(charger, s, f"charger vs standoff {i}"))

    print("Port axes")
    ok = True
    for x, z in B.SWITCH_CENTERS_XZ_MM:
        ok &= clear_ray(shell, [x, 0.0, z], [0, -1, 0], f"switch bore at x={x}")
    sd = B.MICROSD_SOCKET_X_MM
    ok &= clear_ray(shell, [sum(sd) / 2, 0.0, -0.75], [0, -1, 0], "microSD card path")
    ok &= clear_ray(shell, [sum(B.USB_A_X_MM) / 2, 40.0, 8.0], [0, 1, 0], "USB-A axis")
    ok &= clear_ray(shell, [L.charger_usb_c_x, 10.0, L.charger_usb_c_z], [0, -1, 0], "charger USB-C axis")
    face = round(B.ETHERNET_NOSE_Y_MM + B.ETHERNET_CLEARANCE_MM, 2)
    ok &= clear_ray(shell, [sum(B.ETHERNET_X_MM) / 2, 40.0, 8.0], [0, 1, 0], "Ethernet (must be blind)",
                    expect=[face, round(L.y_max, 2)])

    # Gaps that the constants promise, measured on the model.
    v = board.vertices
    cx0 = L.charger.x0 - B.CHARGER_JST_OVERHANG_MM
    under = v[(v[:, 0] >= cx0) & (v[:, 0] <= L.charger.x1) & (v[:, 1] >= L.charger.y0) & (v[:, 1] <= L.charger.y1)]
    nano_top = float(under[:, 2].max()) - EPS
    under_heads = max(
        float(v[np.hypot(v[:, 0] - x, v[:, 1] - y) <= B.CHARGER_SCREW_HEAD_MM[0] / 2][:, 2].max(initial=-9.0)) - EPS
        for x, y in L.charger_holes
    )
    print(f"Nano tallest part under the charger: {nano_top:.2f} mm (constant {B.NANO_MIDBOARD_PART_HEIGHT_MM})")
    print(f"  to the charger's solder tails: {L.charger_pcb_z - B.CHARGER_UNDERSIDE_DEPTH_MM - nano_top:.2f} mm")
    print(f"  under the screw heads: {L.charger_bottom_z - under_heads:.2f} mm")
    print(f"charger tallest part to lid underside: {L.top_z - L.charger_part_top_z:.2f} mm")
    print(f"cell top to OLED underside (incl. 2 mm solder): {oled_z0 - 2.0 - (L.floor_z + B.BATTERY_SIZE_MM[2]):.2f} mm")
    if nano_top > B.NANO_MIDBOARD_PART_HEIGHT_MM + 1e-6:
        print("FAIL: NANO_MIDBOARD_PART_HEIGHT_MM is lower than the board model under the charger")
        return 1
    if worst >= 0.05 or not ok:
        print("FAIL")
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
