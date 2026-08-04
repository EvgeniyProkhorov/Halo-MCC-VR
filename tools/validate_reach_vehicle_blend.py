#!/usr/bin/env python3
"""Validate a generated or in-progress Reach vehicle-camera authoring kit."""

from __future__ import annotations

import json
import math
import sys

import bpy


NODE_COUNTS = {
    "banshee": 21, "space_banshee": 20, "ghost": 15, "revenant": 8,
    "wraith": 13, "wraith_gunner": 6, "mongoose": 19, "warthog": 27,
    "warthog_chaingun": 7, "warthog_gauss": 7, "warthog_rocket": 9,
    "falcon": 22, "sabre": 16, "scorpion": 21, "forklift": 14,
    "cart": 10, "shade_plasma": 11, "shade_flak": 11,
    "plasma_turret": 6, "machinegun": 7, "shade_base": 1,
}
CHILD_IDENTITIES = {
    "wraith_gunner", "warthog_chaingun", "warthog_gauss",
    "warthog_rocket", "shade_plasma", "shade_flak",
}


def check(condition, message, problems):
    if not condition:
        problems.append(message)


def finite_matrix(matrix):
    return all(math.isfinite(matrix[row][column])
               for row in range(4) for column in range(4))


def main():
    problems = []
    scene = bpy.context.scene
    check(bpy.data.is_saved, "blend is not saved", problems)
    check(scene.unit_settings.system == 'METRIC', "units are not metric", problems)
    check(scene.unit_settings.length_unit == 'METERS', "length unit is not metres", problems)
    check(abs(scene.unit_settings.scale_length - 1.0) < 1e-9,
          "scene unit scale is not 1.0", problems)
    check(scene.get("reach_identity_count") == 20,
          "scene identity count metadata is not 20", problems)
    check(scene.get("reach_camera_count") == 25,
          "scene camera count metadata is not 25", problems)

    helper = bpy.data.texts.get("reach_vr_seats.py")
    extractor = bpy.data.texts.get("parse_reach_vehicle_points.py")
    evidence_text = bpy.data.texts.get("REACH_SOURCE_EVIDENCE.json")
    check(helper is not None, "embedded Reach helper is missing", problems)
    check(extractor is not None, "embedded point extractor is missing", problems)
    check(evidence_text is not None, "embedded source evidence is missing", problems)
    if helper is not None:
        helper_source = helper.as_string()
        check("use_positional_tracking = False" in helper_source,
              "helper does not disable positional tracking", problems)
        check("reach.mark_vehicle_seat_placed" in helper_source,
              "helper lacks placement acknowledgement", problems)
        check('"version": (1, 1, 0)' in helper_source and
              "def draw_camera_translation" in helper_source and
              all('"location", index=%d' % axis in helper_source
                  for axis in range(3)),
              "helper lacks the inline translation-only controls", problems)
    if evidence_text is None:
        evidence = {"identities": [], "source": {}, "warnings": []}
    else:
        evidence = json.loads(evidence_text.as_string())
    check(evidence.get("world_units_to_metres") == 3.048,
          "evidence scale is not 3.048", problems)
    check(evidence.get("hrek_build") == "2023.07.17.176677.1-QFE1",
          "unexpected HREK build", problems)

    identities = evidence.get("identities", [])
    expected_collections = ["%02d %s" % (item["order"], item["id"])
                            for item in identities]
    actual_collections = [collection.name for collection in scene.collection.children]
    check(len(identities) == 20, "evidence does not have 20 identities", problems)
    check(actual_collections == expected_collections,
          "top-level identity collection order changed", problems)

    source = evidence.get("source", {})
    check(set(source) == set(NODE_COUNTS), "source identity set changed", problems)
    for identity_id, expected_nodes in NODE_COUNTS.items():
        record = source.get(identity_id, {})
        check(record.get("node_count") == expected_nodes,
              "%s node count is not %d" % (identity_id, expected_nodes), problems)
        for field in ("vehicle_sha256", "model_sha256",
                      "render_model_sha256", "mesh_sha256"):
            value = record.get(field, "")
            check(len(value) == 64 and all(char in "0123456789ABCDEF" for char in value),
                  "%s %s is not an uppercase SHA-256" % (identity_id, field), problems)

    expected_cameras = []
    for identity in identities:
        identity_id = identity["id"]
        collection = bpy.data.collections.get("%02d %s" %
                                              (identity["order"], identity_id))
        if collection is None:
            continue
        cameras = [obj for obj in collection.objects if obj.type == 'CAMERA']
        seat_names = [seat["camera_name"] for seat in identity["seats"]]
        expected_cameras.extend(seat_names)
        check(sorted(obj.name for obj in cameras) == sorted(seat_names),
              "%s camera membership changed" % identity_id, problems)
        model = collection.objects.get("mesh:%s" % identity_id)
        check(model is not None and model.type == 'MESH',
              "%s HREK mesh is missing" % identity_id, problems)
        if model is not None:
            check(model.data.get("reach_hrek_mesh_sha256") ==
                  source.get(identity_id, {}).get("mesh_sha256"),
                  "%s mesh source hash changed" % identity_id, problems)
            check(len(model.data.vertices) ==
                  source.get(identity_id, {}).get("mesh_vertex_count"),
                  "%s mesh vertex count changed" % identity_id, problems)
            check(len(model.data.polygons) ==
                  source.get(identity_id, {}).get("mesh_face_count"),
                  "%s mesh face count changed" % identity_id, problems)
        humans = [obj for obj in collection.objects
                  if obj.name.startswith("ref:2.08m_human:")]
        human = humans[0] if len(humans) == 1 else None
        check(human is not None, "%s human reference is missing" % identity_id, problems)
        if human is not None:
            check(abs(human.dimensions.z - 2.08) < 1e-6,
                  "%s human reference is not 2.08m" % identity_id, problems)
        carriers = [obj for obj in collection.objects
                    if obj.name.startswith("ref:carrier:")]
        if identity_id in CHILD_IDENTITIES:
            check(identity.get("frame") == "direct_child_unit",
                  "%s lost direct-child frame evidence" % identity_id, problems)
            check(len(carriers) == 1 and finite_matrix(carriers[0].matrix_world),
                  "%s carrier-frame reference is invalid" % identity_id, problems)
        else:
            check(identity.get("frame") == "identity_root",
                  "%s frame is not identity root" % identity_id, problems)
            check(not carriers, "%s unexpectedly has a carrier reference" % identity_id,
                  problems)
        for obj in collection.objects:
            if obj.type == 'CAMERA':
                check(not obj.hide_select and not any(obj.lock_location) and
                      all(obj.lock_rotation) and all(obj.lock_scale),
                      "%s translation-only authoring locks changed" % obj.name,
                      problems)
            else:
                check(bool(obj.get("reach_locked_reference")) and obj.hide_select and
                      all(obj.lock_location) and all(obj.lock_rotation) and
                      all(obj.lock_scale), "%s is not locked reference geometry" % obj.name,
                      problems)

    actual_cameras = sorted(obj.name for obj in bpy.data.objects
                            if obj.type == 'CAMERA' and obj.name.startswith("cam:"))
    check(len(expected_cameras) == 25 and len(set(expected_cameras)) == 25,
          "evidence camera list is not 25 unique cameras", problems)
    check(actual_cameras == sorted(expected_cameras),
          "blend camera set changed", problems)
    for name in expected_cameras:
        camera = bpy.data.objects.get(name)
        if camera is None:
            continue
        check(finite_matrix(camera.matrix_world), "%s matrix is non-finite" % name, problems)
        check(isinstance(
                  camera.get("reach_needs_user_placement", None), bool),
              "%s placement acknowledgement is missing or invalid" % name,
              problems)
        rotation = camera.matrix_world.to_3x3()
        check(abs(rotation.determinant() - 1.0) < 1e-5,
              "%s camera basis is reflected or scaled" % name, problems)

    layers = list(bpy.context.view_layer.layer_collection.children)
    enabled = [layer.name for layer in layers if not layer.exclude]
    check(len(enabled) == 1 and enabled[0] in expected_collections,
          "authoring view must isolate exactly one known identity collection",
          problems)
    check(evidence.get("warnings") == [
        "wraith seat0 camera marker driver_camera absent; seat marker used"],
        "unexpected HREK marker warnings", problems)

    if problems:
        for problem in problems:
            print("VALIDATION_ERROR: " + problem, file=sys.stderr)
        raise SystemExit(2)
    print("VALIDATION_OK")
    print("collections=20 cameras=25 source_assets=21 child_frames=6")


if __name__ == "__main__":
    main()
