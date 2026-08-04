#!/usr/bin/env python3
"""Replace only the embedded Reach seat helper in an existing authoring file."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

import bpy


HELPER_NAME = "reach_vr_seats.py"
VR_ADDON = "viewport_vr_preview"
CAMERA_PREFIX = "cam:"


def arguments():
    parser = argparse.ArgumentParser(
        description="Safely stage an updated Reach seat helper")
    parser.add_argument("--blend", required=True, type=Path)
    parser.add_argument("--helper", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    values = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    return parser.parse_args(values)


def normalized(text):
    return text.replace("\r\n", "\n").replace("\r", "\n")


def digest(text):
    return hashlib.sha256(normalized(text).encode("utf-8")).hexdigest().upper()


def stable(value):
    if value is None or isinstance(value, (bool, int, float, str)):
        return value
    if hasattr(value, "to_list"):
        return stable(value.to_list())
    if hasattr(value, "items"):
        return {str(key): stable(item)
                for key, item in sorted(value.items(), key=lambda pair: str(pair[0]))}
    try:
        return [stable(item) for item in value]
    except TypeError:
        return str(value)


def vector(values):
    return [round(float(value), 12) for value in values]


def custom_properties(owner):
    return {key: stable(owner[key]) for key in sorted(owner.keys())}


def object_record(obj):
    return {
        "type": obj.type,
        "data": obj.data.name if obj.data else None,
        "parent": obj.parent.name if obj.parent else None,
        "collections": sorted(collection.name for collection in obj.users_collection),
        "location": vector(obj.location),
        "rotation_mode": obj.rotation_mode,
        "rotation_euler": vector(obj.rotation_euler),
        "rotation_quaternion": vector(obj.rotation_quaternion),
        "scale": vector(obj.scale),
        "delta_location": vector(obj.delta_location),
        "delta_rotation_euler": vector(obj.delta_rotation_euler),
        "delta_scale": vector(obj.delta_scale),
        "lock_location": list(obj.lock_location),
        "lock_rotation": list(obj.lock_rotation),
        "lock_scale": list(obj.lock_scale),
        "hide_select": bool(obj.hide_select),
        "hide_viewport": bool(obj.hide_viewport),
        "hide_render": bool(obj.hide_render),
        "custom": custom_properties(obj),
    }


def snapshot():
    scene = bpy.context.scene
    view_layer = bpy.context.view_layer
    landmarks = getattr(scene, "vr_landmarks", [])
    texts = {
        text.name: digest(text.as_string())
        for text in bpy.data.texts if text.name != HELPER_NAME
    }
    records = {
        obj.name: object_record(obj)
        for obj in sorted(bpy.data.objects, key=lambda item: item.name)
    }
    return {
        "objects": records,
        "collections": {
            collection.name: {
                "objects": sorted(obj.name for obj in collection.objects),
                "children": sorted(child.name for child in collection.children),
            }
            for collection in sorted(
                bpy.data.collections, key=lambda item: item.name)
        },
        "data_names": {
            "objects": sorted(item.name for item in bpy.data.objects),
            "meshes": sorted(item.name for item in bpy.data.meshes),
            "cameras": sorted(item.name for item in bpy.data.cameras),
            "collections": sorted(item.name for item in bpy.data.collections),
            "texts": sorted(item.name for item in bpy.data.texts),
        },
        "top_layers": {
            layer.name: {
                "exclude": bool(layer.exclude),
                "hide_viewport": bool(layer.hide_viewport),
            }
            for layer in view_layer.layer_collection.children
        },
        "scene_camera": scene.camera.name if scene.camera else None,
        "scene_custom": custom_properties(scene),
        "active_object": (
            view_layer.objects.active.name
            if view_layer.objects.active else None),
        "selected_objects": sorted(obj.name for obj in bpy.context.selected_objects),
        "landmarks": [
            {
                "name": landmark.name,
                "type": landmark.type,
                "object": (
                    landmark.base_pose_object.name
                    if landmark.base_pose_object else None),
                "scale": round(float(landmark.base_scale), 12),
            }
            for landmark in landmarks
        ],
        "landmark_selected": getattr(scene, "vr_landmarks_selected", None),
        "landmark_active": getattr(scene, "vr_landmarks_active", None),
        "texts": texts,
    }


def snapshot_digest(value):
    payload = json.dumps(value, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(payload.encode("utf-8")).hexdigest().upper()


def enable_vr_addon():
    if VR_ADDON not in bpy.context.preferences.addons:
        bpy.ops.preferences.addon_enable(module=VR_ADDON)
    if not hasattr(bpy.types.Scene, "vr_landmarks"):
        raise RuntimeError("VR Scene Inspection did not register landmarks")


def main():
    args = arguments()
    blend = args.blend.resolve()
    helper_path = args.helper.resolve()
    output = args.output.resolve()
    if blend == output:
        raise SystemExit("ERROR: output must be a staging file")
    if blend.parent != output.parent:
        raise SystemExit("ERROR: stage file must stay beside the source blend")
    if output.exists():
        raise SystemExit("ERROR: refusing to overwrite stage file: %s" % output)
    helper_source = helper_path.read_text(encoding="utf-8")
    compile(helper_source, str(helper_path), "exec")

    enable_vr_addon()
    bpy.ops.wm.open_mainfile(filepath=str(blend))
    cameras = [obj for obj in bpy.data.objects
               if obj.type == 'CAMERA' and obj.name.startswith(CAMERA_PREFIX)]
    if len(cameras) != 25 or len(bpy.context.scene.vr_landmarks) != 25:
        raise SystemExit(
            "ERROR: expected 25 cameras and 25 decoded VR landmarks")
    block = bpy.data.texts.get(HELPER_NAME)
    if block is None:
        raise SystemExit("ERROR: blend has no embedded %s" % HELPER_NAME)
    before = snapshot()
    old_hash = digest(block.as_string())
    block.clear()
    block.write(helper_source)
    if snapshot() != before:
        raise SystemExit("ERROR: in-memory update changed non-helper state")

    bpy.ops.wm.save_as_mainfile(
        filepath=str(output), check_existing=False, compress=True,
        relative_remap=False, copy=True)
    bpy.ops.wm.open_mainfile(filepath=str(output))
    after = snapshot()
    if after != before:
        changed = sorted(
            key for key in set(before) | set(after)
            if before.get(key) != after.get(key))
        raise SystemExit(
            "ERROR: staged save changed non-helper state: %s" %
            ", ".join(changed))
    staged = bpy.data.texts.get(HELPER_NAME)
    if staged is None or normalized(staged.as_string()) != normalized(helper_source):
        raise SystemExit("ERROR: staged helper does not match source")
    print("HELPER_UPDATE_OK")
    print("source=%s" % blend)
    print("output=%s" % output)
    print("state_sha256=%s" % snapshot_digest(after))
    print("old_helper_sha256=%s" % old_hash)
    print("new_helper_sha256=%s" % digest(helper_source))


if __name__ == "__main__":
    main()
