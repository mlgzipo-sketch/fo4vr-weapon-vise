# Background job on RigVR_v2.blend: brings in the three magazine pouches (pistol, rifle, universal) the user made
# in their scene (work/new_pouches.blend, world space of the user's mannequin) as 'V2 Pouch<Kind>' empties in the
# FO4 body space, the same way job_fit.py did the others (moved by the hang point only, not stretched).
import bpy, json, math, os, sys
from mathutils import Vector, Matrix
OUT = sys.argv[-1]
D = os.path.dirname(bpy.data.filepath)
O = bpy.data.objects
log = {}
SX, SY, DY, K = 1.47, 1.54, -0.3, 1.29
def fit_point(p):
    z = p.z if p.z < 72 else 72 + (p.z - 72) * K
    return Vector((p.x * SX, p.y * SY + DY, z))
KINDS = {'Подсумок Магазин для пистолетов': 'PouchMagPistol', 'Подсумок магазин винтовок': 'PouchMagRifle',
         'Подсумок универсальный': 'PouchMagUniversal'}
src = os.path.join(D, 'work', 'new_pouches.blend')
with bpy.data.libraries.load(src, link=False) as (fr, to):
    to.collections = [c for c in fr.collections if c in KINDS]
pouches = bpy.data.collections['V2 Pouches']
WANT = {'pouch_002': 'pouch_002.002', 'cover': 'cover.002', 'strap_013': 'strap_013.002'}
for col in to.collections:
    kind = KINDS[col.name]
    old = O.get('V2 ' + kind)
    if old:
        for c in list(old.children): bpy.data.objects.remove(c, do_unlink=True)
        bpy.data.objects.remove(old, do_unlink=True)
    meshes = [o for o in col.objects if o.type == 'MESH']
    # no parents: the stored basis is the world matrix (matrix_world of an appended, unlinked object is not evaluated)
    mw0 = meshes[0].matrix_basis
    yaw = math.atan2(mw0.col[0].y, mw0.col[0].x)           # the pouch's own turn about Z (the user tilted some)
    rot = Matrix.Rotation(yaw, 4, 'Z')
    pts = [rot.inverted() @ (o.matrix_basis @ Vector(v)) for o in meshes for v in o.bound_box]
    centre = rot @ Vector([(min(p[i] for p in pts) + max(p[i] for p in pts)) / 2 for i in range(3)])
    ne = O.new('V2 ' + kind, None); ne.empty_display_type = 'ARROWS'; ne.empty_display_size = 3
    pouches.objects.link(ne)
    ne.matrix_world = Matrix.Translation(fit_point(centre)) @ rot
    for o in meshes:
        for slot in o.material_slots:
            m = slot.material
            if m and m.name not in WANT.values():
                for k, v in WANT.items():
                    if m.name.startswith(k):
                        if v in bpy.data.materials and bpy.data.materials[v] is not m:
                            slot.material = bpy.data.materials[v]
                        else:
                            m.name = v
        nc = o.copy(); pouches.objects.link(nc)
        nc.parent = ne; nc.matrix_parent_inverse = Matrix.Identity(4)
        nc.matrix_basis = (Matrix.Translation(centre) @ rot).inverted() @ o.matrix_basis
        nc.hide_render = True
    ne.hide_render = True
    log[kind] = {'centre_user': list(centre), 'centre_body': list(fit_point(centre)), 'parts': [o.name for o in meshes], 'yaw': math.degrees(yaw),
                 'mats': sorted({s.material.name for o in meshes for s in o.material_slots if s.material})}
    bpy.data.collections.remove(col)
for o in list(O):
    if o.users == 0 or (not o.users_collection and o.parent is None and o.name not in [x.name for x in pouches.objects]):
        pass
bpy.ops.wm.save_as_mainfile(filepath=bpy.data.filepath)
json.dump(log, open(OUT, 'w'), ensure_ascii=False)
