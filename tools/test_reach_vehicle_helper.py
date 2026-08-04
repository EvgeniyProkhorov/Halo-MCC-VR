#!/usr/bin/env python3
"""No-save Blender smoke test for the Reach Seats inline transform controls."""

from __future__ import annotations

import argparse
import sys
import types
from pathlib import Path

import bpy


VR_ADDON = "viewport_vr_preview"
HELPER_NAME = "reach_vr_seats.py"
CAMERA_PREFIX = "cam:"


def arguments():
    parser = argparse.ArgumentParser(
        description="Test the embedded Reach seat helper without saving")
    parser.add_argument("--blend", required=True, type=Path)
    parser.add_argument("--helper", required=True, type=Path)
    values = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    return parser.parse_args(values)


def normalized(text):
    return text.replace("\r\n", "\n").replace("\r", "\n")


def vector(values):
    return tuple(float(value) for value in values)


def camera_state(camera):
    return {
        "location": vector(camera.location),
        "rotation_euler": vector(camera.rotation_euler),
        "rotation_quaternion": vector(camera.rotation_quaternion),
        "scale": vector(camera.scale),
        "delta_location": vector(camera.delta_location),
        "delta_rotation_euler": vector(camera.delta_rotation_euler),
        "delta_scale": vector(camera.delta_scale),
        "lock_location": tuple(camera.lock_location),
        "lock_rotation": tuple(camera.lock_rotation),
        "lock_scale": tuple(camera.lock_scale),
        "needs_placement": camera.get("reach_needs_user_placement", True),
    }


class LayoutRecorder:
    def __init__(self):
        self.properties = []
        self.use_property_split = False
        self.use_property_decorate = True

    def box(self):
        return self

    def label(self, **_kwargs):
        return None

    def prop(self, data, prop, **kwargs):
        self.properties.append({
            "data": data,
            "prop": prop,
            "index": kwargs.get("index"),
            "text": kwargs.get("text"),
        })


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    args = arguments()
    if VR_ADDON not in bpy.context.preferences.addons:
        bpy.ops.preferences.addon_enable(module=VR_ADDON)
    bpy.ops.wm.open_mainfile(filepath=str(args.blend.resolve()))
    embedded = bpy.data.texts.get(HELPER_NAME)
    require(embedded is not None, "embedded Reach helper is missing")
    source = args.helper.resolve().read_text(encoding="utf-8")
    require(
        normalized(embedded.as_string()) == normalized(source),
        "embedded and external helpers differ")

    module = types.ModuleType("reach_vehicle_helper_test")
    module.__file__ = str(args.helper.resolve())
    sys.modules[module.__name__] = module
    namespace = module.__dict__
    exec(compile(source, str(args.helper), "exec"), namespace)
    namespace["unregister"]()
    require(namespace["ensure_addon"](), "VR landmarks are unavailable")
    namespace["register"]()
    require(namespace["build_landmarks"]() == 25, "did not build 25 landmarks")

    cameras = sorted(
        (obj for obj in bpy.data.objects
         if obj.type == 'CAMERA' and obj.name.startswith(CAMERA_PREFIX)),
        key=lambda obj: obj.name)
    require(len(cameras) == 25, "camera count changed")
    original = {camera.name: camera_state(camera) for camera in cameras}
    require(namespace["goto_index"](bpy.context, 0), "could not select first seat")
    first = bpy.context.scene.vr_landmarks[0].base_pose_object

    recorder = LayoutRecorder()
    namespace["draw_camera_translation"](recorder, first)
    expected = [
        ("location", 0, "Forward (+X)"),
        ("location", 1, "Left (+Y)"),
        ("location", 2, "Up (+Z)"),
    ]
    observed = [
        (item["prop"], item["index"], item["text"])
        for item in recorder.properties]
    require(observed == expected, "inline transform fields changed: %r" % observed)
    require(
        all(item["data"] is first for item in recorder.properties),
        "inline fields do not target the active camera")

    offsets = (0.101, 0.202, 0.303)
    for item, offset in zip(recorder.properties, offsets):
        item["data"].location[item["index"]] += offset
    bpy.context.view_layer.update()
    changed = camera_state(first)
    before = original[first.name]
    for index, offset in enumerate(offsets):
        require(
            abs(changed["location"][index] -
                (before["location"][index] + offset)) < 1e-6,
            "location component %d did not update" % index)
    for field in (
            "rotation_euler", "rotation_quaternion", "scale",
            "delta_location", "delta_rotation_euler", "delta_scale",
            "lock_location", "lock_rotation", "lock_scale",
            "needs_placement"):
        require(changed[field] == before[field],
                "translation edit changed %s" % field)
    for camera in cameras:
        if camera is not first:
            require(camera_state(camera) == original[camera.name],
                    "translation edit changed %s" % camera.name)
    location_delta = tuple(
        changed["location"][index] - before["location"][index]
        for index in range(3))
    runtime_trim = (
        location_delta[0], -location_delta[1], location_delta[2])
    require(
        all(abs(actual - expected) < 1e-6
            for actual, expected in zip(
                runtime_trim, (0.101, -0.202, 0.303))),
        "runtime forward/right/up mapping changed: %r" % (runtime_trim,))

    require(namespace["goto_index"](bpy.context, 1), "could not select next seat")
    second = bpy.context.scene.vr_landmarks[1].base_pose_object
    second_recorder = LayoutRecorder()
    namespace["draw_camera_translation"](second_recorder, second)
    require(
        all(item["data"] is second for item in second_recorder.properties),
        "seat navigation did not retarget inline controls")

    first.location = before["location"]
    bpy.context.view_layer.update()
    require(camera_state(first) == before, "test did not restore active camera")
    namespace["unregister"]()
    sys.modules.pop(module.__name__, None)
    print("REACH_SEAT_UI_TEST_OK")
    print("cameras=25 controls=3 axes=forward,left,up")


if __name__ == "__main__":
    main()
