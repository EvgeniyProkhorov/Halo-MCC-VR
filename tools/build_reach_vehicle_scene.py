#!/usr/bin/env python3
"""Build the official-HREK Reach vehicle camera authoring scene in Blender."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import shutil
import sys
import xml.etree.ElementTree as ET

import bpy
from mathutils import Matrix, Quaternion, Vector

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))
import directx_x  # noqa: E402

CAMERA_BASIS = Matrix((
    (0.0, 0.0, -1.0, 0.0),
    (-1.0, 0.0, 0.0, 0.0),
    (0.0, 1.0, 0.0, 0.0),
    (0.0, 0.0, 0.0, 1.0),
))


def digest(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest().upper()


def floats(value, count):
    result = tuple(float(part) for part in value.split(","))
    if len(result) != count or not all(math.isfinite(v) for v in result):
        raise ValueError("bad vector %r" % value)
    return result


def direct_field_values(element, name):
    return [
        child.get("value", "") for child in list(element)
        if child.tag == "field" and child.get("name") == name
    ]


def field_value(element, name, occurrence=0):
    values = direct_field_values(element, name)
    if occurrence >= len(values):
        raise ValueError("%s missing field %s" % (
            element.get("name", element.tag), name))
    return values[occurrence]


def block_elements(parent, name):
    children = list(parent)
    for index, child in enumerate(children):
        if child.tag != "field" or child.get("name") != name:
            continue
        count = int(child.get("value", "0"))
        result = []
        for sibling in children[index + 1:]:
            if sibling.tag == "element":
                result.append(sibling)
                if len(result) == count:
                    return result
        if count == 0:
            return []
        raise ValueError("short block %s: %d/%d" % (
            name, len(result), count))
    raise ValueError("missing block %s" % name)


def parse_vehicle(path, allowed):
    root = ET.parse(path).getroot()
    seat_elements = block_elements(root, "seats")
    result = []
    for raw_index in allowed:
        matches = [
            item for item in seat_elements
            if int(item.get("index", "-1")) == raw_index]
        if len(matches) != 1:
            raise ValueError("%s seat%d missing/duplicate" % (
                path, raw_index))
        seat = matches[0]
        seat_flags = field_value(seat, "flags").lower()
        for forbidden in (
                "boarding seat", "invalid for player", "invalid for hero"):
            if forbidden in seat_flags:
                raise ValueError("%s seat%d has %s" % (
                    path, raw_index, forbidden))
        children = list(seat)
        unit_index = next(
            index for index, child in enumerate(children)
            if child.tag == "field" and
            child.get("name") == "unit camera")
        unit_flags = ""
        camera_marker = ""
        for child in children[unit_index + 1:]:
            if child.tag != "field":
                continue
            if child.get("name") == "flags" and not unit_flags:
                unit_flags = child.get("value", "")
            if child.get("name") == "camera marker name":
                camera_marker = child.get("value", "")
                break
        result.append({
            "index": raw_index,
            "name": seat.get("name", "seat%d" % raw_index),
            "label": field_value(seat, "label"),
            "seat_flags": field_value(seat, "flags"),
            "seat_marker": field_value(seat, "marker name"),
            "camera_flags": unit_flags,
            "camera_marker": camera_marker,
        })
    return result


def parse_node(element):
    return {
        "index": int(element.get("index", "-1")),
        "name": field_value(element, "name"),
        "parent": field_value(element, "parent node"),
        "position": floats(
            field_value(element, "default translation"), 3),
        "rotation": floats(
            field_value(element, "default rotation"), 4),
    }


def parse_marker_group(element):
    records = []
    for marker in block_elements(element, "markers"):
        records.append({
            "permutation": int(field_value(
                marker, "permutation index")),
            "node_index": int(field_value(marker, "node index")),
            "position": floats(field_value(marker, "translation"), 3),
            "rotation": floats(field_value(marker, "rotation"), 4),
        })
    return field_value(element, "name").lower(), records


def parse_render_model(path):
    nodes = []
    groups = {}
    expected_nodes = None
    expected_groups = None
    mode = None
    depth = -1
    for event, element in ET.iterparse(path, events=("start", "end")):
        if event == "start":
            depth += 1
            continue
        if depth == 1 and element.tag == "field":
            if element.get("name") == "nodes":
                expected_nodes = int(element.get("value", "0"))
                mode = "nodes"
            elif element.get("name") == "marker groups":
                expected_groups = int(element.get("value", "0"))
                mode = "markers"
        elif depth == 1 and element.tag == "element":
            if mode == "nodes":
                nodes.append(parse_node(element))
                if len(nodes) == expected_nodes:
                    mode = None
            elif mode == "markers":
                name, records = parse_marker_group(element)
                groups.setdefault(name, []).extend(records)
                if len(groups) == expected_groups:
                    element.clear()
                    break
            element.clear()
        elif depth == 1:
            element.clear()
        depth -= 1
    if expected_nodes != len(nodes) or expected_groups != len(groups):
        raise ValueError("%s incomplete nodes/markers" % path)
    nodes.sort(key=lambda node: node["index"])
    if [node["index"] for node in nodes] != list(range(len(nodes))):
        raise ValueError("%s non-contiguous nodes" % path)
    by_name = {node["name"]: node for node in nodes}
    if len(by_name) != len(nodes):
        raise ValueError("%s duplicate node" % path)
    cache = {}

    def world_node(name):
        if name in cache:
            return cache[name]
        node = by_name[name]
        x, y, z, w = node["rotation"]
        local_q = Quaternion((w, x, y, z)).normalized()
        local_p = Vector(node["position"])
        if node["parent"] == "NONE":
            result = local_p, local_q
        else:
            parent_p, parent_q = world_node(node["parent"])
            result = (
                parent_p + parent_q @ local_p,
                (parent_q @ local_q).normalized())
        cache[name] = result
        return result

    resolved = {}
    for group_name, records in groups.items():
        resolved[group_name] = []
        for record in records:
            index = record["node_index"]
            if index < 0:
                node_name = "NONE"
                node_p, node_q = Vector(), Quaternion()
            else:
                if index >= len(nodes):
                    raise ValueError("%s marker node out of range" % path)
                node_name = nodes[index]["name"]
                node_p, node_q = world_node(node_name)
            x, y, z, w = record["rotation"]
            local_q = Quaternion((w, x, y, z)).normalized()
            local_p = Vector(record["position"])
            world_p = node_p + node_q @ local_p
            world_q = (node_q @ local_q).normalized()
            resolved[group_name].append({
                "permutation": record["permutation"],
                "node": node_name,
                "position": tuple(world_p),
                "rotation": tuple(world_q),
            })
    return {
        "node_count": len(nodes),
        "marker_group_count": len(groups),
        "markers": resolved,
    }


def choose_marker(render, name):
    records = list(render["markers"].get(name.lower(), []))
    records.sort(key=lambda item: (
        0 if item["permutation"] == -1 else
        1 if item["permutation"] == 0 else 2,
        item["permutation"]))
    return (records[0] if records else None), len(records)


def tag_matrix(marker, scale):
    return (
        Matrix.Translation(Vector(marker["position"]) * scale) @
        Quaternion(marker["rotation"]).to_matrix().to_4x4())


def lock_reference(obj):
    obj.lock_location = (True, True, True)
    obj.lock_rotation = (True, True, True)
    obj.lock_scale = (True, True, True)
    obj.hide_select = True
    obj["reach_locked_reference"] = True


def empty_ref(collection, name, matrix, display="ARROWS", size=0.18):
    obj = bpy.data.objects.new(name, None)
    obj.matrix_world = matrix
    obj.empty_display_type = display
    obj.empty_display_size = size
    obj.show_name = True
    obj.show_in_front = True
    collection.objects.link(obj)
    lock_reference(obj)
    return obj


def human_ref(collection):
    mesh = bpy.data.meshes.new("ref:2.08m:%s" % collection.name)
    x, y, z = 0.30, 0.225, 2.08
    vertices = [
        (-x, -y, 0), (x, -y, 0), (x, y, 0), (-x, y, 0),
        (-x, -y, z), (x, -y, z), (x, y, z), (-x, y, z)]
    faces = [
        (0, 1, 2, 3), (4, 7, 6, 5), (0, 4, 5, 1),
        (1, 5, 6, 2), (2, 6, 7, 3), (4, 0, 3, 7)]
    mesh.from_pydata(vertices, [], faces)
    obj = bpy.data.objects.new(
        "ref:2.08m_human:%s" % collection.name, mesh)
    obj.display_type = "WIRE"
    obj.color = (0.2, 0.9, 0.3, 1.0)
    obj.show_in_front = True
    collection.objects.link(obj)
    lock_reference(obj)


def load_mesh(asset_id, source, scale):
    path = source / (asset_id + ".mesh.x")
    meshes = directx_x.extract_meshes(directx_x.read_objects(path))
    if len(meshes) != 1:
        raise ValueError("%s expected one mesh" % path)
    mesh = meshes[0]
    if any(abs(a - b) > 1e-6 for a, b in zip(
            mesh.transform, directx_x.IDENTITY_MATRIX)):
        raise ValueError("%s has unproven frame transform" % path)
    vertices = [
        (v[0] * scale, v[2] * scale, v[1] * scale)
        for v in mesh.vertices]
    faces = [tuple(reversed(face)) for face in mesh.faces]
    data = bpy.data.meshes.new("HREK:%s" % asset_id)
    data.from_pydata(vertices, [], faces)
    data.validate(verbose=False)
    data.update()
    data["reach_hrek_mesh_sha256"] = digest(path)
    data["reach_axis_mapping"] = "DirectX(x,y,z)->Reach(x,z,y)"
    return data


def write_readme(path, evidence):
    lineup = "\n".join(
        "  %-18s %s" % (
            item["id"],
            ", ".join("seat%d" % seat["index"]
                      for seat in item["seats"]))
        for item in evidence["identities"])
    text = """HALO: REACH FIRST-PERSON VEHICLE CAMERA KIT v1
================================================

Open reach_vehicle_cameras_v1.blend. It has exactly 20 identity collections
and only the official-HREK-proven player seats listed below.

Every cam:<vehicle>:seatN object is an HREK seat/camera MARKER SEED, not a
claimed eye point. Translate only the cam: object until the eye is correct,
then save. Rotation/scale and all ref:/NEEDS_PLACEMENT: objects are locked.

Axes: Blender +X forward, +Y left, +Z up. Blender is in metres. Reach world
units = metres / 3.048. The Reach-only export check proved DirectX (x,y,z)
maps to Reach/Blender (x,z,y); winding is reversed for that reflection.

THE INITIAL HREK MARKER CAMERA IS THE COMPILED RUNTIME SEED. Your translated
camera is the desired effective eye. Runtime applies:
  effective = seed + { forward, -right, up }
using:
  vehicle_cam_forward_m_reach_<vehicle>_seatN
  vehicle_cam_up_m_reach_<vehicle>_seatN
  vehicle_cam_right_m_reach_<vehicle>_seatN
Unset Reach keys follow universal vehicle trims, so use zero effective trims
when comparing the game directly with Blender.

Mounted child identities (Wraith gunner, Warthog guns, Shade cannons) remain
in the immediate child unit frame. Their ultimate carrier is the locked wire
mesh inverse-transformed around that child. Do not flatten them to carrier
coordinates.

For 1:1 VR authoring, run embedded reach_vr_seats.py in Blender's Text Editor,
press N in the 3D View, and open Reach Seats. Edit Forward, Left, and Up
directly in that panel; Rebuild and Prev/Next never move cameras. Mark each
adjusted camera placed with the panel, then save the .blend.

To export audited deltas from those seed points, run Blender in this directory:
  blender.exe --background --factory-startup reach_vehicle_cameras_v1.blend
    --python parse_reach_vehicle_points.py --
    --output reach_vehicle_camera_points.json
The extractor refuses an incomplete set by default. Add --allow-unplaced only
for a clearly labelled draft. Runtime values are authored-minus-seed DELTAS,
not absolute positions; absolute points remain audit-only. It preserves
immediate-child coordinates, converts +left to runtime -right, and emits a
flat halomccvr_cfg_values map using the exact per-seat configuration keys.

Proven lineup:
%s

Meshes, nodes, markers, seats, attachment frames, and scale are from official
HREK 2023.07.17.176677.1-QFE1. Exact source hashes and node counts are embedded
as REACH_SOURCE_EVIDENCE.json and copied to reach_vehicle_scene.json. Geometry
is default-pose reference geometry. Wraith seat0 has no exported driver_camera
marker, so its HREK seat marker is an explicitly labelled fallback seed. Every
marker seed still needs user visual placement and headset acceptance.
""" % lineup
    path.write_text(text, encoding="utf-8", newline="\n")


def arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument(
        "--manifest", type=Path,
        default=SCRIPT_DIR / "reach_vehicle_kit_manifest.json")
    parser.add_argument(
        "--helper", type=Path,
        default=SCRIPT_DIR / "reach_vr_seats.py")
    parser.add_argument(
        "--extractor", type=Path,
        default=SCRIPT_DIR / "parse_reach_vehicle_points.py")
    values = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    return parser.parse_args(values)


def main():
    args = arguments()
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    identities = manifest["identities"]
    if len(identities) != 20:
        raise ValueError("manifest must have exactly 20 identities")
    if len({item["id"] for item in identities}) != 20:
        raise ValueError("duplicate identity")
    scale = float(manifest["world_units_to_metres"])
    if scale != 3.048:
        raise ValueError("Reach scale changed")
    source = args.source.resolve()
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    assets = identities + manifest.get("reference_assets", [])

    render_data = {}
    source_evidence = {}
    for asset in assets:
        asset_id = asset["id"]
        paths = {
            kind: source / ("%s.%s" % (asset_id, suffix))
            for kind, suffix in (
                ("vehicle", "vehicle.xml"),
                ("model", "model.xml"),
                ("render_model", "render_model.xml"),
                ("mesh", "mesh.x"))}
        render_data[asset_id] = parse_render_model(
            paths["render_model"])
        source_evidence[asset_id] = {
            "tag": asset["tag"],
            "node_count": render_data[asset_id]["node_count"],
            **{kind + "_sha256": digest(path)
               for kind, path in paths.items()},
        }
    seat_data = {
        item["id"]: parse_vehicle(
            source / (item["id"] + ".vehicle.xml"), item["seats"])
        for item in identities}

    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.unit_settings.system = "METRIC"
    scene.unit_settings.length_unit = "METERS"
    scene.unit_settings.scale_length = 1.0
    if scene.world is None:
        scene.world = bpy.data.worlds.new("Reach Vehicle Camera Kit World")
    scene.world.color = (0.008, 0.012, 0.02)
    scene["reach_kit_version"] = 1
    scene["reach_hrek_build"] = manifest["hrek_build"]
    scene["reach_identity_count"] = 20
    scene["reach_camera_status"] = (
        "HREK MARKER SEEDS - USER PLACEMENT REQUIRED")
    meshes = {
        asset["id"]: load_mesh(asset["id"], source, scale)
        for asset in assets}
    for asset in assets:
        mesh = meshes[asset["id"]]
        source_evidence[asset["id"]]["mesh_vertex_count"] = len(mesh.vertices)
        source_evidence[asset["id"]]["mesh_face_count"] = len(mesh.polygons)
    tag_assets = {
        asset["tag"].lower(): asset["id"] for asset in assets}

    evidence = {
        "schema": 1,
        "hrek_build": manifest["hrek_build"],
        "world_units_to_metres": scale,
        "axis_mapping": manifest["directx_to_tag_axes"],
        "source": source_evidence,
        "identities": [],
        "warnings": [],
    }
    collections = []
    cameras = []

    for order, identity in enumerate(identities, 1):
        asset_id = identity["id"]
        collection = bpy.data.collections.new(
            "%02d %s" % (order, asset_id))
        scene.collection.children.link(collection)
        collections.append(collection)

        model = bpy.data.objects.new(
            "mesh:%s" % asset_id, meshes[asset_id])
        model.color = (
            (0.18, 0.44, 0.72, 1.0)
            if "\\human\\" in identity["tag"]
            else (0.52, 0.25, 0.72, 1.0))
        model["reach_tag"] = identity["tag"]
        model["reach_frame"] = "identity_root"
        collection.objects.link(model)
        lock_reference(model)
        human_ref(collection)
        empty_ref(
            collection, "ref:axes_origin", Matrix.Identity(4),
            "ARROWS", 0.55)

        item_evidence = {
            "order": order,
            "id": asset_id,
            "tag": identity["tag"],
            "model_tag": identity["tag"] + ".model",
            "render_model_tag": identity["tag"] + ".render_model",
            "render_model_node_count":
                render_data[asset_id]["node_count"],
            "frame": "identity_root",
            "seats": [],
        }

        if identity.get("carrier"):
            carrier_id = tag_assets[identity["carrier"].lower()]
            attach_name = identity["carrier_marker"]
            attach, count = choose_marker(
                render_data[carrier_id], attach_name)
            if attach is None:
                raise ValueError("%s missing carrier marker %s" % (
                    asset_id, attach_name))
            carrier = bpy.data.objects.new(
                "ref:carrier:%s" % carrier_id, meshes[carrier_id])
            carrier.matrix_world = tag_matrix(
                attach, scale).inverted()
            carrier.display_type = "WIRE"
            carrier.color = (0.38, 0.42, 0.46, 1.0)
            carrier.show_in_front = True
            carrier["reach_tag"] = identity["carrier"]
            carrier["reach_frame"] = (
                "carrier inverse-transformed into direct-child frame")
            carrier["reach_attachment_marker"] = attach_name
            collection.objects.link(carrier)
            lock_reference(carrier)
            empty_ref(
                collection, "ref:carrier_attachment:%s" % attach_name,
                Matrix.Identity(4), "SPHERE", 0.14)
            item_evidence.update({
                "frame": "direct_child_unit",
                "carrier": identity["carrier"],
                "carrier_marker": attach_name,
                "carrier_marker_record_count": count,
            })

        for seat in seat_data[asset_id]:
            seat_name = "seat%d" % seat["index"]
            seat_marker, seat_count = choose_marker(
                render_data[asset_id], seat["seat_marker"])
            if seat_marker is None:
                raise ValueError("%s %s missing seat marker %s" % (
                    asset_id, seat_name, seat["seat_marker"]))
            seat_ref = empty_ref(
                collection,
                "ref:seat:%s:%s:%s" % (
                    asset_id, seat_name, seat["seat_marker"]),
                tag_matrix(seat_marker, scale), "CUBE", 0.14)
            seat_ref["reach_marker_record_count"] = seat_count

            camera_marker = None
            camera_count = 0
            if seat["camera_marker"]:
                camera_marker, camera_count = choose_marker(
                    render_data[asset_id], seat["camera_marker"])
                if camera_marker is not None:
                    camera_ref = empty_ref(
                        collection,
                        "ref:camera:%s:%s:%s" % (
                            asset_id, seat_name,
                            seat["camera_marker"]),
                        tag_matrix(camera_marker, scale),
                        "CONE", 0.16)
                    camera_ref["reach_marker_record_count"] = camera_count
                else:
                    evidence["warnings"].append(
                        "%s %s camera marker %s absent; seat marker used" % (
                            asset_id, seat_name, seat["camera_marker"]))
            seed = camera_marker or seat_marker
            source_label = (
                "unit camera marker %s" % seat["camera_marker"]
                if camera_marker else
                "seat marker %s" % seat["seat_marker"])
            camera_data = bpy.data.cameras.new(
                "camera:%s:%s" % (asset_id, seat_name))
            camera_data.lens = 50.0
            camera_data.clip_start = 0.01
            camera_data.clip_end = 500.0
            camera_data.display_size = 0.20
            camera = bpy.data.objects.new(
                "cam:%s:%s" % (asset_id, seat_name), camera_data)
            camera.matrix_world = (
                tag_matrix(seed, scale) @ CAMERA_BASIS)
            camera.show_name = True
            camera.show_in_front = True
            camera.color = (1.0, 0.32, 0.08, 1.0)
            camera.lock_rotation = (True, True, True)
            camera.lock_scale = (True, True, True)
            camera["reach_identity"] = asset_id
            camera["reach_raw_seat_index"] = seat["index"]
            camera["reach_seed_source"] = source_label
            camera["reach_status"] = "NEEDS USER EYE PLACEMENT"
            camera["reach_needs_user_placement"] = True
            camera["reach_frame"] = item_evidence["frame"]
            camera["reach_render_model_node_count"] = (
                render_data[asset_id]["node_count"])
            for axis in ("forward", "up", "right"):
                camera["reach_config_%s_key" % axis] = (
                    "vehicle_cam_%s_m_reach_%s_%s" %
                    (axis, asset_id, seat_name))
            collection.objects.link(camera)
            cameras.append(camera)
            label = empty_ref(
                collection, "NEEDS_PLACEMENT:%s" % camera.name,
                Matrix.Translation(
                    camera.matrix_world.translation +
                    Vector((0.0, 0.0, 0.22))),
                "CIRCLE", 0.22)
            label.color = (1.0, 0.1, 0.05, 1.0)

            base_wu = camera.matrix_world.translation / scale
            item_evidence["seats"].append({
                **seat,
                "camera_name": camera.name,
                "seed_source": source_label,
                "seed_record_count":
                    camera_count if camera_marker else seat_count,
                "seed_base_world_units": list(base_wu),
                "needs_user_placement": True,
            })
        evidence["identities"].append(item_evidence)

    if len(cameras) != 25:
        raise ValueError("expected 25 cameras, got %d" % len(cameras))
    scene["reach_camera_count"] = len(cameras)
    scene.camera = cameras[0]

    helper = args.helper.read_text(encoding="utf-8")
    helper_block = bpy.data.texts.new("reach_vr_seats.py")
    helper_block.write(helper)
    extractor = args.extractor.read_text(encoding="utf-8")
    extractor_block = bpy.data.texts.new("parse_reach_vehicle_points.py")
    extractor_block.write(extractor)
    evidence_json = json.dumps(evidence, indent=2)
    evidence_block = bpy.data.texts.new(
        "REACH_SOURCE_EVIDENCE.json")
    evidence_block.write(evidence_json)

    for layer_collection in (
            bpy.context.view_layer.layer_collection.children):
        layer_collection.exclude = (
            layer_collection.name != collections[0].name)
    for area in bpy.context.screen.areas if bpy.context.screen else []:
        if area.type == "VIEW_3D":
            area.spaces.active.shading.type = "SOLID"
            area.spaces.active.shading.color_type = "OBJECT"
            area.spaces.active.clip_start = 0.01
            area.spaces.active.clip_end = 1000.0

    bpy.ops.wm.save_as_mainfile(
        filepath=str(output), compress=True)
    shutil.copyfile(args.helper, output.parent / "reach_vr_seats.py")
    shutil.copyfile(
        args.extractor, output.parent / "parse_reach_vehicle_points.py")
    write_readme(output.parent / "README.txt", evidence)
    (output.parent / "reach_vehicle_scene.json").write_text(
        evidence_json + "\n", encoding="utf-8", newline="\n")
    print("REACH_KIT_READY")
    print("identities=20 cameras=25")
    print("blend=%s" % output)
    print("blend_sha256=%s" % digest(output))


if __name__ == "__main__":
    main()
