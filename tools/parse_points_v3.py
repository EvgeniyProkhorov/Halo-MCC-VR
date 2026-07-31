# Read authored seat cameras out of a kit-v3 .blend.
# v3 puts every vehicle at the world origin with an identity transform, so a
# camera's world position IS the model-space point: metres / 3.048 = wu.
#
# TRAP (hit and fixed 2026-07-31): Blender does not evaluate matrix_world for
# objects in collections excluded from the view layer, so a file saved with
# 12 of 13 vehicles switched off parses as (0,0,0) for all of them — silently.
# This parser therefore (a) re-enables every collection first, and (b) also
# computes each position from STORED matrices (matrix_basis /
# matrix_parent_inverse, which never depend on the view layer) and refuses to
# emit any camera where the two disagree.
# Run: blender --background <blend> --python parse_points_v3.py -- <out_json>
import json
import sys

import bpy
from mathutils import Matrix

out_path = sys.argv[sys.argv.index("--") + 1]
WU_TO_M = 3.048

for lc in bpy.context.view_layer.layer_collection.children:
    lc.exclude = False
    lc.hide_viewport = False
bpy.context.view_layer.update()


def stored_world(obj, depth=0):
    """World matrix from stored data only (no dependency graph)."""
    if obj.parent is None or depth > 8:
        return obj.matrix_basis.copy()
    return (stored_world(obj.parent, depth + 1) @
            obj.matrix_parent_inverse @ obj.matrix_basis)


points = {}
problems = []
for obj in bpy.data.objects:
    if not obj.name.startswith("cam:"):
        continue
    parts = obj.name.split(":")
    if len(parts) != 3:
        problems.append("unparseable camera name: " + obj.name)
        continue
    _, model, seat = parts
    evaluated = obj.matrix_world.translation
    stored = stored_world(obj).translation
    drift = max(abs(evaluated[k] - stored[k]) for k in range(3))
    if drift > 1e-4:
        problems.append(
            "%s: evaluated vs stored position differ by %.4f m - refusing "
            "this camera" % (obj.name, drift))
        continue
    if max(abs(stored[k]) for k in range(3)) < 1e-6:
        problems.append(
            "%s: sits exactly at the origin - camera never placed?"
            % obj.name)
    points.setdefault(model, {})[seat] = [
        stored.x / WU_TO_M, stored.y / WU_TO_M, stored.z / WU_TO_M]

with open(out_path, "w") as f:
    json.dump({"points": points, "problems": problems}, f, indent=1)
print("WROTE", out_path, "cameras:", sum(len(v) for v in points.values()),
      "problems:", len(problems))
for p in problems:
    print("PROBLEM", p)
