#!/usr/bin/env python3
"""Extract authored Reach vehicle-camera base points from the Blender kit."""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

import bpy


EVIDENCE_TEXT = "REACH_SOURCE_EVIDENCE.json"
CAMERA_PREFIX = "cam:"


def arguments():
    parser = argparse.ArgumentParser(
        description="Export camera base points from reach_vehicle_cameras_v1.blend")
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--allow-unplaced", action="store_true",
        help="export a draft even when cameras are still marked NEEDS PLACEMENT")
    parser.add_argument("--precision", type=int, default=9)
    values = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    return parser.parse_args(values)


def enable_layer_collection(layer):
    layer.exclude = False
    layer.hide_viewport = False
    for child in layer.children:
        enable_layer_collection(child)


def finite_matrix(matrix):
    return all(math.isfinite(matrix[row][column])
               for row in range(4) for column in range(4))


def rounded(values, precision):
    return [round(float(value), precision) for value in values]


def main():
    args = arguments()
    if not bpy.data.is_saved:
        raise SystemExit("ERROR: save the Reach camera kit before exporting")
    text = bpy.data.texts.get(EVIDENCE_TEXT)
    if text is None:
        raise SystemExit("ERROR: blend lacks embedded %s" % EVIDENCE_TEXT)
    evidence = json.loads(text.as_string())
    scale = float(evidence["world_units_to_metres"])
    if scale != 3.048:
        raise SystemExit("ERROR: embedded Reach scale is not 3.048")

    enable_layer_collection(bpy.context.view_layer.layer_collection)
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()

    expected = []
    identity_order = []
    for identity in evidence["identities"]:
        identity_order.append(identity["id"])
        expected.extend(seat["camera_name"] for seat in identity["seats"])
    actual = sorted(obj.name for obj in bpy.data.objects
                    if obj.type == 'CAMERA' and obj.name.startswith(CAMERA_PREFIX))
    problems = []
    if len(expected) != 25 or len(set(expected)) != 25:
        problems.append("embedded evidence does not describe 25 unique cameras")
    if set(actual) != set(expected):
        problems.append("camera set differs: expected=%s actual=%s" %
                        (sorted(expected), actual))

    seed_points = {}
    authored_points = {}
    deltas = {}
    config_trims = {}
    config_values = {}
    cameras = {}
    unplaced = []
    for identity in evidence["identities"]:
        identity_id = identity["id"]
        seed_points[identity_id] = {}
        authored_points[identity_id] = {}
        deltas[identity_id] = {}
        config_trims[identity_id] = {}
        for seat in identity["seats"]:
            name = seat["camera_name"]
            obj = bpy.data.objects.get(name)
            if obj is None or obj.type != 'CAMERA':
                continue
            matrix = obj.matrix_world.copy()
            evaluated = obj.evaluated_get(depsgraph).matrix_world.copy()
            if not finite_matrix(matrix):
                problems.append("%s has a non-finite world matrix" % name)
                continue
            delta = max(abs(matrix[row][column] - evaluated[row][column])
                        for row in range(4) for column in range(4))
            if delta > 1e-6:
                problems.append("%s evaluated transform differs by %.9g" %
                                (name, delta))
            position_m = matrix.translation
            position_wu = position_m / scale
            seed_wu = [float(value) for value in seat["seed_base_world_units"]]
            delta_wu = [position_wu[index] - seed_wu[index]
                        for index in range(3)]
            trim_m = {
                "forward": delta_wu[0] * scale,
                "right": -delta_wu[1] * scale,
                "up": delta_wu[2] * scale,
            }
            seat_name = "seat%d" % int(seat["index"])
            expected_collection = "%02d %s" % (identity["order"], identity_id)
            collections = [collection.name for collection in obj.users_collection]
            if collections != [expected_collection]:
                problems.append("%s collection is %s, expected %s" %
                                (name, collections, expected_collection))
            if obj.get("reach_identity") != identity_id:
                problems.append("%s identity metadata changed" % name)
            if obj.get("reach_frame") != identity["frame"]:
                problems.append("%s frame metadata changed" % name)
            for axis in ("forward", "up", "right"):
                expected_key = "vehicle_cam_%s_m_reach_%s_%s" % (
                    axis, identity_id, seat_name)
                if obj.get("reach_config_%s_key" % axis) != expected_key:
                    problems.append("%s config key metadata changed" % name)
            needs_placement = bool(obj.get("reach_needs_user_placement", True))
            if needs_placement:
                unplaced.append(name)
            seed_points[identity_id][seat_name] = rounded(seed_wu, args.precision)
            authored_points[identity_id][seat_name] = rounded(
                position_wu, args.precision)
            deltas[identity_id][seat_name] = rounded(delta_wu, args.precision)
            config_trims[identity_id][seat_name] = {
                axis: round(value, args.precision)
                for axis, value in trim_m.items()}
            for axis, value in trim_m.items():
                key = "vehicle_cam_%s_m_reach_%s_%s" % (
                    axis, identity_id, seat_name)
                config_values[key] = round(value, args.precision)
            cameras[name] = {
                "identity": identity_id,
                "seat": seat_name,
                "frame": identity["frame"],
                "placed": not needs_placement,
                "seed_base_world_units": rounded(seed_wu, args.precision),
                "authored_base_metres": rounded(position_m, args.precision),
                "authored_base_world_units": rounded(position_wu, args.precision),
                "delta_from_seed_world_units": rounded(delta_wu, args.precision),
                "runtime_config_trim_metres": {
                    axis: round(value, args.precision)
                    for axis, value in trim_m.items()},
                "matrix_world": [rounded(matrix[row], args.precision)
                                 for row in range(4)],
            }

    if unplaced and not args.allow_unplaced:
        problems.append("%d cameras still need placement: %s" %
                        (len(unplaced), ", ".join(unplaced)))
    if problems:
        for problem in problems:
            print("ERROR: " + problem, file=sys.stderr)
        raise SystemExit(2)

    output = args.output
    if output is None:
        output = Path(bpy.data.filepath).with_name("reach_vehicle_camera_points.json")
    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "schema": 2,
        "title": "Halo: Reach first-person vehicle camera deltas",
        "source_blend": str(Path(bpy.data.filepath).resolve()),
        "hrek_build": evidence["hrek_build"],
        "world_units_to_metres": scale,
        "axes": {"x": "forward", "y": "left", "z": "up"},
        "runtime_trim_equation": (
            "authored = seed + {forward, -right, up}; trim values are metres"),
        "identity_order": identity_order,
        "camera_count": len(cameras),
        "all_cameras_marked_placed": not unplaced,
        "seed_points_world_units": seed_points,
        "authored_points_world_units_audit_only": authored_points,
        "deltas_from_seed_world_units": deltas,
        "runtime_config_trims_metres": config_trims,
        "halomccvr_cfg_values": config_values,
        "cameras": cameras,
    }
    output.write_text(
        json.dumps(payload, indent=2) + "\n", encoding="utf-8", newline="\n")
    print("REACH_POINTS_READY")
    print("cameras=%d placed=%d" % (len(cameras), len(cameras) - len(unplaced)))
    print("output=%s" % output)


if __name__ == "__main__":
    main()
