# RigVR: a Blender scene for making the rig's models (Blender 3.6+ / 4.x, PyNifly for the export).
#
# How to use: Blender -> Scripting tab -> Open this file -> Run Script (once, in an empty scene).
#
# What it builds (1 Blender unit = 1 game unit, about 1.43 cm, as PyNifly imports and exports by default):
#   collection "RigVR Reference"  the mannequin as the plugin sees it (feet at 0, belt at 66, chest panel at 92),
#                                 the rig's places as small empties, and the current hologram sizes as wire boxes.
#                                 Nothing here is exported.
#   collection "RigVR Models"     one empty per model, named like the file it becomes (PouchMag, PouchMed...),
#                                 standing on its place on the mannequin. Model each pouch as a CHILD of its
#                                 empty (parent it with Ctrl+P -> Object), around the empty's origin:
#                                   +Y = the pouch's front (away from the body), +Z = up, origin = where it hangs.
#                                 The mannequin and the rig are modelled the same way around their empty at the feet.
#   "RigVR: export" in the sidebar (N -> RigVR): writes every model to <folder>\Meshes\RigVR\Custom\<name>.nif with
#                                 its empty at the origin (the scene itself is not changed). Materials listed in
#                                 TEXTURES get the game's DDS textures; an object with a custom property
#                                 'rigvr_decimate' (e.g. 0.3) is exported with that many of its polygons.
#
# The plugin uses a model in Meshes\RigVR\Custom\ in place of the hologram with the same name (from the next game start).
# A different size or turn later can also be fixed in RigVR.ini (fScale_Mag, fRot_Mag, fOffset_Mag...).

import math
import os

import bmesh
import bpy
from mathutils import Matrix

BELT = 66.0
CHEST = 92.0

# The plugin's default places (RigVR.ini Slot0..12): group (0 belt, 1 chest, -1 off), right, forward, up, turn (deg).
# 0..3 come from the user's own layout (2026-11-15): magazine pouch front belt, bag left hip, pistol holster right hip,
# carbine over the right shoulder.
PLACES = [
    (0, 0.0, 8.1, 6.05, 0.0), (0, -13.9, 1.1, 7.3, -90.0), (0, 11.5, 3.25, 6.25, 90.0), (1, 6.55, -8.4, 7.65, 180.0),
    (1, -6.55, -8.4, 7.65, 180.0), (1, 0.0, 10.5, 0.0, 0.0), (0, 0.0, -13.9, 7.3, 180.0),
] + [(-1, 0.0, 0.0, 0.0, 0.0)] * 6

# File name, hologram half size (x, y, z), and the place it is modelled on.
MODELS = [
    ('PouchMag', (3.0, 2.0, 4.5), 0),
    ('PouchMed', (4.0, 2.5, 4.0), 5),
    ('PouchGrenade', (3.0, 2.5, 3.5), 5),
    ('PouchHolster', (3.0, 2.0, 7.5), 2),
    ('PouchDump', (5.0, 3.5, 5.0), 1),
    ('PouchBandolier', (7.0, 1.5, 2.0), 5),
    ('PouchSling', (2.5, 2.0, 14.0), 3),
]

# Game textures (Data\Textures\RigVR\<set>_d/_n/_s.dds, made from the Blender images) for each source material.
# A material not listed here is exported as it is.
TEXTURES = {
    'main.006': ('BagMain', 'BagMain', 'BagMain'), 'cap': ('BagCap', 'BagCap', 'BagCap'),
    'strap_081.001': ('BagStrap', 'BagStrap', 'BagStrap'), 'buckle_037': ('BagBuckle', 'BagBuckle', 'BagBuckle'),
    'pad_004': ('BagPad', 'BagPad', 'BagPad'), 'Material.001': ('Holster', 'Holster', 'Holster'),
    'armor': ('Armor', 'Flat', 'Matte'), 'Mannequin': ('Mannequin', 'Flat', 'Matte'),
    'Mannequin 2': ('MannequinMetal', 'Flat', 'Shiny'),
    'pouch_002.002': ('MagPouch', 'MagPouch', 'MagPouch'), 'cover.002': ('MagCover', 'MagCover', 'MagCover'),
    'strap_013.002': ('MagStrap', 'MagStrap', 'MagStrap'),
}


def place_matrix(index):
    group, x, y, z, turn = PLACES[index]
    base = CHEST if group == 1 else BELT
    # the game's turn is clockwise seen from above (to the right); Blender's Z rotation is counter-clockwise
    return Matrix.Translation((x, y, base + z)) @ Matrix.Rotation(math.radians(-turn), 4, 'Z')


def collection(name):
    col = bpy.data.collections.get(name) or bpy.data.collections.new(name)
    if col.name not in bpy.context.scene.collection.children:
        bpy.context.scene.collection.children.link(col)
    return col


def wire_box(name, half, matrix, col):
    bpy.ops.mesh.primitive_cube_add(size=2.0)
    ob = bpy.context.active_object
    ob.name = name
    ob.scale = half
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    ob.matrix_world = matrix @ ob.matrix_world
    ob.display_type = 'WIRE'
    ob.hide_render = True
    for c in list(ob.users_collection):
        c.objects.unlink(ob)
    col.objects.link(ob)
    return ob


def empty(name, matrix, col, kind='ARROWS', size=4.0):
    ob = bpy.data.objects.new(name, None)
    ob.empty_display_type = kind
    ob.empty_display_size = size
    ob.matrix_world = matrix
    col.objects.link(ob)
    return ob


def build():
    ref = collection('RigVR Reference')
    models = collection('RigVR Models')
    # the mannequin as boxes (the plugin's sizes)
    for name, centre, half in [('Ref hips', (0, 0, 66), (12, 7, 5)), ('Ref torso', (0, 0, 88), (13, 7, 15)),
                               ('Ref head', (0, 0.5, 116), (5, 6, 7)), ('Ref pole', (0, 0, 30), (1, 1, 30)),
                               ('Ref stand', (0, 0, 0.5), (13, 13, 0.5))]:
        wire_box(name, half, Matrix.Translation(centre), ref)
    empty('Ref belt anchor (66)', Matrix.Translation((0, 0, BELT)), ref, 'PLAIN_AXES', 6)
    empty('Ref chest anchor (92)', Matrix.Translation((0, 0, CHEST)), ref, 'PLAIN_AXES', 6)
    empty('Ref shoulder L (grab to put on)', Matrix.Translation((-13, 0, CHEST + 10)), ref, 'SPHERE', 2)
    empty('Ref shoulder R (grab to put on)', Matrix.Translation((13, 0, CHEST + 10)), ref, 'SPHERE', 2)
    for i in range(len(PLACES)):
        if PLACES[i][0] < 0:
            continue
        empty('Ref place %d' % i, place_matrix(i), ref, 'SINGLE_ARROW', 3)
    # the models to make
    for name, half, at in MODELS:
        e = empty(name, place_matrix(at), models, 'ARROWS', 3)
        box = wire_box(name + ' (hologram size)', half, place_matrix(at), ref)
        box.parent = e
        box.matrix_parent_inverse = e.matrix_world.inverted()
    empty('Mannequin', Matrix.Identity(4), models, 'ARROWS', 8)
    empty('Rig', Matrix.Identity(4), models, 'ARROWS', 6)
    bpy.context.scene.unit_settings.system = 'NONE'
    print('RigVR scene built')


def game_material(name, data):
    """A plain material whose image paths PyNifly turns into textures\\RigVR\\<set>_d/_n/_s.dds."""
    d, n, sp = TEXTURES[name]
    mat = bpy.data.materials.get('RigVR ' + name) or bpy.data.materials.new('RigVR ' + name)
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()
    out = nt.nodes.new('ShaderNodeOutputMaterial')
    bsdf = nt.nodes.new('ShaderNodeBsdfPrincipled')
    nt.links.new(bsdf.outputs[0], out.inputs['Surface'])

    def image(suffix, setname):
        path = os.path.join(data, 'Textures', 'RigVR', setname + suffix + '.dds')
        node = nt.nodes.new('ShaderNodeTexImage')
        try:
            node.image = bpy.data.images.load(path, check_existing=True)
        except RuntimeError:   # the file is not there yet: the path is all PyNifly needs
            img = bpy.data.images.new(setname + suffix, 4, 4)
            img.filepath = path
            node.image = img
        return node
    nt.links.new(image('_d', d).outputs['Color'], bsdf.inputs['Base Color'])
    nm = nt.nodes.new('ShaderNodeNormalMap')
    nt.links.new(image('_n', n).outputs['Color'], nm.inputs['Color'])
    nt.links.new(nm.outputs['Normal'], bsdf.inputs['Normal'])
    spec = image('_s', sp)
    spec.name = 'Specular_Texture'
    return mat


def export_parts(context, parts, to_origin, data):
    """Copies of the parts: modifiers applied (plus a Decimate if the object has a 'rigvr_decimate' ratio),
    one object per material, game materials, moved so the model's empty is at the origin."""
    deps = context.evaluated_depsgraph_get()
    copies = []
    for p in parts:
        ratio = p.get('rigvr_decimate')
        mod = None
        if ratio:
            mod = p.modifiers.new('RigVR decimate', 'DECIMATE')
            mod.ratio = float(ratio)
            deps.update()
        me = bpy.data.meshes.new_from_object(p.evaluated_get(deps))
        if mod:
            p.modifiers.remove(mod)
        mats = list(me.materials) or [None]
        for index, m in enumerate(mats):
            bm = bmesh.new()
            bm.from_mesh(me)
            if len(mats) > 1:
                bmesh.ops.delete(bm, geom=[f for f in bm.faces if f.material_index != index], context='FACES')
                bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context='VERTS')
            if not bm.faces:
                bm.free()
                continue
            part = bpy.data.meshes.new(p.name + '_' + str(index))
            bm.to_mesh(part)
            bm.free()
            part.materials.clear()
            if m is not None:
                part.materials.append(game_material(m.name, data) if m.name in TEXTURES else m)
            for poly in part.polygons:
                poly.material_index = 0
            if not part.uv_layers:          # PyNifly cannot write a mesh without UVs
                part.uv_layers.new(name='UVMap')
            c = bpy.data.objects.new(p.name + '_' + str(index), part)
            c.matrix_world = to_origin @ p.matrix_world
            context.scene.collection.objects.link(c)
            copies.append(c)
        bpy.data.meshes.remove(me)
    return copies


class RIGVR_OT_export(bpy.types.Operator):
    bl_idname = 'rigvr.export'
    bl_label = 'RigVR: export models'
    bl_description = 'Write every model in "RigVR Models" to <folder>/Meshes/RigVR/Custom/<name>.nif (with PyNifly)'

    def execute(self, context):
        data = bpy.path.abspath(context.scene.rigvr_folder)
        out = os.path.join(data, 'Meshes', 'RigVR', 'Custom')
        os.makedirs(out, exist_ok=True)
        done = []
        for e in bpy.data.collections['RigVR Models'].objects:
            if e.type != 'EMPTY':
                continue
            parts = [c for c in e.children_recursive
                     if c.type == 'MESH' and '(hologram size)' not in c.name and not c.hide_get() and not c.hide_viewport]
            if not parts:
                continue
            copies = export_parts(context, parts, e.matrix_world.inverted(), data)
            bpy.ops.object.select_all(action='DESELECT')
            for c in copies:
                c.select_set(True)
            context.view_layer.objects.active = copies[0]
            path = os.path.join(out, e.name + '.nif')
            try:
                bpy.ops.export_scene.pynifly(filepath=path, target_game='FO4', intuit_defaults=False)
                done.append('%s (%d tris)' % (e.name, sum(len(f.vertices) - 2 for c in copies for f in c.data.polygons)))
            except Exception as error:   # PyNifly missing or a different version
                self.report({'ERROR'}, 'Export of %s failed: %s' % (e.name, error))
            for c in copies:
                me = c.data
                bpy.data.objects.remove(c, do_unlink=True)
                bpy.data.meshes.remove(me)
        self.report({'INFO'}, 'Exported: ' + (', '.join(done) or 'nothing (no meshes under the empties)'))
        return {'FINISHED'}


class RIGVR_PT_panel(bpy.types.Panel):
    bl_label = 'RigVR'
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = 'RigVR'

    def draw(self, context):
        self.layout.prop(context.scene, 'rigvr_folder', text='Folder')
        self.layout.operator('rigvr.export')


def register():
    bpy.types.Scene.rigvr_folder = bpy.props.StringProperty(
        name='Folder', subtype='DIR_PATH',
        default=r'G:\GAMES\Steam\steamapps\common\Fallout 4 VR\Data')
    for cls in (RIGVR_OT_export, RIGVR_PT_panel):
        try:
            bpy.utils.register_class(cls)
        except ValueError:
            bpy.utils.unregister_class(cls)
            bpy.utils.register_class(cls)


register()
if 'RigVR Models' not in bpy.data.collections:
    build()
