# Background job on RigVR_v2.blend: chest/back areas from the body surface, collision probe, pictures.
import bpy, bmesh, json, math, os, sys
from mathutils import Vector, Matrix
from mathutils.bvhtree import BVHTree
OUT = sys.argv[-1]
D = os.path.dirname(bpy.data.filepath)
O = bpy.data.objects
log = {}
for c in bpy.context.scene.collection.children:
    if not c.name.startswith('V2 '):
        c.hide_render = True
        c.hide_viewport = True
body = next(o for o in bpy.data.collections['V2 Body reference'].objects if o.type == 'MESH' and 'Body' in o.name)
rig = O['V2 Rig Base']
areas_c = bpy.data.collections['V2 Areas']
deps = bpy.context.evaluated_depsgraph_get()
def bvh_of(ob):
    bm = bmesh.new(); bm.from_object(ob, deps); bm.transform(ob.matrix_world)
    t = BVHTree.FromBMesh(bm); bm.free(); return t
bvh_body, bvh_rig = bvh_of(body), bvh_of(rig)

def strip(name, z0, z1, a0, a1, bone, use_rig, offset, step_a=7.5, step_z=2.5):
    old = O.get(name)
    if old: bpy.data.objects.remove(old, do_unlink=True)
    zs = [z0 + i * step_z for i in range(int(round((z1 - z0) / step_z)) + 1)]
    angs = [a0 + i * step_a for i in range(int(round((a1 - a0) / step_a)) + 1)]
    rows = []
    for z in zs:
        row = []
        for a in angs:
            d = Vector((math.sin(math.radians(a)), math.cos(math.radians(a)), 0.0))
            start = Vector((0, 0, z)) + d * 60.0
            best = None
            for t in ((bvh_rig, bvh_body) if use_rig else (bvh_body,)):
                hit = t.ray_cast(start, -d, 70.0)
                if hit[0] is not None and (best is None or hit[3] < best[3]):
                    best = hit
            row.append(best[0] + d * offset if best else None)
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
    ob['rigvr_kinds'] = ''
    ob.display_type = 'WIRE'; ob.show_in_front = True
    ob.parent = rig
    ob.color = (1.0, 0.8, 0.1, 1)
    return {'verts': len(verts), 'faces': len(faces)}
log['areas'] = {
    'belt': strip('Area Belt', 68.0, 79.0, -180.0, 180.0, 'Pelvis', True, 0.6),
    'chest': strip('Area Chest', 84.0, 104.0, -45.0, 45.0, 'SPINE2', False, 3.0),
    'back': strip('Area Back', 84.0, 110.0, 130.0, 230.0, 'SPINE2', False, 3.5),
}

# how PyNifly shows a vanilla clutter item's collision in Blender
before = set(O)
try:
    bpy.ops.import_scene.pynifly(filepath=os.path.join(D, 'vanilla', 'WonderGlueLarge.nif'))
except Exception as e:
    log['probe_err'] = str(e)
probe = []
for o in set(O) - before:
    props = {k: (str(o[k])[:120]) for k in o.keys()}
    grp = {}
    for gname in ('pyn_fo4phys', 'pyn_collisionobj'):
        g = getattr(o, gname, None)
        if g is not None:
            try:
                grp[gname] = {p.identifier: str(getattr(g, p.identifier))[:80] for p in g.bl_rna.properties if p.identifier != 'rna_type'}
            except Exception as e:
                grp[gname] = str(e)
    rb = None
    if o.rigid_body:
        rb = {'type': o.rigid_body.type, 'mass': o.rigid_body.mass, 'shape': o.rigid_body.collision_shape}
    cons = [(c.type, c.target.name if getattr(c, 'target', None) else None) for c in o.constraints]
    probe.append({'name': o.name, 'type': o.type, 'parent': o.parent.name if o.parent else None, 'props': props, 'grp': grp, 'rb': rb, 'cons': cons,
                  'verts': len(o.data.vertices) if o.type == 'MESH' else None})
log['probe'] = probe
for o in set(O) - before:
    bpy.data.objects.remove(o, do_unlink=True)

# pictures
sc = bpy.data.scenes['Scene']
cam = O['V2 Cam']
O['V2 Mannequin'].hide_render = True
for o in areas_c.objects:
    o.hide_render = False
shots = []
for label, ang in (('front', 0), ('side', 90), ('back', 180)):
    a = math.radians(ang)
    cam.location = (math.sin(a) * 200, math.cos(a) * 200, 85)
    cam.rotation_euler = (math.radians(90), 0, math.radians(180 - ang))
    path = os.path.join(D, 'work', 'areas_%s.png' % label)
    sc.render.filepath = path
    bpy.ops.render.render(write_still=True)
    shots.append(path)
log['shots'] = shots
bpy.ops.wm.save_mainfile()
json.dump(log, open(OUT, 'w'))
