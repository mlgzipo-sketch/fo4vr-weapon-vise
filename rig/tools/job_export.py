# Background job on RigVR_v2.blend: exports everything the v2 plugin needs.
#   Data\Meshes\RigVR\Custom\Mannequin.nif           mannequin, body space (feet at origin, facing +Y)
#   Data\Meshes\RigVR\Custom\RigBase_Lower/Upper.nif   the rig, body space, split at the waist (Pelvis / SPINE2 on the body)
#   Data\Meshes\RigVR\Custom\Pouch<Kind>.nif          a pouch, origin on its back face middle, +Y outwards, +Z up
#   Data\Meshes\RigVR\Custom\Item<Kind>.nif           the same as a loose item: centred, with a dynamic collision box
#   Data\F4SE\Plugins\RigVR\Rigs\RigBase.txt          binds, models and the areas (triangles) the plugin snaps pouches to
import bpy, bmesh, json, math, os, sys, traceback
from mathutils import Vector, Matrix
OUT = sys.argv[-1]
D = os.path.dirname(bpy.data.filepath)
GAME = os.path.dirname(D)
DATA = os.path.join(GAME, 'Data')
CUSTOM = os.path.join(DATA, 'Meshes', 'RigVR', 'Custom')
RIGS = os.path.join(DATA, 'F4SE', 'Plugins', 'RigVR', 'Rigs')
os.makedirs(CUSTOM, exist_ok=True); os.makedirs(RIGS, exist_ok=True)
O = bpy.data.objects
log = {'files': {}, 'warn': []}
ns = {'__name__': 'rigvr_scene_lib'}
src = open(os.path.join(D, 'RigVR_scene.py'), encoding='utf-8').read()
exec(compile(src.split('\nclass RIGVR_OT_export')[0], 'RigVR_scene.py', 'exec'), ns)
game_material, export_parts = ns['game_material'], ns['export_parts']
ctx = bpy.context
deps = ctx.evaluated_depsgraph_get()

def select_only(objs):
    for o in ctx.view_layer.objects:
        o.select_set(False)
    for o in objs:
        o.select_set(True)
    ctx.view_layer.objects.active = objs[0]

def export(objs, path):
    select_only(objs)
    bpy.ops.export_scene.pynifly(filepath=path, target_game='FO4', intuit_defaults=False)
    log['files'][os.path.basename(path)] = os.path.getsize(path)

def cleanup(objs):
    for c in objs:
        me = c.data if c.type == 'MESH' else None
        bpy.data.objects.remove(c, do_unlink=True)
        if me is not None and me.users == 0:
            bpy.data.meshes.remove(me)

def tris(objs):
    return sum(len(f.vertices) - 2 for c in objs if c.type == 'MESH' for f in c.data.polygons)

# ---- binds straight from the game's skeleton.nif (the nodes the plugin attaches to)
BONES = ('Pelvis', 'SPINE1', 'SPINE2', 'Chest')
bind = {}
try:
    import importlib
    pyn = None
    for name in ('io_scene_nifly.pyn.pynifly', 'io_scene_nifly.pynifly'):
        try:
            pyn = importlib.import_module(name); break
        except Exception:
            pass
    nif = pyn.NifFile(os.path.join(D, 'vanilla', 'skeleton.nif'))
    for b in BONES:
        n = nif.nodes[b]
        xf = n.global_transform
        t = xf.translation; r = xf.rotation; s = xf.scale
        rows = [list(r[i]) if hasattr(r, '__getitem__') else None for i in range(3)]
        bind[b] = {'t': [float(t[0]), float(t[1]), float(t[2])], 'r': [[float(x) for x in row] for row in rows], 's': float(s)}
    log['bind_src'] = 'nif'
except Exception as e:
    log['warn'].append('bind from nif failed: %s' % e)
    arm = O['V2 Skeleton']
    for b in BONES:
        m = arm.matrix_world @ arm.data.bones[b].matrix_local
        bind[b] = {'t': list(m.translation), 'r': [list(m.col[i][:3]) for i in range(3)], 's': 1.0}   # rows = axes
    log['bind_src'] = 'armature'
log['bind'] = bind
try:
    arm = O['V2 Skeleton']
    log['bind_arm'] = {b: {'t': [round(x, 3) for x in (arm.matrix_world @ arm.data.bones[b].matrix_local).translation],
                           'cols': [[round(x, 3) for x in (arm.matrix_world @ arm.data.bones[b].matrix_local).col[i][:3]] for i in range(3)]} for b in BONES}
    log['bind_raw'] = {b: str(nif.nodes[b].global_transform)[:300] for b in BONES}
except Exception as e:
    log['warn'].append('bind compare: %s' % e)

# ---- mannequin and rig (body space)
man = O['V2 Mannequin']
cp = export_parts(ctx, [man], Matrix.Identity(4), DATA); export(cp, os.path.join(CUSTOM, 'Mannequin.nif')); log['mannequin_tris'] = tris(cp); cleanup(cp)

rig = O['V2 Rig Base']
cp = export_parts(ctx, [rig], Matrix.Identity(4), DATA)    # decimated by its rigvr_decimate
WAIST = 80.0
lower, upper = [], []
for c in cp:
    bm = bmesh.new(); bm.from_mesh(c.data)
    up = bm.copy()
    bmesh.ops.delete(bm, geom=[f for f in bm.faces if f.calc_center_median().z >= WAIST], context='FACES')
    bmesh.ops.delete(up, geom=[f for f in up.faces if f.calc_center_median().z < WAIST], context='FACES')
    for b, lst, tag in ((bm, lower, 'L'), (up, upper, 'U')):
        bmesh.ops.delete(b, geom=[v for v in b.verts if not v.link_faces], context='VERTS')
        if b.faces:
            me = bpy.data.meshes.new(c.name + tag); b.to_mesh(me)
            for m in c.data.materials: me.materials.append(m)
            o = O.new(c.name + tag, me); ctx.scene.collection.objects.link(o); lst.append(o)
        b.free()
cleanup(cp)
export(lower, os.path.join(CUSTOM, 'RigBase_Lower.nif')); log['rig_lower_tris'] = tris(lower); cleanup(lower)
export(upper, os.path.join(CUSTOM, 'RigBase_Upper.nif')); log['rig_upper_tris'] = tris(upper); cleanup(upper)

# ---- pouches: visual (origin on the back face) and loose item (centred, with collision)
TEMPLATE = os.path.join(D, 'vanilla', 'WonderGlueLarge.nif')
MASS = {'PouchMagPistol': 0.6, 'PouchMagRifle': 0.8, 'PouchMagUniversal': 0.8, 'PouchMag': 1.0, 'PouchDump': 2.0, 'PouchHolster': 1.0, 'PouchSling': 1.5, 'RigBase': 4.0}
def item_model(kind, parts, centre_mx):
    """parts: export copies (already game materials) with world = their final local; centre_mx: where the item's middle is."""
    before = set(O)
    bpy.ops.import_scene.pynifly(filepath=TEMPLATE)
    new = [o for o in set(O) - before]
    root = next(o for o in new if o.get('pynRoot'))
    coll = next(o for o in new if o.get('pynRigidBody') == 'bhkPhysicsSystem')
    for o in new:
        if o.type == 'MESH' and o is not coll:
            cleanup([o])
    new = [o for o in set(O) - before]
    root.name = 'Item' + kind
    root['pynNodeName'] = 'Item' + kind
    # shapes under the root, centred
    inv = centre_mx.inverted()
    lo = Vector((1e9,) * 3); hi = Vector((-1e9,) * 3)
    for p in parts:
        p.matrix_world = inv @ p.matrix_world
        p.data.transform(p.matrix_world); p.matrix_world = Matrix.Identity(4)
        p.parent = root
        for v in p.data.vertices:
            lo = Vector(map(min, lo, v.co)); hi = Vector(map(max, hi, v.co))
    # collision: a box round the shapes, as a convex polytope
    corners = [Vector((x, y, z)) for x in (lo.x, hi.x) for y in (lo.y, hi.y) for z in (lo.z, hi.z)]
    bm = bmesh.new()
    for c in corners: bm.verts.new(c)
    bmesh.ops.convex_hull(bm, input=bm.verts)
    bm.to_mesh(coll.data); bm.free()
    coll.matrix_world = root.matrix_world.copy()
    if coll.rigid_body:
        coll.rigid_body.mass = MASS.get(kind, 1.0)
    export([root, coll] + [o for o in new if o not in (root, coll)] + parts, os.path.join(CUSTOM, 'Item%s.nif' % kind))
    log['item_' + kind] = {'size': [round(x, 1) for x in (hi - lo)], 'tris': tris(parts)}
    cleanup(parts + [o for o in set(O) - before if o.name in O])

HANG = {}
for kind in ('PouchMagPistol', 'PouchMagRifle', 'PouchMagUniversal', 'PouchDump', 'PouchHolster', 'PouchSling'):
    try:
        e = O['V2 ' + kind]
        meshes = [c for c in e.children_recursive if c.type == 'MESH']
        # back face middle in the empty's frame
        inv = e.matrix_world.inverted()
        pts = [inv @ (c.matrix_world @ v.co) for c in meshes for v in c.data.vertices]
        lo = Vector((min(p.x for p in pts), min(p.y for p in pts), min(p.z for p in pts)))
        hi = Vector((max(p.x for p in pts), max(p.y for p in pts), max(p.z for p in pts)))
        hang = e.matrix_world @ Matrix.Translation(((lo.x + hi.x) / 2, lo.y, (lo.z + hi.z) / 2))
        HANG[kind] = hang.copy()
        cp = export_parts(ctx, meshes, hang.inverted(), DATA)
        export(cp, os.path.join(CUSTOM, kind + '.nif')); cleanup(cp)
        log[kind] = {'size': [round(x, 1) for x in (hi - lo)]}
        middle = e.matrix_world @ Matrix.Translation((lo + hi) / 2)
        cp = export_parts(ctx, meshes, middle.inverted(), DATA)
        item_model(kind, cp, Matrix.Identity(4))
    except Exception:
        log['warn'].append(kind + ': ' + traceback.format_exc()[-800:])

try:   # the rig as a loose item
    for o in (rig,):
        o['rigvr_decimate'] = 0.12
    cp = export_parts(ctx, [rig], Matrix.Identity(4), DATA)
    rig['rigvr_decimate'] = 0.3
    pts = [c.matrix_world @ v.co for c in cp for v in c.data.vertices]
    mid = Vector([(min(p[i] for p in pts) + max(p[i] for p in pts)) / 2 for i in range(3)])
    item_model('RigBase', cp, Matrix.Translation(mid))
except Exception:
    log['warn'].append('rig item: ' + traceback.format_exc()[-800:])

# ---- the rig file for the plugin
lines = ['# RigVR rig RigBase (written by job_export.py from RigVR_v2.blend). Body space: feet at 0, facing +Y, game units.',
         '# bind <bone> rows of the rotation (local axes in body space) then the position',
         '# model <file under Meshes> <bone>   area <name> <bone> <kinds or *>   tri x y z x y z x y z']
for b, x in bind.items():
    r = x['r']
    # pynifly gives the math matrix (columns = the bone's axes); the game's rows are those axes
    axes = [[r[0][i], r[1][i], r[2][i]] for i in range(3)] if log.get('bind_src') == 'nif' else r
    lines.append('bind %s %s %s' % (b, ' '.join('%.6f' % v for row in axes for v in row), ' '.join('%.4f' % v for v in x['t'])))
lines.append(r'model RigVR\Custom\RigBase_Lower.nif Pelvis')
lines.append(r'model RigVR\Custom\RigBase_Upper.nif SPINE2')
segs = {}
for a in bpy.data.collections['V2 Areas'].objects:
    if a.type != 'MESH' or 'rigvr_area' not in a:
        continue
    kinds = a.get('rigvr_kinds') or '*'
    lines.append('area %s %s %s' % (a['rigvr_area'], a.get('rigvr_bone', 'Pelvis'), kinds.replace(' ', '')))
    me = a.data; mw = a.matrix_world
    me.calc_loop_triangles()
    for t in me.loop_triangles:
        p = [mw @ me.vertices[i].co for i in t.vertices]
        lines.append('tri ' + ' '.join('%.3f %.3f %.3f' % (q.x, q.y, q.z) for q in p))
    segs[a['rigvr_area']] = [[list(mw @ me.vertices[e.vertices[0]].co), list(mw @ me.vertices[e.vertices[1]].co)] for e in me.edges]
# the starting layout: every pouch where it hangs in the scene, on its nearest area (rows = axes, body space)
from mathutils.bvhtree import BVHTree
trees = {}
for a in bpy.data.collections['V2 Areas'].objects:
    if a.type == 'MESH' and 'rigvr_area' in a:
        me = a.data; mw = a.matrix_world; me.calc_loop_triangles()
        trees[a['rigvr_area']] = BVHTree.FromPolygons([mw @ v.co for v in me.vertices], [t.vertices[:] for t in me.loop_triangles])
lines.append('# preset <kind> <area> rows of the rotation then the position: the layout a new game starts with')
log['preset'] = {}
for kind, m in HANG.items():
    p = m.translation
    best = min(((t.find_nearest(p)[3] or 1e9), n) for n, t in trees.items())
    rows = [[m.col[i][j] for j in range(3)] for i in range(3)]   # Blender column i = axis i = game row i
    lines.append('preset %s %s %s %s' % (kind[5:], best[1], ' '.join('%.5f' % v for r in rows for v in r), ' '.join('%.3f' % v for v in p)))
    log['preset'][kind] = [best[1], round(best[0], 2), [round(v, 2) for v in p]]
for n in segs:
    lines.append('highlight %s RigVR\\Custom\\Area_RigBase_%s.nif' % (n, n))
open(os.path.join(RIGS, 'RigBase.txt'), 'w').write('\n'.join(lines) + '\n')
log['rigfile_lines'] = len(lines)
json.dump(segs, open(os.path.join(D, 'work', 'area_segments.json'), 'w'))
json.dump(log, open(OUT, 'w'))
