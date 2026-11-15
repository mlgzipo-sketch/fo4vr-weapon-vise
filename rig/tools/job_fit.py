# Background job: build RigVR_v2.blend (FO4 body space) from RigVR_models.blend.
import bpy, bmesh, json, math, os, sys
from mathutils import Vector, Matrix
from mathutils.bvhtree import BVHTree

OUT = sys.argv[-1]
D = os.path.dirname(bpy.data.filepath)
V = os.path.join(D, 'vanilla')
O = bpy.data.objects
log = {}

def coll(name, parent=None):
    c = bpy.data.collections.get(name) or bpy.data.collections.new(name)
    p = parent or bpy.context.scene.collection
    if c.name not in [x.name for x in p.children]:
        p.children.link(c)
    return c

for c in list(bpy.context.scene.collection.children):
    c.hide_viewport = True   # the old layout collections stay, hidden

ref = coll('V2 Body reference')
fit = coll('V2 Rig')
areas_c = coll('V2 Areas')

# FO4 body + skeleton
before = set(O)
bpy.ops.import_scene.pynifly(filepath=os.path.join(V, 'MaleBody.nif'))
bpy.ops.import_scene.pynifly(filepath=os.path.join(V, 'skeleton.nif'))
for o in set(O) - before:
    for c in list(o.users_collection):
        c.objects.unlink(o)
    ref.objects.link(o)
    o.hide_render = True
arm = next(o for o in set(O) - before if o.type == 'ARMATURE' and 'skeleton' in o.name.lower())
arm.name = 'V2 Skeleton'
bind = {}
for b in arm.data.bones:
    if b.name in ('Pelvis', 'SPINE1', 'SPINE2', 'Chest', 'LLeg_Thigh', 'RLeg_Thigh'):
        m = arm.matrix_world @ b.matrix_local
        bind[b.name] = [list(r) for r in m]
log['bind'] = bind

# the user's mannequin / rig, moved into the game body's proportions (waist 72 stays, wider, deeper, taller above)
SX, SY, DY, K = 1.47, 1.54, -0.3, 1.29
def fit_point(p):
    z = p.z if p.z < 72 else 72 + (p.z - 72) * K
    return Vector((p.x * SX, p.y * SY + DY, z))
deps = bpy.context.evaluated_depsgraph_get()
def fitted(src, name):
    me = bpy.data.meshes.new_from_object(src.evaluated_get(deps))
    mw = src.matrix_world
    for v in me.vertices:
        v.co = fit_point(mw @ v.co)
    me.update()
    ob = O.new(name, me)
    fit.objects.link(ob)
    return ob
man = fitted(O['Mannequin.001'], 'V2 Mannequin')
rig = fitted(O['buckle_001_mesh.011'], 'V2 Rig Base')
rig['rigvr_decimate'] = 0.3

# areas: strips that hug the rig's outside, found by casting rays at the rig from outside
bm = bmesh.new(); bm.from_mesh(rig.data)
bvh = BVHTree.FromBMesh(bm)
def strip(name, z0, z1, a0, a1, bone, step_a=8.0, step_z=2.5):
    rows = []
    zs = [z0 + i * step_z for i in range(int((z1 - z0) / step_z) + 1)]
    angs = [a0 + i * step_a for i in range(int((a1 - a0) / step_a) + 1)]
    for z in zs:
        row = []
        for a in angs:
            d = Vector((math.sin(math.radians(a)), math.cos(math.radians(a)), 0.0))   # 0 deg = front (+Y), 90 = right (+X)
            start = Vector((0, 0, z)) + d * 60.0
            hit = bvh.ray_cast(start, -d, 70.0)
            row.append(hit[0] + d * 0.6 if hit[0] is not None else None)
        rows.append(row)
    verts, faces, index = [], [], {}
    for i, row in enumerate(rows):
        for j, p in enumerate(row):
            if p is not None:
                index[(i, j)] = len(verts); verts.append(p)
    for i in range(len(rows) - 1):
        for j in range(len(angs) - 1):
            q = [(i, j), (i, j + 1), (i + 1, j + 1), (i + 1, j)]
            if all(k in index for k in q):
                faces.append([index[k] for k in q])
    me = bpy.data.meshes.new(name); me.from_pydata(verts, [], faces); me.update()
    ob = O.new(name, me); areas_c.objects.link(ob)
    ob['rigvr_area'] = name.replace('Area ', '')
    ob['rigvr_bone'] = bone
    ob['rigvr_kinds'] = ''          # empty = every pouch kind may hang here
    ob.display_type = 'WIRE'; ob.show_in_front = True; ob.hide_render = True
    ob.parent = rig
    return {'verts': len(verts), 'faces': len(faces)}
log['areas'] = {
    'belt': strip('Area Belt', 68.0, 79.0, -180.0, 180.0, 'Pelvis'),
    'chest': strip('Area Chest', 84.0, 104.0, -50.0, 50.0, 'SPINE2'),
    'back': strip('Area Back', 82.0, 108.0, 125.0, 235.0, 'SPINE2'),
}
bm.free()

# pouch references: the user's models moved to the same body space by their hang point only (not stretched)
pouches = coll('V2 Pouches')
for name in ('PouchMag', 'PouchDump', 'PouchHolster', 'PouchSling'):
    e = O[name]
    ne = O.new('V2 ' + name, None); ne.empty_display_type = 'ARROWS'; ne.empty_display_size = 3
    pouches.objects.link(ne)
    t = e.matrix_world.translation
    ne.matrix_world = Matrix.Translation(fit_point(t)) @ e.matrix_world.to_3x3().to_4x4()
    for c in e.children:
        if c.type == 'MESH' and '(hologram size)' not in c.name:
            nc = c.copy(); pouches.objects.link(nc)
            nc.parent = ne
            nc.matrix_parent_inverse = Matrix.Identity(4)
            nc.matrix_basis = e.matrix_world.inverted() @ c.matrix_world
    ne.hide_render = True

# pictures to check the fit: front and back, workbench
sc = bpy.context.scene
sc.render.engine = 'BLENDER_WORKBENCH'
sc.display.shading.color_type = 'OBJECT'
sc.render.resolution_x, sc.render.resolution_y = 900, 1100
for o in ref.objects:
    if o.type == 'MESH' and 'Body' in o.name:
        o.hide_render = False; o.color = (0.75, 0.62, 0.5, 1)
man.color = (0.5, 0.5, 0.55, 1); rig.color = (0.2, 0.35, 0.2, 1)
man.hide_render = True
for o in areas_c.objects:
    o.hide_render = True
cam_data = bpy.data.cameras.new('V2 Cam'); cam_data.type = 'ORTHO'; cam_data.ortho_scale = 75
cam = O.new('V2 Cam', cam_data); sc.collection.objects.link(cam); sc.camera = cam
shots = []
for label, ang in (('front', 0), ('side', 90), ('back', 180)):
    a = math.radians(ang)
    cam.location = (math.sin(a) * 200, math.cos(a) * 200, 85)
    cam.rotation_euler = (math.radians(90), 0, math.radians(180 - ang))
    path = os.path.join(D, 'work', 'fit_%s.png' % label)
    sc.render.filepath = path
    bpy.ops.render.render(write_still=True)
    shots.append(path)
log['shots'] = shots
bpy.ops.wm.save_as_mainfile(filepath=os.path.join(D, 'RigVR_v2.blend'))
json.dump(log, open(OUT, 'w'))
