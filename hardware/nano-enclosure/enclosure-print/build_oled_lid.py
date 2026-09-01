#!/usr/bin/env python3
"""Build the 31 mm ESP32-P4 Nano lid with a centered OLED window.

The supplied Nano lid already contains the mating edge, retaining clips, and
connector clearances.  This script preserves that lower geometry, extends the
upper shell to the requested height, keeps the original 2 mm roof thickness,
and cuts a rounded portrait window for the existing 1.3-inch SH1106 OLED.
"""

from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
import trimesh

TARGET_LID_HEIGHT_MM = 31.0
ROOF_THICKNESS_MM = 2.0

OLED_WINDOW_WIDTH_MM = 15.5
OLED_WINDOW_HEIGHT_MM = 30.2
OLED_WINDOW_RADIUS_MM = 1.0
OLED_CUT_MARGIN_MM = 1.25
OLED_CORNER_SEGMENTS = 32

# Connector coordinates come from the supplied Waveshare board and enclosure
# models. Both connectors face the lid's positive-Y wall.
USB_A_MIN_X_MM = 19.7
USB_A_MAX_X_MM = 27.5
USB_A_MAX_Z_MM = 15.8
USB_A_TOP_CLEARANCE_MM = 0.6

ETHERNET_MIN_X_MM = 27.5
ETHERNET_MAX_X_MM = 44.5
ETHERNET_MAX_Z_MM = 15.3
ETHERNET_TOP_CLEARANCE_MM = 0.7
ETHERNET_FACE_INNER_Y_MM = 51.7

WALL_INNER_Y_MM = 50.5
WALL_OUTER_Y_MM = 52.5

# RST and BOOT are side-actuated toward the USB-C edge. The supplied donor lid
# leaves one large slot for both switches; this revision closes that slot and
# replaces it with two guided button holes.
BUTTON_SLOT_MIN_X_MM = 3.0
BUTTON_SLOT_MAX_X_MM = 18.0
BUTTON_SLOT_MIN_Z_MM = 1.6
BUTTON_SLOT_MAX_Z_MM = 7.25
BUTTON_FACE_OUTER_Y_MM = -2.5
BUTTON_FACE_INNER_Y_MM = -1.3

RESET_CENTER_XZ_MM = (7.75, 3.25)
BOOT_CENTER_XZ_MM = (13.25, 3.25)
BUTTON_SWITCH_FACE_Y_MM = -0.5
BUTTON_TIP_CLEARANCE_MM = 0.05

BUTTON_HOLE_RADIUS_MM = 1.0
BUTTON_SHAFT_RADIUS_MM = 0.78
BUTTON_CAP_WIDTH_MM = 3.8
BUTTON_CAP_HEIGHT_MM = 3.0
BUTTON_CAP_RADIUS_MM = 0.5
BUTTON_CAP_DEPTH_MM = 0.9
BUTTON_RETAINER_RADIUS_MM = 1.12
BUTTON_RETAINER_DEPTH_MM = 0.4
BUTTON_RETAINER_WALL_CLEARANCE_MM = 0.1


def rounded_rectangle_prism(
    *,
    width: float,
    height: float,
    radius: float,
    depth: float,
    center: tuple[float, float, float],
) -> trimesh.Trimesh:
    """Return a watertight rounded-rectangle cutter."""

    if radius <= 0 or radius * 2 >= min(width, height):
        raise ValueError("radius must be positive and smaller than half the window")

    center_x, center_y, center_z = center
    pieces: list[trimesh.Trimesh] = []

    for extents in (
        (width - (2 * radius), height, depth),
        (width, height - (2 * radius), depth),
    ):
        box = trimesh.creation.box(extents=extents)
        box.apply_translation((center_x, center_y, center_z))
        pieces.append(box)

    corner_x = (width / 2) - radius
    corner_y = (height / 2) - radius
    for x_sign in (-1, 1):
        for y_sign in (-1, 1):
            corner = trimesh.creation.cylinder(
                radius=radius,
                height=depth,
                sections=OLED_CORNER_SEGMENTS,
            )
            corner.apply_translation(
                (
                    center_x + (x_sign * corner_x),
                    center_y + (y_sign * corner_y),
                    center_z,
                )
            )
            pieces.append(corner)

    cutter = trimesh.boolean.union(pieces, engine="manifold")
    if cutter is None or not cutter.is_watertight:
        raise RuntimeError("failed to build a watertight OLED cutter")
    return cutter


def box_between(
    *,
    minimum: tuple[float, float, float],
    maximum: tuple[float, float, float],
) -> trimesh.Trimesh:
    minimum_vector = np.asarray(minimum, dtype=float)
    maximum_vector = np.asarray(maximum, dtype=float)
    box = trimesh.creation.box(extents=maximum_vector - minimum_vector)
    box.apply_translation((minimum_vector + maximum_vector) / 2)
    return box


def vertical_cylinder(
    *,
    radius: float,
    minimum_z: float,
    maximum_z: float,
    center_xy: tuple[float, float],
    sections: int = 48,
) -> trimesh.Trimesh:
    cylinder = trimesh.creation.cylinder(
        radius=radius,
        height=maximum_z - minimum_z,
        sections=sections,
    )
    cylinder.apply_translation(
        (
            center_xy[0],
            center_xy[1],
            (minimum_z + maximum_z) / 2,
        )
    )
    return cylinder


def y_axis_cylinder(
    *,
    radius: float,
    minimum_y: float,
    maximum_y: float,
    center_xz: tuple[float, float],
    sections: int = 48,
) -> trimesh.Trimesh:
    cylinder = trimesh.creation.cylinder(
        radius=radius,
        height=maximum_y - minimum_y,
        sections=sections,
    )
    cylinder.apply_transform(
        trimesh.transformations.rotation_matrix(
            np.pi / 2,
            (1.0, 0.0, 0.0),
        )
    )
    cylinder.apply_translation(
        (
            center_xz[0],
            (minimum_y + maximum_y) / 2,
            center_xz[1],
        )
    )
    return cylinder


def build_button_plunger() -> trimesh.Trimesh:
    """Build one short USB-C-edge button, already oriented cap-down."""

    shaft_length = (
        BUTTON_SWITCH_FACE_Y_MM
        - BUTTON_TIP_CLEARANCE_MM
        - (BUTTON_FACE_OUTER_Y_MM - BUTTON_CAP_DEPTH_MM)
    )
    shaft = vertical_cylinder(
        radius=BUTTON_SHAFT_RADIUS_MM,
        minimum_z=BUTTON_CAP_DEPTH_MM * 0.65,
        maximum_z=shaft_length,
        center_xy=(0.0, 0.0),
    )
    retainer_minimum_z = (
        BUTTON_FACE_INNER_Y_MM
        + BUTTON_RETAINER_WALL_CLEARANCE_MM
        - (BUTTON_FACE_OUTER_Y_MM - BUTTON_CAP_DEPTH_MM)
    )
    retainer = vertical_cylinder(
        radius=BUTTON_RETAINER_RADIUS_MM,
        minimum_z=retainer_minimum_z,
        maximum_z=retainer_minimum_z + BUTTON_RETAINER_DEPTH_MM,
        center_xy=(0.0, 0.0),
    )
    cap = rounded_rectangle_prism(
        width=BUTTON_CAP_WIDTH_MM,
        height=BUTTON_CAP_HEIGHT_MM,
        radius=BUTTON_CAP_RADIUS_MM,
        depth=BUTTON_CAP_DEPTH_MM,
        center=(0.0, 0.0, BUTTON_CAP_DEPTH_MM / 2),
    )

    plunger = trimesh.boolean.union(
        [shaft, retainer, cap],
        engine="manifold",
    )
    if plunger is None or not plunger.is_watertight:
        raise RuntimeError("failed to create the printable button plunger")
    return plunger


def build_lid(source: Path) -> trimesh.Trimesh:
    lid = trimesh.load_mesh(source, process=True)
    if not isinstance(lid, trimesh.Trimesh):
        raise TypeError(f"{source}: expected one mesh")

    # The donor STL contains one small triangular opening. Repairing that face
    # leaves the supplied shell as one closed volume before the boolean cut.
    trimesh.repair.fill_holes(lid)
    if not lid.is_watertight:
        raise ValueError(f"{source}: donor lid could not be repaired")

    minimum_z, maximum_z = lid.bounds[:, 2]
    original_height = maximum_z - minimum_z
    extension = TARGET_LID_HEIGHT_MM - original_height
    if extension < 0:
        raise ValueError(
            f"{source}: donor lid is already taller than {TARGET_LID_HEIGHT_MM} mm"
        )

    # Move the complete roof upward so its thickness is not stretched. Faces
    # connecting the roof to the lower shell become the extended side walls;
    # all mating and retaining geometry below the roof remains unchanged.
    roof_inner_z = maximum_z - ROOF_THICKNESS_MM
    roof_vertices = lid.vertices[:, 2] >= (roof_inner_z - 1e-5)
    lid.vertices[roof_vertices, 2] += extension

    # The donor called "top-solid" still has six openings and a stepped
    # exterior outline in its roof. Fill the complete original roof envelope
    # with a new 2 mm slab before cutting the one intentional OLED opening.
    revised_maximum_z = lid.bounds[1, 2]
    face_centers = lid.triangles_center
    top_faces = (lid.face_normals[:, 2] > 0.9) & (
        face_centers[:, 2] > (revised_maximum_z - 0.1)
    )
    top_vertices = np.vstack([lid.vertices[face] for face in lid.faces[top_faces]])
    roof_minimum_xy = np.min(top_vertices[:, :2], axis=0)
    roof_maximum_xy = np.max(top_vertices[:, :2], axis=0)
    roof_size_xy = roof_maximum_xy - roof_minimum_xy
    roof_center_xy = (roof_minimum_xy + roof_maximum_xy) / 2

    smooth_roof = trimesh.creation.box(
        extents=(
            roof_size_xy[0],
            roof_size_xy[1],
            ROOF_THICKNESS_MM,
        )
    )
    smooth_roof.apply_translation(
        (
            roof_center_xy[0],
            roof_center_xy[1],
            revised_maximum_z - (ROOF_THICKNESS_MM / 2),
        )
    )
    lid = trimesh.boolean.union([lid, smooth_roof], engine="manifold")
    if lid is None or not lid.is_watertight:
        raise RuntimeError("failed to create the smooth solid roof")

    # Retain a correctly sized USB-A opening but fill the unused air gap that
    # appeared when the upper shell was extended.
    usb_upper_wall = box_between(
        minimum=(
            USB_A_MIN_X_MM,
            WALL_INNER_Y_MM,
            USB_A_MAX_Z_MM + USB_A_TOP_CLEARANCE_MM,
        ),
        maximum=(
            USB_A_MAX_X_MM,
            WALL_OUTER_Y_MM,
            revised_maximum_z - ROOF_THICKNESS_MM,
        ),
    )

    # Hide the Ethernet jack without requiring desoldering. The lower section
    # is a thin 0.8 mm cosmetic face with 0.7 mm clearance from the supplied
    # RJ45 model. Above the jack the wall returns to the full 2 mm thickness.
    ethernet_face = box_between(
        minimum=(
            ETHERNET_MIN_X_MM,
            ETHERNET_FACE_INNER_Y_MM,
            lid.bounds[0, 2],
        ),
        maximum=(
            ETHERNET_MAX_X_MM,
            WALL_OUTER_Y_MM,
            ETHERNET_MAX_Z_MM + ETHERNET_TOP_CLEARANCE_MM,
        ),
    )
    ethernet_upper_wall = box_between(
        minimum=(
            ETHERNET_MIN_X_MM,
            WALL_INNER_Y_MM,
            ETHERNET_MAX_Z_MM + ETHERNET_TOP_CLEARANCE_MM,
        ),
        maximum=(
            ETHERNET_MAX_X_MM,
            WALL_OUTER_Y_MM,
            revised_maximum_z - ROOF_THICKNESS_MM,
        ),
    )

    # Close the donor's large shared switch slot with a thin USB-C-edge face.
    # The face stops 0.8 mm before the switch actuators so the printed buttons
    # can move freely.
    button_face = box_between(
        minimum=(
            BUTTON_SLOT_MIN_X_MM,
            BUTTON_FACE_OUTER_Y_MM,
            BUTTON_SLOT_MIN_Z_MM,
        ),
        maximum=(
            BUTTON_SLOT_MAX_X_MM,
            BUTTON_FACE_INNER_Y_MM,
            BUTTON_SLOT_MAX_Z_MM,
        ),
    )

    lid = trimesh.boolean.union(
        [
            lid,
            usb_upper_wall,
            ethernet_face,
            ethernet_upper_wall,
            button_face,
        ],
        engine="manifold",
    )
    if lid is None or not lid.is_watertight:
        raise RuntimeError("failed to add the connector and button walls")

    footprint_center = tuple(np.mean(lid.bounds[:, :2], axis=0))
    revised_maximum_z = lid.bounds[1, 2]
    cut_depth = ROOF_THICKNESS_MM + (2 * OLED_CUT_MARGIN_MM)
    cutter = rounded_rectangle_prism(
        width=OLED_WINDOW_WIDTH_MM,
        height=OLED_WINDOW_HEIGHT_MM,
        radius=OLED_WINDOW_RADIUS_MM,
        depth=cut_depth,
        center=(
            footprint_center[0],
            footprint_center[1],
            revised_maximum_z - (ROOF_THICKNESS_MM / 2),
        ),
    )

    button_cutters = [
        y_axis_cylinder(
            radius=BUTTON_HOLE_RADIUS_MM,
            minimum_y=BUTTON_FACE_OUTER_Y_MM - 0.5,
            maximum_y=BUTTON_FACE_INNER_Y_MM + 0.5,
            center_xz=center_xz,
        )
        for center_xz in (RESET_CENTER_XZ_MM, BOOT_CENTER_XZ_MM)
    ]
    all_cutters = trimesh.boolean.union(
        [cutter, *button_cutters],
        engine="manifold",
    )
    revised = trimesh.boolean.difference([lid, all_cutters], engine="manifold")
    if revised is None or not isinstance(revised, trimesh.Trimesh):
        raise RuntimeError("OLED window boolean returned no mesh")
    if not revised.is_watertight or not revised.is_winding_consistent:
        raise RuntimeError("revised lid is not a closed, consistently wound solid")

    actual_height = float(revised.extents[2])
    if abs(actual_height - TARGET_LID_HEIGHT_MM) > 0.002:
        raise RuntimeError(
            f"expected {TARGET_LID_HEIGHT_MM} mm lid, got {actual_height:.4f} mm"
        )
    return revised


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--stl-output", required=True, type=Path)
    parser.add_argument("--3mf-output", dest="three_mf_output", type=Path)
    parser.add_argument("--button-output", type=Path)
    parser.add_argument(
        "--button-3mf-output",
        dest="button_three_mf_output",
        type=Path,
    )
    args = parser.parse_args()

    revised = build_lid(args.source)

    args.stl_output.parent.mkdir(parents=True, exist_ok=True)
    revised.export(args.stl_output)
    if args.three_mf_output is not None:
        args.three_mf_output.parent.mkdir(parents=True, exist_ok=True)
        revised.export(args.three_mf_output)

    if args.button_output is not None or args.button_three_mf_output is not None:
        button = build_button_plunger()
        if args.button_output is not None:
            args.button_output.parent.mkdir(parents=True, exist_ok=True)
            button.export(args.button_output)
        if args.button_three_mf_output is not None:
            args.button_three_mf_output.parent.mkdir(parents=True, exist_ok=True)
            reset_button = button.copy()
            boot_button = button.copy()
            reset_button.apply_translation((-4.0, 0.0, 0.0))
            boot_button.apply_translation((4.0, 0.0, 0.0))
            button_pair = trimesh.Scene(
                {
                    "RST button": reset_button,
                    "BOOT button": boot_button,
                }
            )
            button_pair.export(args.button_three_mf_output)

    print(f"STL: {args.stl_output}")
    if args.three_mf_output is not None:
        print(f"3MF: {args.three_mf_output}")
    if args.button_output is not None:
        print(f"Button STL (print two): {args.button_output}")
    if args.button_three_mf_output is not None:
        print(f"Button-pair 3MF: {args.button_three_mf_output}")
    print(
        "Bounds (mm): "
        f"{revised.extents[0]:.3f} × "
        f"{revised.extents[1]:.3f} × "
        f"{revised.extents[2]:.3f}"
    )
    print(f"Watertight: {revised.is_watertight}")


if __name__ == "__main__":
    main()
