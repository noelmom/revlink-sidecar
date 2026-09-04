#!/usr/bin/env python3
"""Build the RevLink Sidecar battery enclosure: shell, OLED lid, side buttons.

Issue #23. Holds the ESP32-P4-NANO, the 1.3-inch SH1106 OLED, the Adafruit
6106 charger and an EEMB LP103454 cell in two floor-level bays side by side.
The charger hangs from the lid above the Nano's -Y half, between the two
2x13 headers; the OLED hangs from the lid above the cell:

    +X ->   [ Nano, 6106 above its -Y half ] rib [ cell, OLED above ]

Every part of this file is authored from scratch. No donor STL is loaded
or reused; the Nano dimensions below were *measured* from the board model
and are facts about the Waveshare board, not copied geometry.

Coordinate frame = the Nano PCB frame: PCB x 0..50, y 0..50, PCB underside
at z = 0, +Y is the USB-A / Ethernet edge, -Y is the USB-C / RESET / BOOT
edge. The case extends around that.

Requires Python 3.11+, numpy, trimesh, shapely, manifold3d, lxml.
"""

from __future__ import annotations

import argparse
import math
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import trimesh
from shapely.geometry import Polygon, box as shapely_box

# ===========================================================================
# VERIFIED — ESP32-P4-NANO. Read from the board model at 0.1 mm resolution
# (planar sections; see the README for the method) and cross-checked against
# the constants the shipped 55 mm lid already uses. Trust these.
# ===========================================================================
NANO_PCB_SIZE_MM = (50.0, 50.0)
NANO_PCB_THICKNESS_MM = 1.6
NANO_HOLE_INSET_MM = 2.45           # four holes, diameter 2.5, 45.1 mm pitch
NANO_UNDERSIDE_DEPTH_MM = 2.25      # deepest bottom-side part
NANO_PIN_TOP_Z_MM = 10.5            # header pin tips (10.2 in the model, +0.3 margin)

USB_A_X_MM = (19.7, 27.1)           # vertical receptacle on the +Y edge
USB_A_Z_MM = (0.0, 15.8)             # the shell flange reaches the PCB underside
USB_A_NOSE_Y_MM = 51.6
USB_A_BODY_Y0_MM = 32.5             # the receptacle body starts here

ETHERNET_X_MM = (28.0, 44.0)        # RJ45, concealed
ETHERNET_TOP_Z_MM = 15.3
ETHERNET_NOSE_Y_MM = 51.0
ETHERNET_BODY_Y0_MM = 30.0

NANO_USB_C_X_MM = (20.4, 29.6)      # on the -Y edge, concealed
NANO_USB_C_NOSE_Y_MM = -1.0

SWITCH_CENTERS_XZ_MM = ((7.75, 3.25), (13.25, 3.25))   # RESET, BOOT
SWITCH_ACTUATOR_Y_MM = -0.5         # actuator tip, protrudes past the PCB

MICROSD_SOCKET_X_MM = (32.0, 43.5)  # underside, flush with the -Y edge
MICROSD_SOCKET_Z_MM = (-1.5, 0.0)

# What stands on the Nano's -Y half, where the charger goes. Everything
# between the headers and south of the mid pins is <= 5.0 mm tall (the
# Nano's own USB-C receptacle); the module can and the two pin rows are the
# only taller things nearby and the charger is placed clear of them in plan.
NANO_HEADER_X_MM = ((1.25, 4.39), (45.61, 48.75))   # P2 (nearest RESET), P1
NANO_HEADER_Y_MM = (6.2, 37.28)
NANO_MODULE_CAN_MM = ((32.5, 42.0), (1.0, 23.0), 7.2)      # x, y, top z
NANO_MID_PINS_MM = ((17.4, 25.6), (29.37, 29.97), 10.2)   # 1x4 row before the USB-A
NANO_AUX_PINS_MM = ((6.7, 7.3), (6.2, 9.3), 10.2)         # 1x2 row beside P2
NANO_MIDBOARD_PART_HEIGHT_MM = 5.0                        # tallest part elsewhere

# ===========================================================================
# VERIFIED — cell and display
# ===========================================================================
# MEASURED on the cell in hand. Note X and Y exceed the LP103454's nominal
# 34 x 54 by 0.30 and 0.54 while Z comes in 0.55 under 10.3 - so the datum
# is the article, not the datasheet. Basing the bay on the nominal left
# only 0.46 mm of the 1.0 mm swell allowance in Y, the clearance quietly
# paying for tolerance instead of swell.
BATTERY_SIZE_MM = (34.30, 54.54, 9.75)        # EEMB LP103454, X × Y × Z here
OLED_PCB_SIZE_MM = (33.43, 35.53)             # MEASURED, X × Y as installed (portrait)
OLED_ACTIVE_SIZE_MM = (15.5, 30.2)            # lit area, portrait

# ===========================================================================
# Adafruit 6106. Outline, hole and connector positions come from Adafruit's
# published EagleCAD board file (github.com/adafruit/Adafruit-bq25185-with-
# 5V-Boost-PCB, commit 2ce979f); the package names in that file identify the
# connectors (USB-C CUSB31-CFM2AX-01 top-mount, JST PH S2B-PH-SM4-TB surface
# mount, VLC5045-footprint inductor, 3.5 mm terminal block through-hole).
#
# MEASURED on the physical board (2026-09): length 29.43, width 19.87 across
# the JST / 18.86 across bare PCB, 4.76 total at the USB-C, 7.04 total over
# the tallest connector *with the terminal block still fitted*.
#
# CONFIRMED: the 7.04 reading was taken across the JST, not the terminal
# block, so it measures the JST directly. The JST is SMT with no underside
# tails, making it 7.04 - 1.6 = 5.44 above the PCB top. That also exceeds the
# 4.5 inductor, so the JST is the governing part and stays so once the
# terminal block is removed (two wires are soldered in its place to the
# Nano's P2 pins 1 and 3).
#
# Distributor listings for an S2B-PH-SM4-TB quote heights that will not fit
# inside 7.04. The physical board wins; the part may not be that exact
# variant. Everything derived from this constant (charger height, lid boss
# height, USB-C cutout) recomputes on rebuild, and the layout refuses above
# 6.40 - swept against the guard, so there is ~1 mm of unused slack.
# ===========================================================================
CHARGER_PCB_SIZE_MM = (29.21, 19.05)          # Eagle outline = 1.150 × 0.750 in
CHARGER_PCB_THICKNESS_MM = 1.6
# CONFIRMED against the board: centres measured 24.13 (long, exact) and 13.94
# (short, 0.03 under the 13.97 these give), inset 2.54 on every edge. Kept at
# the Eagle values rather than refitted to the 0.03 - that is caliper
# technique, and the imperial grid is more trustworthy than one reading.
CHARGER_HOLES_MM = ((2.54, 2.54), (26.67, 2.54), (2.54, 16.51), (26.67, 16.51))
CHARGER_HOLE_DIAMETER_MM = 2.52               # MEASURED; near-zero play on M2.5
CHARGER_USB_C_CENTER_MM = 9.525               # along charger y, on the x = 0 edge
CHARGER_USB_C_OVERHANG_MM = 1.14              # shell past the PCB edge; confirm
CHARGER_USB_C_HEIGHT_MM = 3.16                # MEASURED 4.76 - 1.6: top-mount
CHARGER_USB_C_WIDTH_MM = 8.94
CHARGER_JST_CENTER_MM = 13.97                 # along charger x, on the y = 19.05 edge
CHARGER_JST_WIDTH_MM = 8.0                    # footprint outline along charger x
CHARGER_JST_OVERHANG_MM = 0.82                # MEASURED 19.87 - 19.05, mating face past the edge
CHARGER_MAX_PART_HEIGHT_MM = 5.44             # MEASURED, the JST: 7.04 - 1.6
CHARGER_UNDERSIDE_DEPTH_MM = 1.0              # USB-C shell legs and the two wire tails, trimmed
CHARGER_SCREW_HEAD_MM = (5.0, 2.0)            # M2.5 pan head (dk, k) under the PCB

# ===========================================================================
# PROVISIONAL — SH1106 module details the datasheet does not give
# ===========================================================================
OLED_MODULE_THICKNESS_MM = 3.59     # MEASURED, glass face to PCB back, no pins
# MEASURED. Margins from the PCB edge to the lit area came out 10.98 and 6.95
# across X, so the lit centre sits 2.015 from the PCB centre - the module is
# not symmetric, and the previous (0.0, 0.0) put the window 2 mm out. Y is off
# by 0.085, which is nothing. Signs assume the module is installed with its
# pad edge toward -Y and its narrow-margin side toward +X; rotating it 180
# degrees flips both, and the firmware can flip the image to match.
#
# Installed pad edge toward -Y, narrow-margin side toward +X.
OLED_ACTIVE_OFFSET_MM = (2.015, -0.085)  # lit-area centre relative to PCB centre
# The window was cut at exactly the lit area, leaving nothing for the 0.6 mm
# of pocket play. Relief is capped by the Y glass border, 0.51 per side, past
# which the window would show bare PCB rather than glass.
OLED_WINDOW_RELIEF_MM = 0.35        # per side, added around the lit area
# MEASURED. Symmetric about the PCB centre, so they are a clean datum.
OLED_HOLE_SPACING_MM = (27.98, 30.11)
OLED_HOLE_DIAMETER_MM = 3.00
OLED_POST_DIAMETER_MM = 2.80        # 0.2 clearance: +/-0.1 vs the pocket's +/-0.3
OLED_WIRE_EXIT_WIDTH_MM = 10.0      # notch in both Y ends of the pocket frame

# ===========================================================================
# Design parameters
# ===========================================================================
WALL_MM = 2.0
FLOOR_MM = 2.0
LID_MM = 2.0
POST_HEIGHT_MM = 3.0                # floor to Nano PCB underside
STANDOFF_LENGTH_MM = 15.0           # M2.5 male/female brass, PCB top to lid
USB_A_MIN_ROOF_CLEARANCE_MM = 0.5

NANO_XY_CLEARANCE_MM = 0.5
NANO_PLUS_Y_CLEARANCE_MM = 0.6      # PCB edge to +Y wall inner face
USB_C_CONCEAL_CLEARANCE_MM = 0.8    # Nano USB-C nose to -Y wall inner face, minimum
BATTERY_CLEARANCE_MM = 1.0          # total per axis; pouch cells swell in service
CHARGER_CLEARANCE_MM = 0.5          # charger PCB edge to wall / to Nano parts, in plan
CHARGER_ROOF_CLEARANCE_MM = 0.6     # tallest charger part to the lid underside
CHARGER_NANO_CLEARANCE_MM = 1.0     # charger underside envelope to the Nano's parts, minimum
OLED_POCKET_CLEARANCE_MM = 0.6      # total, per axis

RIB_MM = 2.0
RIB_TOP_GAP_MM = 2.5                # rib top below the lid, wire pass
RIB_NOTCH_WIDTH_MM = 6.0
RIB_NOTCH_DEPTH_MM = 6.0

SCREW_WALL_MM = 5.85                # right-hand wall carrying two lid screws
POST_HOLE_DIAMETER_MM = 2.05        # M2.5 tap drill / thread-forming
POST_HOLE_DEPTH_MM = 4.6
LID_SCREW_CLEARANCE_MM = 2.7
NANO_POST_DIAMETER_MM = 5.0
CHARGER_BOSS_DIAMETER_MM = 4.6      # lid bosses; clears the USB-C shell by the 2.54 hole

BAY_CORNER_RADIUS_MM = 1.5          # Ø3 end mill
OUTER_CORNER_RADIUS_MM = 3.0

USB_A_OPENING_CLEARANCE_MM = 0.5
ETHERNET_FACE_MM = 1.1
ETHERNET_CLEARANCE_MM = 0.5
CHARGER_USB_C_CUTOUT_MM = (13.0, 7.0, 1.5)    # USB-C plug overmold envelope
MIN_ROOF_OVER_CUTOUT_MM = 1.0       # thinner than this and the cutout becomes a top-open notch
NANO_USB_C_SERVICE_OPENING = False      # True: expose the Nano's USB-C for reflashing
NANO_USB_C_CUTOUT_MM = (13.0, 7.0, 1.5)
NANO_USB_C_HEIGHT_MM = 3.26
MICROSD_SLOT_X_MM = (31.0, 44.5)
MICROSD_SLOT_TOP_Z_MM = 0.3
MICROSD_SCOOP_DEPTH_MM = 1.0
MICROSD_SCOOP_MARGIN_MM = 1.5
OLED_FRAME_MM = 1.2
OLED_WINDOW_RADIUS_MM = 1.0
BATTERY_LEDGE_HEIGHT_MM = 2.0

# Side buttons (RESET, BOOT) — the same concept as the shipped lid: a printed
# plunger in a plain bore, retained by a small flange that snaps through.
# The -Y wall is 2 mm; a block on its inner face extends the bore to
# BUTTON_GUIDE_MM so the plunger stays square to the switch.
BUTTON_GUIDE_MM = 4.6
BUTTON_GUIDE_BLOCK_MARGIN_MM = 2.5
BUTTON_BORE_RADIUS_MM = 1.0
BUTTON_SHAFT_RADIUS_MM = 0.78
BUTTON_CAP_SIZE_MM = (3.8, 3.0)
BUTTON_CAP_RADIUS_MM = 0.5
BUTTON_CAP_DEPTH_MM = 0.9
BUTTON_RETAINER_RADIUS_MM = 1.12
BUTTON_RETAINER_DEPTH_MM = 0.5
BUTTON_RETAINER_WALL_CLEARANCE_MM = 0.1
BUTTON_TIP_CLEARANCE_MM = 0.05

SEGMENTS = 48


# ===========================================================================
# Derived layout
# ===========================================================================
@dataclass(frozen=True)
class Bay:
    x0: float
    x1: float
    y0: float
    y1: float

    @property
    def cx(self) -> float:
        return (self.x0 + self.x1) / 2

    @property
    def cy(self) -> float:
        return (self.y0 + self.y1) / 2


@dataclass(frozen=True)
class Layout:
    board: Bay
    battery: Bay
    charger: Bay            # the charger PCB outline in the case frame
    x_min: float
    x_max: float
    y_min: float
    y_max: float
    floor_z: float          # bay floor (top of the floor slab)
    bottom_z: float         # outside bottom
    top_z: float            # shell top = lid underside
    lid_top_z: float
    charger_pcb_z: float    # charger PCB underside
    charger_pcb_top_z: float
    charger_part_top_z: float
    charger_bottom_z: float     # lowest point of the charger's underside envelope (screw heads)
    charger_origin: tuple[float, float]   # case-frame position of charger (0, 0)
    charger_holes: tuple[tuple[float, float], ...]
    charger_usb_c_x: float
    charger_usb_c_z: float      # centre of the receptacle shell
    charger_usb_c_notch: bool   # True: top-open notch closed by the lid
    charger_jst_y: float
    charger_output_pads: tuple[tuple[float, float], ...]
    nano_holes: tuple[tuple[float, float], ...]
    wall_screws: tuple[tuple[float, float], ...]
    oled_center: tuple[float, float]
    battery_ledge_x0: float
    button_guide_y1: float


def oled_hole_positions(layout) -> tuple[tuple[float, float], ...]:
    """The four OLED mounting holes in case coordinates."""
    pcx = layout.oled_center[0] - OLED_ACTIVE_OFFSET_MM[0]
    pcy = layout.oled_center[1] - OLED_ACTIVE_OFFSET_MM[1]
    sx, sy = OLED_HOLE_SPACING_MM
    return tuple((pcx + dx, pcy + dy)
                 for dx in (-sx / 2, sx / 2) for dy in (-sy / 2, sy / 2))


def charger_to_case(origin: tuple[float, float], cx: float, cy: float) -> tuple[float, float]:
    """Charger frame -> case frame.

    The 6106 is rotated +90 deg: its USB-C edge (charger x = 0) faces the
    -Y wall, its long axis runs along +Y, its JST edge (charger y = 19.05)
    faces -X toward P2 and its header edge (charger y = 0) faces +X toward
    the module can and the cell.
    """
    x1, y0 = origin
    return (x1 - cy, y0 + cx)


def compute_layout() -> Layout:
    top_z = NANO_PCB_THICKNESS_MM + STANDOFF_LENGTH_MM
    if top_z < USB_A_Z_MM[1] + USB_A_MIN_ROOF_CLEARANCE_MM:
        raise ValueError("standoff too short: USB-A would touch the lid")
    if POST_HEIGHT_MM < NANO_UNDERSIDE_DEPTH_MM + 0.5:
        raise ValueError("posts too short for the Nano's underside parts")

    # Both bays share the same Y extent, set by the cell.
    bay_y1 = NANO_PCB_SIZE_MM[1] + NANO_PLUS_Y_CLEARANCE_MM
    bay_y0 = bay_y1 - (BATTERY_SIZE_MM[1] + BATTERY_CLEARANCE_MM)
    if bay_y0 > NANO_USB_C_NOSE_Y_MM - USB_C_CONCEAL_CLEARANCE_MM:
        raise ValueError("-Y wall too close to the Nano's concealed USB-C")

    board = Bay(
        x0=-NANO_XY_CLEARANCE_MM,
        x1=NANO_PCB_SIZE_MM[0] + NANO_XY_CLEARANCE_MM,
        y0=bay_y0,
        y1=bay_y1,
    )

    oled_pocket_x = OLED_PCB_SIZE_MM[0] + OLED_POCKET_CLEARANCE_MM
    battery_width = max(
        BATTERY_SIZE_MM[0] + BATTERY_CLEARANCE_MM,
        oled_pocket_x + 2 * OLED_FRAME_MM + OLED_POCKET_CLEARANCE_MM,
    )
    battery_x0 = board.x1 + RIB_MM
    battery = Bay(x0=battery_x0, x1=battery_x0 + battery_width, y0=bay_y0, y1=bay_y1)

    x_min = board.x0 - WALL_MM
    x_max = battery.x1 + SCREW_WALL_MM
    y_min = bay_y0 - WALL_MM
    y_max = bay_y1 + WALL_MM
    floor_z = -POST_HEIGHT_MM
    bottom_z = floor_z - FLOOR_MM

    # --- Charger: hangs from the lid above the Nano's -Y half. -------------
    # Vertical: as high as its tallest part allows, so the Nano keeps the
    # most room underneath.
    charger_part_top_z = top_z - CHARGER_ROOF_CLEARANCE_MM
    charger_pcb_top_z = charger_part_top_z - CHARGER_MAX_PART_HEIGHT_MM
    charger_pcb_z = charger_pcb_top_z - CHARGER_PCB_THICKNESS_MM
    charger_bottom_z = charger_pcb_z - max(CHARGER_UNDERSIDE_DEPTH_MM, CHARGER_SCREW_HEAD_MM[1])
    boss_height = top_z - charger_pcb_top_z
    if boss_height < POST_HOLE_DEPTH_MM + 0.5:
        raise ValueError("charger too close to the lid for the boss thread depth")
    if charger_bottom_z < NANO_MIDBOARD_PART_HEIGHT_MM + CHARGER_NANO_CLEARANCE_MM:
        raise ValueError(
            f"charger underside at z {charger_bottom_z:.2f} is within "
            f"{CHARGER_NANO_CLEARANCE_MM} mm of the Nano's parts ({NANO_MIDBOARD_PART_HEIGHT_MM}); "
            "CHARGER_MAX_PART_HEIGHT_MM is too large for this height"
        )

    # Plan: USB-C edge against the -Y wall, header edge 0.5 mm short of the
    # module can, JST edge (and its overhang) clear of the P2 header.
    charger_x1 = NANO_MODULE_CAN_MM[0][0] - CHARGER_CLEARANCE_MM
    charger_x0 = charger_x1 - CHARGER_PCB_SIZE_MM[1]
    charger_y0 = board.y0 + CHARGER_CLEARANCE_MM
    charger_y1 = charger_y0 + CHARGER_PCB_SIZE_MM[0]
    charger = Bay(x0=charger_x0, x1=charger_x1, y0=charger_y0, y1=charger_y1)
    if charger_x0 - CHARGER_JST_OVERHANG_MM < NANO_HEADER_X_MM[0][1] + CHARGER_CLEARANCE_MM:
        raise ValueError("charger JST overhangs the P2 header")
    if charger_x0 - CHARGER_JST_OVERHANG_MM < NANO_AUX_PINS_MM[0][1] + CHARGER_CLEARANCE_MM \
            and charger_bottom_z < NANO_AUX_PINS_MM[2] + CHARGER_NANO_CLEARANCE_MM:
        raise ValueError("charger over the auxiliary pin row beside P2")
    corridor = NANO_MID_PINS_MM[1][0] - charger_y1
    if corridor < CHARGER_CLEARANCE_MM:
        raise ValueError("charger +Y end reaches the pin row in front of the USB-A")
    if charger_y1 > min(USB_A_BODY_Y0_MM, ETHERNET_BODY_Y0_MM) - CHARGER_CLEARANCE_MM:
        raise ValueError("charger reaches the USB-A / RJ45 bodies")

    charger_origin = (charger.x1, charger.y0)
    charger_holes = tuple(charger_to_case(charger_origin, hx, hy) for hx, hy in CHARGER_HOLES_MM)
    charger_usb_c_x = charger_to_case(charger_origin, 0.0, CHARGER_USB_C_CENTER_MM)[0]
    charger_usb_c_z = charger_pcb_top_z + CHARGER_USB_C_HEIGHT_MM / 2
    charger_jst_y = charger_to_case(charger_origin, CHARGER_JST_CENTER_MM, CHARGER_PCB_SIZE_MM[1])[1]
    # Terminal-block pads (charger x 25.4, y 9.525 ± 1.75) now carry the two output wires.
    charger_output_pads = tuple(charger_to_case(charger_origin, 25.4, 9.525 + d) for d in (-1.75, 1.75))
    cutout_top = charger_usb_c_z + CHARGER_USB_C_CUTOUT_MM[1] / 2
    charger_usb_c_notch = cutout_top > top_z - MIN_ROOF_OVER_CUTOUT_MM

    inset = NANO_HOLE_INSET_MM
    w, h = NANO_PCB_SIZE_MM
    nano_holes = ((inset, inset), (w - inset, inset), (inset, h - inset), (w - inset, h - inset))

    screw_x = battery.x1 + SCREW_WALL_MM / 2
    span = (battery.y1 - battery.y0) / 2 - 3.0
    wall_screws = ((screw_x, battery.cy - span), (screw_x, battery.cy + span))

    return Layout(
        board=board,
        battery=battery,
        charger=charger,
        x_min=x_min,
        x_max=x_max,
        y_min=y_min,
        y_max=y_max,
        floor_z=floor_z,
        bottom_z=bottom_z,
        top_z=top_z,
        lid_top_z=top_z + LID_MM,
        charger_pcb_z=charger_pcb_z,
        charger_pcb_top_z=charger_pcb_top_z,
        charger_part_top_z=charger_part_top_z,
        charger_bottom_z=charger_bottom_z,
        charger_origin=charger_origin,
        charger_holes=charger_holes,
        charger_usb_c_x=charger_usb_c_x,
        charger_usb_c_z=charger_usb_c_z,
        charger_usb_c_notch=charger_usb_c_notch,
        charger_jst_y=charger_jst_y,
        charger_output_pads=charger_output_pads,
        nano_holes=nano_holes,
        wall_screws=wall_screws,
        # The PCB is what has to fit the bay, so it keeps the bay centre and the
        # window follows the lit area off to one side. Centring the window
        # instead pushes the PCB 2 mm sideways, which fouls the rib in one
        # rotation and the outer wall in the other - the bay has no room for it.
        oled_center=(battery.cx + OLED_ACTIVE_OFFSET_MM[0],
                     battery.cy + OLED_ACTIVE_OFFSET_MM[1]),
        battery_ledge_x0=battery.x0 + BATTERY_SIZE_MM[0] + BATTERY_CLEARANCE_MM,
        button_guide_y1=y_min + BUTTON_GUIDE_MM,
    )


# ===========================================================================
# Geometry helpers
# ===========================================================================
def rounded_rect(x0: float, y0: float, x1: float, y1: float, radius: float) -> Polygon:
    if radius <= 0:
        return shapely_box(x0, y0, x1, y1)
    if 2 * radius > min(x1 - x0, y1 - y0):
        raise ValueError("corner radius larger than half the rectangle")
    return shapely_box(x0 + radius, y0 + radius, x1 - radius, y1 - radius).buffer(
        radius, join_style="round", quad_segs=SEGMENTS // 4
    )


def prism_z(polygon: Polygon, z0: float, z1: float) -> trimesh.Trimesh:
    """Extrude an XY polygon between two Z levels."""
    mesh = trimesh.creation.extrude_polygon(polygon, height=z1 - z0)
    mesh.apply_translation((0.0, 0.0, z0))
    return mesh


def prism_y(polygon_xz: Polygon, y0: float, y1: float) -> trimesh.Trimesh:
    """Extrude an XZ polygon between two Y levels (a wall cutter)."""
    mesh = trimesh.creation.extrude_polygon(polygon_xz, height=y1 - y0)
    # (x, y, z) -> (x, -z, y): the extrusion axis becomes -Y, polygon y becomes Z
    mesh.apply_transform(trimesh.transformations.rotation_matrix(math.pi / 2, (1.0, 0.0, 0.0)))
    mesh.apply_translation((0.0, y1, 0.0))
    return mesh


def box(x0: float, y0: float, z0: float, x1: float, y1: float, z1: float) -> trimesh.Trimesh:
    mesh = trimesh.creation.box(extents=(x1 - x0, y1 - y0, z1 - z0))
    mesh.apply_translation(((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2))
    return mesh


def cylinder_z(cx: float, cy: float, z0: float, z1: float, radius: float) -> trimesh.Trimesh:
    mesh = trimesh.creation.cylinder(radius=radius, height=z1 - z0, sections=SEGMENTS)
    mesh.apply_translation((cx, cy, (z0 + z1) / 2))
    return mesh


def cylinder_y(cx: float, cz: float, y0: float, y1: float, radius: float) -> trimesh.Trimesh:
    mesh = trimesh.creation.cylinder(radius=radius, height=y1 - y0, sections=SEGMENTS)
    mesh.apply_transform(trimesh.transformations.rotation_matrix(math.pi / 2, (1.0, 0.0, 0.0)))
    mesh.apply_translation((cx, (y0 + y1) / 2, cz))
    return mesh


def union(parts: list[trimesh.Trimesh]) -> trimesh.Trimesh:
    result = trimesh.boolean.union(parts, engine="manifold")
    return _checked(result, "union")


def difference(base: trimesh.Trimesh, cutters: list[trimesh.Trimesh]) -> trimesh.Trimesh:
    result = trimesh.boolean.difference([base, union(cutters)], engine="manifold")
    return _checked(result, "difference")


def _checked(mesh, what: str) -> trimesh.Trimesh:
    if mesh is None or not isinstance(mesh, trimesh.Trimesh):
        raise RuntimeError(f"{what}: boolean returned no mesh")
    if not mesh.is_watertight:
        raise RuntimeError(f"{what}: result is not watertight")
    return mesh


def validate(mesh: trimesh.Trimesh, name: str, bodies: int = 1) -> None:
    if not mesh.is_watertight or not mesh.is_winding_consistent or not mesh.is_volume:
        raise RuntimeError(f"{name}: not a closed, consistently wound solid")
    if mesh.body_count != bodies:
        raise RuntimeError(f"{name}: expected {bodies} body(ies), found {mesh.body_count}")


# ===========================================================================
# Shell
# ===========================================================================
def build_shell(L: Layout) -> trimesh.Trimesh:
    over = 1.0  # cutters overshoot every face they cut so no faces are coplanar
    shell = prism_z(
        rounded_rect(L.x_min, L.y_min, L.x_max, L.y_max, OUTER_CORNER_RADIUS_MM),
        L.bottom_z,
        L.top_z,
    )

    # Two top-open bays share one floor.
    bays = [
        prism_z(rounded_rect(b.x0, b.y0, b.x1, b.y1, BAY_CORNER_RADIUS_MM), L.floor_z, L.top_z + over)
        for b in (L.board, L.battery)
    ]
    shell = difference(shell, bays)

    # Posts for the Nano, a low ledge that locates the cell in X, and the
    # plunger guide block on the inner face of the -Y wall (floor to just
    # above the bores: a step in the pocket, no undercut).
    posts = [
        cylinder_z(x, y, L.floor_z - 0.5, L.floor_z + POST_HEIGHT_MM, NANO_POST_DIAMETER_MM / 2)
        for x, y in L.nano_holes
    ]
    ledge = box(
        L.battery_ledge_x0, L.battery.y0 + BAY_CORNER_RADIUS_MM, L.floor_z - 0.5,
        L.battery.x1 + 0.5, L.battery.y1 - BAY_CORNER_RADIUS_MM, L.floor_z + BATTERY_LEDGE_HEIGHT_MM,
    )
    sx = [x for x, _ in SWITCH_CENTERS_XZ_MM]
    sz = [z for _, z in SWITCH_CENTERS_XZ_MM]
    m = BUTTON_GUIDE_BLOCK_MARGIN_MM
    guide = box(min(sx) - m, L.y_min + 0.5, L.floor_z - 0.5, max(sx) + m, L.button_guide_y1, max(sz) + m)
    if L.button_guide_y1 > SWITCH_ACTUATOR_Y_MM - 1.0:
        raise ValueError("plunger guide block reaches the switch actuators")
    if max(sz) + m > L.charger_bottom_z - 0.5 and max(sx) + m > L.charger.x0 - CHARGER_JST_OVERHANG_MM:
        raise ValueError("plunger guide block reaches the charger")
    shell = union([shell, *posts, ledge, guide])

    cutters: list[trimesh.Trimesh] = []

    # Post holes: blind, tapped M2.5 or thread-forming.
    hole_top = L.floor_z + POST_HEIGHT_MM
    for x, y in L.nano_holes:
        cutters.append(cylinder_z(x, y, hole_top - POST_HOLE_DEPTH_MM, hole_top + over, POST_HOLE_DIAMETER_MM / 2))

    # Lid screws in the thick right-hand wall.
    for x, y in L.wall_screws:
        cutters.append(cylinder_z(x, y, L.top_z - POST_HOLE_DEPTH_MM - 2.0, L.top_z + over, POST_HOLE_DIAMETER_MM / 2))

    # USB-A: a top-open notch in the +Y wall, closed by the lid.
    c = USB_A_OPENING_CLEARANCE_MM
    cutters.append(
        prism_y(
            shapely_box(USB_A_X_MM[0] - c, USB_A_Z_MM[0] - c, USB_A_X_MM[1] + c, L.top_z + over),
            L.board.y1 - over,
            L.y_max + over,
        )
    )

    # Ethernet: concealed behind a thin face, top-open recess on the inner side.
    cutters.append(
        box(
            ETHERNET_X_MM[0] - ETHERNET_CLEARANCE_MM, L.board.y1 - over, L.floor_z,
            ETHERNET_X_MM[1] + ETHERNET_CLEARANCE_MM, ETHERNET_NOSE_Y_MM + ETHERNET_CLEARANCE_MM, L.top_z + over,
        )
    )
    if L.y_max - (ETHERNET_NOSE_Y_MM + ETHERNET_CLEARANCE_MM) < ETHERNET_FACE_MM - 1e-6:
        raise ValueError("Ethernet face thinner than ETHERNET_FACE_MM")

    # RESET and BOOT plunger bores through the -Y wall and its guide block.
    for x, z in SWITCH_CENTERS_XZ_MM:
        cutters.append(cylinder_y(x, z, L.y_min - over, L.button_guide_y1 + over, BUTTON_BORE_RADIUS_MM))

    # microSD: floor-level slot through the -Y wall plus a finger scoop outside.
    cutters.append(
        box(MICROSD_SLOT_X_MM[0], L.y_min - over, L.floor_z - 1e-3,
            MICROSD_SLOT_X_MM[1], L.board.y0 + over, MICROSD_SLOT_TOP_Z_MM)
    )
    m = MICROSD_SCOOP_MARGIN_MM
    cutters.append(
        box(MICROSD_SLOT_X_MM[0] - m, L.y_min - over, L.floor_z - 0.5,
            MICROSD_SLOT_X_MM[1] + m, L.y_min + MICROSD_SCOOP_DEPTH_MM, MICROSD_SLOT_TOP_Z_MM + m)
    )

    # Optional service opening for the Nano's own USB-C (concealed by default;
    # reflashing is done before assembly or with the board lifted out).
    if NANO_USB_C_SERVICE_OPENING:
        nw, nh, nr = NANO_USB_C_CUTOUT_MM
        nx = sum(NANO_USB_C_X_MM) / 2
        nz = NANO_PCB_THICKNESS_MM + NANO_USB_C_HEIGHT_MM / 2
        cutters.append(
            prism_y(rounded_rect(nx - nw / 2, nz - nh / 2, nx + nw / 2, nz + nh / 2, nr),
                    L.y_min - over, L.board.y0 + over)
        )

    # Charger USB-C through the -Y wall, sized for any compliant plug overmold.
    # The charger sits high, so the envelope usually reaches the lid: then it
    # is a top-open notch closed by the lid, like the USB-A.
    cw, ch, cr = CHARGER_USB_C_CUTOUT_MM
    cz = L.charger_usb_c_z
    if L.charger_usb_c_notch:
        profile = shapely_box(L.charger_usb_c_x - cw / 2, cz - ch / 2, L.charger_usb_c_x + cw / 2, L.top_z + over)
    else:
        profile = rounded_rect(L.charger_usb_c_x - cw / 2, cz - ch / 2, L.charger_usb_c_x + cw / 2, cz + ch / 2, cr)
    cutters.append(prism_y(profile, L.y_min - over, L.board.y0 + over))

    # The rib sits below the lid so wires can cross anywhere; one deeper
    # notch on the JST line for the cell lead.
    rib_top = L.top_z - RIB_TOP_GAP_MM
    a, b = L.board, L.battery
    y0 = max(a.y0, b.y0) + BAY_CORNER_RADIUS_MM
    y1 = min(a.y1, b.y1) - BAY_CORNER_RADIUS_MM
    cutters.append(box(a.x1 - over, y0, rib_top, b.x0 + over, y1, L.top_z + over))
    notch_y = L.charger_jst_y
    cutters.append(
        box(a.x1 - over, notch_y - RIB_NOTCH_WIDTH_MM / 2, rib_top - RIB_NOTCH_DEPTH_MM,
            b.x0 + over, notch_y + RIB_NOTCH_WIDTH_MM / 2, L.top_z + over)
    )

    shell = difference(shell, cutters)
    validate(shell, "shell")
    return shell


# ===========================================================================
# Lid
# ===========================================================================
def build_lid(L: Layout) -> trimesh.Trimesh:
    over = 1.0
    lid = prism_z(
        rounded_rect(L.x_min, L.y_min, L.x_max, L.y_max, OUTER_CORNER_RADIUS_MM),
        L.top_z,
        L.lid_top_z,
    )

    # Pocket frame on the underside: locates the OLED PCB, glass against the roof.
    ox, oy = L.oled_center
    ax, ay = OLED_ACTIVE_OFFSET_MM
    px = OLED_PCB_SIZE_MM[0] + OLED_POCKET_CLEARANCE_MM
    py = OLED_PCB_SIZE_MM[1] + OLED_POCKET_CLEARANCE_MM
    # The lit area is centred on the window; the PCB is offset from it.
    pcx, pcy = ox - ax, oy - ay
    frame_z0 = L.top_z - OLED_MODULE_THICKNESS_MM
    frame = prism_z(
        rounded_rect(pcx - px / 2 - OLED_FRAME_MM, pcy - py / 2 - OLED_FRAME_MM,
                     pcx + px / 2 + OLED_FRAME_MM, pcy + py / 2 + OLED_FRAME_MM, OLED_FRAME_MM),
        frame_z0,
        L.top_z + 0.5,
    )

    # Four bosses carry the charger: they land on its PCB top at the mounting
    # holes and take M2.5 screws from below.
    bosses = [
        cylinder_z(x, y, L.charger_pcb_top_z, L.top_z + 0.5, CHARGER_BOSS_DIAMETER_MM / 2)
        for x, y in L.charger_holes
    ]
    lid = union([lid, frame, *bosses])

    cutters = [
        prism_z(shapely_box(pcx - px / 2, pcy - py / 2, pcx + px / 2, pcy + py / 2), frame_z0 - over, L.top_z),
    ]
    # Wire exits through both Y ends of the frame.
    for y0, y1 in ((pcy - py / 2 - OLED_FRAME_MM - over, pcy - py / 2 + over),
                   (pcy + py / 2 - over, pcy + py / 2 + OLED_FRAME_MM + over)):
        cutters.append(box(pcx - OLED_WIRE_EXIT_WIDTH_MM / 2, y0, frame_z0 - over,
                           pcx + OLED_WIRE_EXIT_WIDTH_MM / 2, y1, L.top_z))
    # Window over the lit area.
    ww = OLED_ACTIVE_SIZE_MM[0] + 2 * OLED_WINDOW_RELIEF_MM
    wh = OLED_ACTIVE_SIZE_MM[1] + 2 * OLED_WINDOW_RELIEF_MM
    cutters.append(
        prism_z(rounded_rect(ox - ww / 2, oy - wh / 2, ox + ww / 2, oy + wh / 2, OLED_WINDOW_RADIUS_MM),
                L.top_z - over, L.lid_top_z + over)
    )
    # Screw clearance holes over the Nano standoffs and the two wall screws.
    for x, y in (*L.nano_holes, *L.wall_screws):
        cutters.append(cylinder_z(x, y, L.top_z - over, L.lid_top_z + over, LID_SCREW_CLEARANCE_MM / 2))
    # Blind tapped holes up into the charger bosses.
    for x, y in L.charger_holes:
        cutters.append(cylinder_z(x, y, L.charger_pcb_top_z - over, L.charger_pcb_top_z + POST_HOLE_DEPTH_MM,
                                  POST_HOLE_DIAMETER_MM / 2))

    lid = difference(lid, cutters)

    # Locating posts through the OLED's own mounting holes. Added after the
    # pocket cutter, which would otherwise remove them. They stand clear of
    # the glass: the holes sit outside its X extent.
    lid = union([lid] + [
        cylinder_z(x, y, L.top_z - OLED_MODULE_THICKNESS_MM, L.top_z,
                   OLED_POST_DIAMETER_MM / 2)
        for x, y in oled_hole_positions(L)
    ])
    validate(lid, "lid")
    return lid


# ===========================================================================
# Side buttons
# ===========================================================================
def build_button(L: Layout) -> trimesh.Trimesh:
    """One plunger, cap down (+Z is inward when installed)."""
    guide = L.button_guide_y1 - L.y_min
    shaft_len = (SWITCH_ACTUATOR_Y_MM - L.y_min) - BUTTON_TIP_CLEARANCE_MM
    retainer_z0 = guide + BUTTON_RETAINER_WALL_CLEARANCE_MM
    if retainer_z0 + BUTTON_RETAINER_DEPTH_MM >= shaft_len:
        raise ValueError("no room for the button retainer between guide and switch")
    cap = prism_z(
        rounded_rect(-BUTTON_CAP_SIZE_MM[0] / 2, -BUTTON_CAP_SIZE_MM[1] / 2,
                     BUTTON_CAP_SIZE_MM[0] / 2, BUTTON_CAP_SIZE_MM[1] / 2, BUTTON_CAP_RADIUS_MM),
        0.0, BUTTON_CAP_DEPTH_MM,
    )
    shaft = cylinder_z(0.0, 0.0, BUTTON_CAP_DEPTH_MM * 0.5, BUTTON_CAP_DEPTH_MM + shaft_len, BUTTON_SHAFT_RADIUS_MM)
    retainer = cylinder_z(0.0, 0.0, BUTTON_CAP_DEPTH_MM + retainer_z0,
                          BUTTON_CAP_DEPTH_MM + retainer_z0 + BUTTON_RETAINER_DEPTH_MM, BUTTON_RETAINER_RADIUS_MM)
    button = union([cap, shaft, retainer])
    validate(button, "button")
    return button


# ===========================================================================
def summary(L: Layout, shell: trimesh.Trimesh, lid: trimesh.Trimesh) -> str:
    nose_y = L.charger.y0 - CHARGER_USB_C_OVERHANG_MM
    lines = [
        "Layout (case frame = Nano PCB frame, mm)",
        f"  outer footprint     {L.x_max - L.x_min:.2f} x {L.y_max - L.y_min:.2f}",
        f"  assembled height    {L.lid_top_z - L.bottom_z:.2f}  (shell {L.top_z - L.bottom_z:.2f} + lid {LID_MM:.2f})",
        f"  lid underside z     {L.top_z:.2f}  (USB-A top {USB_A_Z_MM[1]:.2f}, clearance {L.top_z - USB_A_Z_MM[1]:.2f})",
        f"  board bay           x {L.board.x0:.2f}..{L.board.x1:.2f}  y {L.board.y0:.2f}..{L.board.y1:.2f}",
        f"  battery bay         x {L.battery.x0:.2f}..{L.battery.x1:.2f}  y {L.battery.y0:.2f}..{L.battery.y1:.2f}",
        f"  charger PCB         x {L.charger.x0:.2f}..{L.charger.x1:.2f}  y {L.charger.y0:.2f}..{L.charger.y1:.2f}"
        f"  (JST overhang to x {L.charger.x0 - CHARGER_JST_OVERHANG_MM:.2f}), rotated +90 deg",
        f"  charger z           heads {L.charger_bottom_z:.2f}  PCB {L.charger_pcb_z:.2f}..{L.charger_pcb_top_z:.2f}"
        f"  parts to {L.charger_part_top_z:.2f}  (lid {L.top_z:.2f})",
        f"  charger to Nano     {L.charger_bottom_z - NANO_MIDBOARD_PART_HEIGHT_MM:.2f} under the screw heads,"
        f" {L.charger_pcb_z - CHARGER_UNDERSIDE_DEPTH_MM - NANO_MIDBOARD_PART_HEIGHT_MM:.2f} under the board",
        f"  charger corridor    {NANO_MID_PINS_MM[1][0] - L.charger.y1:.2f} to the pin row before the USB-A",
        "  charger holes       " + ", ".join(f"({x:.2f}, {y:.2f})" for x, y in L.charger_holes),
        f"  lid bosses          {L.top_z - L.charger_pcb_top_z:.2f} tall, Ø{CHARGER_BOSS_DIAMETER_MM}",
        f"  charger USB-C       x {L.charger_usb_c_x:.2f} z {L.charger_usb_c_z:.2f} on the -Y face,"
        f" nose {nose_y - L.y_min:.2f} behind the face, {'top-open notch' if L.charger_usb_c_notch else 'closed cutout'}",
        f"  charger JST         x {L.charger.x0:.2f} edge, y {L.charger_jst_y:.2f}; rib notch on that line",
        "  charger 5 V pads    " + ", ".join(f"({x:.2f}, {y:.2f})" for x, y in L.charger_output_pads),
        f"  OLED lit centre     ({L.oled_center[0]:.2f}, {L.oled_center[1]:.2f})",
        f"  -Y wall thickness   {L.board.y0 - L.y_min:.2f} (plunger guide {L.button_guide_y1 - L.y_min:.2f})",
        f"  lid screws          {len(L.nano_holes) + len(L.wall_screws)} x M2.5 from above,"
        f" {len(L.charger_holes)} x M2.5 into the bosses from below",
        f"  shell bounds        {shell.extents[0]:.3f} x {shell.extents[1]:.3f} x {shell.extents[2]:.3f}",
        f"  lid bounds          {lid.extents[0]:.3f} x {lid.extents[1]:.3f} x {lid.extents[2]:.3f}",
    ]
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out-dir", type=Path, default=Path(__file__).parent / "print-package")
    parser.add_argument("--prefix", default="revlink-sidecar-battery")
    args = parser.parse_args()

    L = compute_layout()
    shell = build_shell(L)
    lid = build_lid(L)
    button = build_button(L)

    out = args.out_dir
    out.mkdir(parents=True, exist_ok=True)
    names = {
        "shell": f"01-{args.prefix}-shell",
        "lid": f"02-{args.prefix}-lid-oled",
        "buttons": f"03-{args.prefix}-side-buttons-pair",
    }
    for mesh, key in ((shell, "shell"), (lid, "lid")):
        mesh.export(out / f"{names[key]}.stl")
        mesh.export(out / f"{names[key]}.3mf")

    reset_button = button.copy()
    boot_button = button.copy()
    reset_button.apply_translation((-4.0, 0.0, 0.0))
    boot_button.apply_translation((4.0, 0.0, 0.0))
    pair = union([reset_button, boot_button])
    validate(pair, "button pair", bodies=2)
    pair.export(out / f"{names['buttons']}.stl")
    trimesh.Scene({"RST button": reset_button, "BOOT button": boot_button}).export(out / f"{names['buttons']}.3mf")

    for key in names.values():
        print(f"wrote {out / key}.stl and .3mf")
    print(summary(L, shell, lid))


if __name__ == "__main__":
    main()
