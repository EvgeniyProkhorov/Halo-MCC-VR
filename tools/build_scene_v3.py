# Kit v3: one COLLECTION per vehicle, every vehicle at the WORLD ORIGIN.
# Run: blender --background --factory-startup --python build_scene_v3.py -- \
#        <objs_tag_dir> <authored_points.json> <seat_markers.json>
#        <markers.json> <nodes.json> <out_blend>
#
# Why v3 exists: v1/v2 laid the vehicles out in a row, so every camera's
# number depended on its vehicle's grid offset. Here each vehicle sits at
# (0,0,0) with an identity transform, so a camera's WORLD position IS the
# model-space point (metres / 3.048 = world units). Nothing to subtract,
# nothing to mis-parent.
#
# Mounted turret gunner seats live in their CARRIER's collection with the gun
# shown bolted in place: the game stores a mounted turret's position relative
# to the carrier's attachment NODE, so a point authored in the floating gun's
# own space lands ~1.3 m off. Authoring the gunner eye against the carrier is
# both correct at runtime and the only way to see where a head belongs.
import json
import math
import os
import sys

import bpy
from mathutils import Matrix, Vector

argv = sys.argv[sys.argv.index("--") + 1:]
OBJS, POINTS_JSON, SEATS_JSON, MARKERS_JSON, NODES_JSON, OUT = argv[:6]

WU_TO_M = 3.048
EYE_UP_WU = 0.33          # seated eye above a seat marker, user-tunable

with open(POINTS_JSON) as f:
    AUTHORED = json.load(f).get("points", {})
with open(SEATS_JSON) as f:
    SEAT_MARKERS = json.load(f)
with open(MARKERS_JSON) as f:
    MARKERS = json.load(f)
with open(NODES_JSON) as f:
    NODES = json.load(f)


def marker_pos(model, group):
    """Compose a marker into its model's tag space (node world + local)."""
    inst = MARKERS.get(model, {}).get(group)
    if not inst:
        return None
    i = inst[0]
    t = i.get("translation", [0.0, 0.0, 0.0])
    node = i.get("node", -1)
    nodes = NODES.get(model, [])
    base = nodes[node]["world"] if isinstance(node, int) and 0 <= node < len(
        nodes) else [0.0, 0.0, 0.0]
    return [base[k] + t[k] for k in range(3)]


def seat_marker(model, label):
    info = SEAT_MARKERS.get(model, {}).get(label)
    if isinstance(info, dict) and "tag_pos" in info:
        return list(info["tag_pos"])
    return marker_pos(model, label)


# (collection name, mesh key, [(seat label, seat marker source)],
#  [(ref mesh key, mount marker on carrier)])
LINEUP = [
    ("01 Warthog", "warthog",
     [("warthog_d", ("warthog", "warthog_d")),
      ("warthog_p", ("warthog", "warthog_p")),
      ("warthog_g", ("hog_chaingun", "camera_gunner"))],
     [("hog_chaingun", "turret")]),
    ("02 Mongoose", "mongoose",
     [("mongoose_d", ("mongoose", "mongoose_d")),
      ("mongoose_p", ("mongoose", "mongoose_p"))], []),
    ("03 Ghost", "ghost", [("ghost_d", ("ghost", "ghost_d"))], []),
    ("04 Chopper", "brute_chopper",
     [("chopper_d", ("brute_chopper", "chopper_d"))], []),
    ("05 Banshee", "banshee", [("banshee_d", ("banshee", "banshee_d"))], []),
    ("06 Hornet", "hornet",
     [("hornet_d", ("hornet", "hornet_d")),
      ("hornet_p_l", ("hornet", "hornet_p_l")),
      ("hornet_p_r", ("hornet", "hornet_p_r"))], []),
    ("07 Scorpion", "scorpion",
     [("scorpion_d", ("scorpion", "scorpion_d")),
      ("scorpion_g", ("scorpion", "anti_infantry_turret"))],
     [("scorpion_turret", "anti_infantry_turret")]),
    ("08 Wraith", "wraith",
     [("wraith_d", ("wraith", "wraith_d")),
      ("wraith_g", ("wraith", "anti_infantry_turret"))],
     [("wraith_turret", "anti_infantry_turret")]),
    ("09 Prowler", "mauler",
     [("mauler_d", ("mauler", "mauler_d")),
      ("mauler_p_l", ("mauler", "mauler_p_l")),
      ("mauler_p_r", ("mauler", "mauler_p_r")),
      ("mauler_g", ("mauler", "mauler_g"))], []),
    ("10 Shade", "shade", [("shade_d", ("shade", "shade_d"))], []),
    ("11 Turret machinegun", "machinegun_turret",
     [("turret_g", ("machinegun_turret", "camera"))], []),
    ("12 Turret plasma cannon", "plasma_cannon",
     [("turret_g", ("plasma_cannon", "camera"))], []),
    ("13 Turret missile pod", "missile_pod",
     [("turret_g", ("missile_pod", "camera"))], []),
]

# Gunner seats whose authored frame CHANGED from the floating gun to the
# carrier: their old v1/v2 numbers are not reusable and are re-seeded from
# the engine's own gunner-camera marker instead.
REFRAMED = {("warthog", "warthog_g"), ("scorpion", "scorpion_g"),
            ("wraith", "wraith_g"), ("mauler", "mauler_g")}

# Old authored keys, so unchanged seats keep the user's own work.
OLD_KEY = {
    "warthog_d": ("warthog", "warthog_d"),
    "warthog_p": ("warthog", "warthog_p"),
    "mongoose_d": ("mongoose", "mongoose_d"),
    "mongoose_p": ("mongoose", "mongoose_p"),
    "ghost_d": ("ghost", "ghost_d"),
    "chopper_d": ("brute_chopper", "chopper_d"),
    "banshee_d": ("banshee", "banshee_d"),
    "hornet_d": ("hornet", "hornet_d"),
    "hornet_p_l": ("hornet", "hornet_p_l"),
    "hornet_p_r": ("hornet", "hornet_p_r"),
    "scorpion_d": ("scorpion", "scorpion_d"),
    "wraith_d": ("wraith", "wraith_d"),
    "mauler_d": ("mauler", "mauler_d"),
    "mauler_p_l": ("mauler", "mauler_p_l"),
    "mauler_p_r": ("mauler", "mauler_p_r"),
    "shade_d": ("shade", "shade_d"),
}
TURRET_OLD = {
    "machinegun_turret": ("machinegun_turret", "turret_g"),
    "plasma_cannon": ("plasma_cannon", "turret_g"),
    "missile_pod": ("missile_pod", "turret_g"),
}

CAM_ROT = Matrix((
    (0.0, 0.0, -1.0),
    (-1.0, 0.0, 0.0),
    (0.0, 1.0, 0.0))).to_4x4()

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.unit_settings.system = 'METRIC'
scene.render.resolution_x = 1400
scene.render.resolution_y = 1000
root_coll = scene.collection
notes = []


def wu(p):
    return Vector((p[0] * WU_TO_M, p[1] * WU_TO_M, p[2] * WU_TO_M))


def import_mesh(path, name):
    before = set(bpy.data.objects)
    bpy.ops.wm.obj_import(filepath=path, forward_axis='Y', up_axis='Z')
    imported = [o for o in bpy.data.objects if o not in before]
    for idx, obj in enumerate(imported):
        obj.name = name if idx == 0 else "%s_%d" % (name, idx)
        obj.matrix_basis = Matrix.Identity(4)     # sits AT the world origin
    return imported


for coll_name, mesh_key, seats, refs in LINEUP:
    path = os.path.join(OBJS, mesh_key + ".obj")
    if not os.path.exists(path):
        notes.append("missing obj: " + mesh_key)
        continue
    coll = bpy.data.collections.new(coll_name)
    root_coll.children.link(coll)

    parts = import_mesh(path, mesh_key)
    for obj in parts:
        for c in list(obj.users_collection):
            c.objects.unlink(obj)
        coll.objects.link(obj)
    carrier = parts[0]

    lo = Vector((1e9,) * 3)
    hi = Vector((-1e9,) * 3)
    for obj in parts:
        for corner in obj.bound_box:
            v = Vector(corner)
            lo = Vector(map(min, lo, v))
            hi = Vector(map(max, hi, v))

    # reference geometry: the mounted gun, bolted where the game bolts it
    for ref_key, mount_group in refs:
        ref_path = os.path.join(OBJS, ref_key + ".obj")
        mount = marker_pos(mesh_key, mount_group)
        if not os.path.exists(ref_path) or mount is None:
            notes.append("no ref mesh/mount for %s on %s" % (ref_key, mesh_key))
            continue
        ref_parts = import_mesh(ref_path, "REF_%s_DO_NOT_MOVE" % ref_key)
        for obj in ref_parts:
            for c in list(obj.users_collection):
                c.objects.unlink(obj)
            coll.objects.link(obj)
            obj.matrix_basis = Matrix.Translation(wu(mount))
            obj.hide_select = True          # can't be grabbed by accident

    # engine seat anchors, for reference while nudging
    for label, info in SEAT_MARKERS.get(mesh_key, {}).items():
        pos = info["tag_pos"] if isinstance(info, dict) and "tag_pos" in info \
            else None
        if pos is None:
            continue
        gz = bpy.data.objects.new("seat:%s:%s" % (mesh_key, label), None)
        gz.empty_display_type = 'ARROWS'
        gz.empty_display_size = 0.18
        coll.objects.link(gz)
        gz.matrix_basis = Matrix.Translation(wu(pos))
        gz.hide_select = True

    for seat_label, (src_model, src_group) in seats:
        cam_data = bpy.data.cameras.new("cam:%s:%s" % (mesh_key, seat_label))
        cam_data.lens_unit = 'FOV'
        cam_data.sensor_fit = 'HORIZONTAL'
        cam_data.angle = math.radians(100.0)
        cam_data.clip_start = 0.01
        cam_data.clip_end = 500.0
        cam_data.display_size = 0.35
        cam = bpy.data.objects.new("cam:%s:%s" % (mesh_key, seat_label),
                                   cam_data)
        coll.objects.link(cam)
        cam.parent = carrier                    # carrier is identity at origin
        cam.matrix_parent_inverse = Matrix.Identity(4)

        pos = None
        reframed = (mesh_key, seat_label) in REFRAMED
        if not reframed:
            key = OLD_KEY.get(seat_label) or TURRET_OLD.get(mesh_key)
            if key and key[0] in AUTHORED and key[1] in AUTHORED[key[0]]:
                pos = list(AUTHORED[key[0]][key[1]])     # user's own point
        if pos is None:
            if src_model == mesh_key:
                anchor = seat_marker(mesh_key, src_group)
                if anchor:
                    pos = [anchor[0], anchor[1], anchor[2] + EYE_UP_WU]
            else:
                # gunner seat: carrier mount + the gun's own camera marker
                mount = marker_pos(mesh_key, refs[0][1]) if refs else None
                local = marker_pos(src_model, src_group)
                if mount and local:
                    pos = [mount[k] + local[k] for k in range(3)]
        if pos is None:
            notes.append("no seed for %s:%s (placed above bbox)"
                         % (mesh_key, seat_label))
            pos = [(lo.x + hi.x) * 0.5 / WU_TO_M,
                   (lo.y + hi.y) * 0.5 / WU_TO_M, hi.z / WU_TO_M]
        cam.matrix_basis = Matrix.Translation(wu(pos)) @ CAM_ROT

    # Master Chief, standing on the ground beside this vehicle
    mesh = bpy.data.meshes.new("chief_%s" % mesh_key)
    h, w = 2.08, 0.45
    verts = [(x, y, z) for x in (-w / 2, w / 2) for y in (-w / 2, w / 2)
             for z in (0.0, h)]
    faces = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 2, 6, 4), (1, 5, 7, 3),
             (0, 4, 5, 1), (2, 3, 7, 6)]
    mesh.from_pydata(verts, [], faces)
    ref = bpy.data.objects.new("MASTER_CHIEF_2m08", mesh)
    coll.objects.link(ref)
    ref.location = (0.0, hi.y + 0.9, lo.z)
    ref.hide_select = True

# Show only the first vehicle; tick a collection's checkbox in the Outliner
# to switch to another. `exclude` (not the eye) so a hidden vehicle is truly
# unloaded — every vehicle sits at the same origin, so two visible at once
# would interpenetrate.
for idx, lc in enumerate(bpy.context.view_layer.layer_collection.children):
    lc.exclude = idx != 0

for screen in bpy.data.screens:
    for area in screen.areas:
        if area.type == 'VIEW_3D':
            for space in area.spaces:
                if space.type == 'VIEW_3D':
                    space.clip_start = 0.01
                    space.clip_end = 2000.0

bpy.ops.wm.save_as_mainfile(filepath=OUT)
print("SAVED", OUT)
for n in notes:
    print("NOTE", n)
