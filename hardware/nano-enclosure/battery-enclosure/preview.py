#!/usr/bin/env python3
"""Render the preview PNGs in this directory from the layout and the STLs.

    preview-layout.png         plan view with every part and opening labelled
    preview-shell.png          the shell, from above the -Y face
    preview-lid-underside.png  the lid flipped, showing the OLED frame and bosses

Run after build_battery_enclosure.py. Needs matplotlib in addition to the
builder's dependencies.
"""

from __future__ import annotations

import sys
from pathlib import Path

import matplotlib
import numpy as np
import trimesh

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.patches import Circle, FancyBboxPatch, Rectangle  # noqa: E402
from mpl_toolkits.mplot3d.art3d import Poly3DCollection  # noqa: E402

sys.path.insert(0, str(Path(__file__).parent))
import build_battery_enclosure as B  # noqa: E402

HERE = Path(__file__).parent
PKG = HERE / "print-package"


def rect(ax, x0, y0, x1, y1, **kw):
    ax.add_patch(Rectangle((x0, y0), x1 - x0, y1 - y0, **kw))


def label(ax, x, y, text, **kw):
    kw.setdefault("ha", "center")
    kw.setdefault("va", "center")
    kw.setdefault("fontsize", 8)
    ax.text(x, y, text, **kw)


def plan_view(L: B.Layout, path: Path) -> None:
    fig, ax = plt.subplots(figsize=(13.5, 8.2))
    w, h, z = L.x_max - L.x_min, L.y_max - L.y_min, L.lid_top_z - L.bottom_z
    ax.set_title(f"RevLink Sidecar battery enclosure — plan view, {w:.1f} x {h:.1f} x {z:.1f} mm assembled")

    ax.add_patch(FancyBboxPatch((L.x_min, L.y_min), w, h, boxstyle="round,pad=0,rounding_size=3",
                                fc="#d9d9d9", ec="#555", lw=1.2))
    for bay, fc in ((L.board, "#eef4ea"), (L.battery, "#f7e9e9")):
        ax.add_patch(FancyBboxPatch((bay.x0, bay.y0), bay.x1 - bay.x0, bay.y1 - bay.y0,
                                    boxstyle="round,pad=0,rounding_size=1.5", fc=fc, ec="#777", lw=0.8))
    label(ax, 12.0, L.board.y1 - 2.0, "Nano bay", color="#888")
    label(ax, L.battery.cx, L.battery.y1 - 2.5, "cell bay", color="#888")

    # Nano and its features.
    rect(ax, 0, 0, 50, 50, fc="#cfe6c4", ec="#2f7f3f", lw=1.0)
    label(ax, 10.5, 30.0, "ESP32-P4-NANO\n50 x 50", fontsize=8)
    for (x0, x1) in B.NANO_HEADER_X_MM:
        rect(ax, x0, B.NANO_HEADER_Y_MM[0], x1, B.NANO_HEADER_Y_MM[1], fc="#333", ec="none")
    label(ax, sum(B.NANO_HEADER_X_MM[0]) / 2, 20, "P2", color="w", rotation=90, fontsize=7)
    label(ax, sum(B.NANO_HEADER_X_MM[1]) / 2, 20, "P1", color="w", rotation=90, fontsize=7)
    rect(ax, *B.USB_A_X_MM[:1], B.USB_A_BODY_Y0_MM, B.USB_A_X_MM[1], B.USB_A_NOSE_Y_MM, fc="#888", ec="#444")
    label(ax, sum(B.USB_A_X_MM) / 2, 42, "USB-A", color="w", rotation=90, fontsize=7)
    rect(ax, B.ETHERNET_X_MM[0], B.ETHERNET_BODY_Y0_MM, B.ETHERNET_X_MM[1], B.ETHERNET_NOSE_Y_MM, fc="#aaa", ec="#444")
    label(ax, sum(B.ETHERNET_X_MM) / 2, 41, "RJ45\n(blind)", fontsize=7)
    rect(ax, 6, 36, 17, 46, fc="#efe6b0", ec="#8a7a2a")
    label(ax, 11.5, 41, "C6\nantenna", fontsize=7)
    (cx0, cx1), (cy0, cy1), _ = B.NANO_MODULE_CAN_MM
    rect(ax, cx0, cy0, cx1, cy1, fc="#b9b9b9", ec="#444")
    label(ax, (cx0 + cx1) / 2, 12, "module\ncan 7.2", fontsize=7)
    (px0, px1), (py0, py1), _ = B.NANO_MID_PINS_MM
    rect(ax, px0, py0 - 0.8, px1, py1 + 0.8, fc="#333", ec="none")
    rect(ax, B.NANO_USB_C_X_MM[0], -1.0, B.NANO_USB_C_X_MM[1], 7.5, fc="#999", ec="#444")
    label(ax, 25, 3.3, "USB-C\n(blind)", color="w", fontsize=6.5)
    rect(ax, B.MICROSD_SOCKET_X_MM[0], 0, B.MICROSD_SOCKET_X_MM[1], 12, fc="none", ec="#333", ls="--")
    label(ax, sum(B.MICROSD_SOCKET_X_MM) / 2, 6, "microSD\n(under)", fontsize=7)
    for (x, _), name in zip(B.SWITCH_CENTERS_XZ_MM, ("RST", "BOOT")):
        rect(ax, x - 2, -0.5, x + 2, 2.5, fc="#c33", ec="#600")
        label(ax, x, 4.2, name, fontsize=7)
    for x, y in L.nano_holes:
        ax.add_patch(Circle((x, y), 2.3, fc="#f4d35e", ec="#8a6d00"))

    # Charger, hanging from the lid above the Nano.
    c = L.charger
    rect(ax, c.x0, c.y0, c.x1, c.y1, fc="#9fc0e8", ec="#1f4e8a", lw=1.2, alpha=0.85, hatch="///")
    rect(ax, c.x0 - B.CHARGER_JST_OVERHANG_MM, L.charger_jst_y - B.CHARGER_JST_WIDTH_MM / 2,
         c.x0 + 6.0, L.charger_jst_y + B.CHARGER_JST_WIDTH_MM / 2, fc="#e6c78a", ec="#7a5a10")
    label(ax, c.x0 + 2.6, L.charger_jst_y, "JST", fontsize=6.5)
    rect(ax, L.charger_usb_c_x - B.CHARGER_USB_C_WIDTH_MM / 2, c.y0 - B.CHARGER_USB_C_OVERHANG_MM,
         L.charger_usb_c_x + B.CHARGER_USB_C_WIDTH_MM / 2, c.y0 + 6.5, fc="#777", ec="#333")
    label(ax, L.charger_usb_c_x, c.y0 + 3.0, "USB-C", color="w", fontsize=6.5)
    for x, y in L.charger_output_pads:
        ax.add_patch(Circle((x, y), 0.9, fc="#7fbf7f", ec="#264"))
    label(ax, sum(p[0] for p in L.charger_output_pads) / 2, L.charger_output_pads[0][1] + 2.4, "5 V out", fontsize=6.5)
    label(ax, c.x1 - 6.0, 12.0,
          f"Adafruit 6106\n{B.CHARGER_PCB_SIZE_MM[1]} x {B.CHARGER_PCB_SIZE_MM[0]}\n"
          f"on lid bosses\nz {L.charger_pcb_z:.1f}–{L.charger_part_top_z:.1f}\n(above the Nano)", fontsize=7)
    for x, y in L.charger_holes:
        ax.add_patch(Circle((x, y), B.CHARGER_BOSS_DIAMETER_MM / 2, fc="#f4d35e", ec="#8a6d00"))

    # Cell and OLED.
    cl = B.BATTERY_CLEARANCE_MM / 2
    rect(ax, L.battery.x0 + cl, L.battery.y0 + cl, L.battery.x0 + cl + B.BATTERY_SIZE_MM[0],
         L.battery.y0 + cl + B.BATTERY_SIZE_MM[1], fc="#f0c8c8", ec="#a33", lw=1.0)
    label(ax, L.battery.cx, L.battery.y1 - 6, f"LP103454 cell {B.BATTERY_SIZE_MM[0]:.0f} x {B.BATTERY_SIZE_MM[1]:.0f}", fontsize=7.5)
    ox, oy = L.oled_center
    rect(ax, ox - B.OLED_PCB_SIZE_MM[0] / 2, oy - B.OLED_PCB_SIZE_MM[1] / 2,
         ox + B.OLED_PCB_SIZE_MM[0] / 2, oy + B.OLED_PCB_SIZE_MM[1] / 2, fc="none", ec="#222", ls="--")
    label(ax, ox, oy + B.OLED_PCB_SIZE_MM[1] / 2 - 2.2, "OLED PCB pocket (above cell)", fontsize=7)
    rect(ax, ox - B.OLED_ACTIVE_SIZE_MM[0] / 2, oy - B.OLED_ACTIVE_SIZE_MM[1] / 2,
         ox + B.OLED_ACTIVE_SIZE_MM[0] / 2, oy + B.OLED_ACTIVE_SIZE_MM[1] / 2, fc="#1e1e2e", ec="#000")
    label(ax, ox, oy, f"OLED\nwindow\n{B.OLED_ACTIVE_SIZE_MM[0]} x {B.OLED_ACTIVE_SIZE_MM[1]}", color="w", fontsize=7)
    for x, y in L.wall_screws:
        ax.add_patch(Circle((x, y), 1.35, fc="#f4d35e", ec="#8a6d00"))

    # Rib notch and plunger guide block.
    rect(ax, L.board.x1, L.charger_jst_y - B.RIB_NOTCH_WIDTH_MM / 2, L.battery.x0,
         L.charger_jst_y + B.RIB_NOTCH_WIDTH_MM / 2, fc="#fff", ec="#555")
    m = B.BUTTON_GUIDE_BLOCK_MARGIN_MM
    sx = [x for x, _ in B.SWITCH_CENTERS_XZ_MM]
    rect(ax, min(sx) - m, L.y_min, max(sx) + m, L.button_guide_y1, fc="#c4c4c4", ec="#555")

    label(ax, L.x_min + 1, L.y_max + 4, "+Y face: USB-A, blind RJ45", ha="left")
    label(ax, L.x_min + 1, L.y_min - 4, "-Y face: RST, BOOT, charger USB-C, blind Nano USB-C, microSD slot", ha="left")
    label(ax, L.x_max - 1, L.y_max + 4,
          f"lid screws: {len(L.nano_holes) + len(L.wall_screws)} from above, {len(L.charger_holes)} into bosses from below",
          ha="right")
    ax.set_xlim(L.x_min - 5, L.x_max + 5)
    ax.set_ylim(L.y_min - 8, L.y_max + 8)
    ax.set_aspect("equal")
    ax.set_xlabel("x (mm, Nano PCB frame)")
    ax.set_ylabel("y (mm)")
    fig.tight_layout()
    fig.savefig(path, dpi=110)
    plt.close(fig)


def render(mesh: trimesh.Trimesh, path: Path, title: str, elev: float, azim: float, flip: bool = False) -> None:
    m = mesh.copy()
    if flip:
        m.apply_transform(trimesh.transformations.rotation_matrix(np.pi, (1.0, 0.0, 0.0)))
    tri = m.vertices[m.faces]
    light = np.array([0.4, -0.6, 0.7])
    light /= np.linalg.norm(light)
    shade = 0.45 + 0.55 * np.clip(m.face_normals @ light, 0, 1)
    colors = np.stack([shade * 0.72, shade * 0.75, shade * 0.80, np.ones_like(shade)], axis=1)
    fig = plt.figure(figsize=(12, 6.5))
    ax = fig.add_subplot(111, projection="3d")
    ax.add_collection3d(Poly3DCollection(tri, facecolors=colors, edgecolors="none"))
    lo, hi = m.bounds
    ax.set_xlim(lo[0], hi[0])
    ax.set_ylim(lo[1], hi[1])
    ax.set_zlim(lo[2], hi[2])
    ax.set_box_aspect(hi - lo)
    ax.view_init(elev=elev, azim=azim)
    ax.set_axis_off()
    ax.set_title(title)
    fig.tight_layout()
    fig.savefig(path, dpi=110)
    plt.close(fig)


def main() -> None:
    L = B.compute_layout()
    plan_view(L, HERE / "preview-layout.png")
    shell = trimesh.load_mesh(PKG / "01-revlink-sidecar-battery-shell.stl")
    lid = trimesh.load_mesh(PKG / "02-revlink-sidecar-battery-lid-oled.stl")
    render(shell, HERE / "preview-shell.png", "Battery shell, from above the -Y (RESET/BOOT/charger USB-C) face",
           elev=38, azim=-125)
    render(lid, HERE / "preview-lid-underside.png", "Lid, underside (OLED pocket frame, charger bosses)",
           elev=38, azim=-125, flip=True)
    for name in ("preview-layout.png", "preview-shell.png", "preview-lid-underside.png"):
        print(f"wrote {HERE / name}")


if __name__ == "__main__":
    main()
