#!/usr/bin/env python3
"""Regenerate vehicle OBJs in TAG SPACE, meters, winding-corrected.

The H3EK .x exports store verts as (x, up, lateral). The kit v1 mistake was
mapping lateral with a negation (tag_y = -mesh_z), which mirrored every
vehicle (user-confirmed: driver seat rendered on the right). The verified
mapping is set by TAG_Y_SIGN below:
  +1  -> tag = (x, +z, y)  pure swap; .x file is left-handed  (v2 default)
  -1  -> tag = (x, -z, y)  the (wrong) v1 mapping, kept for A/B renders
A two-axis swap has det -1, so faces are re-wound to keep normals outward.
Output verts are premultiplied by 3.048 (wu -> meters) so the Blender scene
imports with an identity transform and cameras parent to meshes cleanly.
"""
import os, sys
import xparse

BASE = os.path.dirname(os.path.abspath(__file__))
MESHES = os.path.join(BASE, 'meshes')
TAG_Y_SIGN = +1 if '--v1-mirror' not in sys.argv else -1
OUT = os.path.join(BASE, 'objs-tag' if TAG_Y_SIGN > 0 else 'objs-mirror')
WU_TO_M = 3.048

NAMES = ['warthog', 'mongoose', 'ghost', 'scorpion', 'wraith', 'banshee',
         'hornet', 'brute_chopper', 'mauler', 'shade', 'hog_chaingun',
         'hog_gauss', 'hog_troop', 'scorpion_turret', 'wraith_turret',
         'mauler_turret', 'machinegun_turret', 'plasma_cannon', 'missile_pod']

def find_x(name):
    cands = [fn for fn in os.listdir(MESHES)
             if fn.lower().startswith(name + '.') and '.x' in fn.lower()[len(name):]
             and not fn.lower().endswith(('.xml', '.dds'))]
    for pref in (name + '.x.x', name + '.mesh.x', name + '.x'):
        if pref in cands:
            return os.path.join(MESHES, pref)
    return os.path.join(MESHES, cands[0]) if cands else None

os.makedirs(OUT, exist_ok=True)
for name in NAMES:
    xp = find_x(name)
    if xp is None:
        print('MISSING', name)
        continue
    roots, meshes, anomalies = xparse.load(xp)
    with open(os.path.join(OUT, name + '.obj'), 'w', newline='\n') as fh:
        fh.write('# tag-space meters; tag=(x, %+dz, y) from .x; winding %s\n'
                 % (TAG_Y_SIGN, 'reversed' if TAG_Y_SIGN > 0 else 'kept'))
        base = 0
        for idx, m in enumerate(meshes):
            fh.write('o %s\n' % (name if len(meshes) == 1
                                 else '%s_%d' % (name, idx)))
            for v in m['verts']:
                tx, ty, tz = v[0], TAG_Y_SIGN * v[2], v[1]
                fh.write('v %.9g %.9g %.9g\n'
                         % (tx * WU_TO_M, ty * WU_TO_M, tz * WU_TO_M))
            for f in m['faces']:
                order = list(reversed(f)) if TAG_Y_SIGN > 0 else list(f)
                fh.write('f %s\n' % ' '.join(str(i + 1 + base) for i in order))
            base += len(m['verts'])
    print('ok', name, sum(len(m['verts']) for m in meshes), 'verts')
