#!/usr/bin/env python3
"""Build RigVR.esp (no Creation Kit needed).

Records (FormIDs fixed: a save keeps the quest and the items by id, never renumber; add new ones at the end):
  STAT RigVR_Mannequin / Rig / Ghost / Cursor / Pouch*   what the plugin draws (models from build_models.py)
  MISC RigVR_Kit                                         the folded mannequin: drop it from the Pip-Boy to set it up
  MISC RigVR_Item*                                       the pouches as inventory items
  SOUN RigVR_Snd*                                        vanilla sounds: snap, pick up, focus, put in
  FLST RigVR_Stats / RigVR_PouchItems / RigVR_Sounds
  QUST RigVR_Quest                                       start-game-enabled, runs RigVRMain
Plain .esp, form version 131, master Fallout4.esm; works in Fallout 4 VR.
"""
import struct
import sys

OUT = sys.argv[1] if len(sys.argv) > 1 else 'RigVR.esp'
FORM_VERSION = 131
OWN = 0x01000000

POUCHES = ['Mag', 'Med', 'Grenade', 'Holster', 'Dump', 'Bandolier', 'Sling']   # pouch type 1..7 in the plugin
NAMES = {'Mag': 'Rig: Magazine Pouch', 'Med': 'Rig: First Aid Pouch', 'Grenade': 'Rig: Grenade Pouch',
         'Holster': 'Rig: Holster', 'Dump': 'Rig: Dump Pouch', 'Bandolier': 'Rig: Ammo Loops',
         'Sling': 'Rig: Back Gun Sling'}

IDS = {'RigVR_Mannequin': 0x800, 'RigVR_Rig': 0x801, 'RigVR_Ghost': 0x802, 'RigVR_Cursor': 0x803}
for k, p in enumerate(POUCHES[:6]):
    IDS['RigVR_Pouch' + p] = 0x804 + k          # 0x804..0x809
IDS['RigVR_Kit'] = 0x80A
for k, p in enumerate(POUCHES[:6]):
    IDS['RigVR_Item' + p] = 0x80B + k           # 0x80B..0x810
IDS.update({'RigVR_SndSnap': 0x811, 'RigVR_SndPickUp': 0x812, 'RigVR_SndFocus': 0x813, 'RigVR_SndPutIn': 0x814,
            'RigVR_Stats': 0x815, 'RigVR_PouchItems': 0x816, 'RigVR_Sounds': 0x817, 'RigVR_Quest': 0x818,
            'RigVR_PouchSling': 0x819, 'RigVR_ItemSling': 0x81A})   # 0.2
# 0.4: the pouches are real items hung on the rig by hand; three magazine pouches replace the old one; the rig is an item
IDS.update({'RigVR_ItemMagPistol': 0x81B, 'RigVR_ItemMagRifle': 0x81C, 'RigVR_ItemMagUniversal': 0x81D,
            'RigVR_ItemRigBase': 0x81E, 'RigVR_OldItems': 0x81F})


def zstr(s): return s.encode('latin1') + b'\0'
def sub(tag, data):
    assert len(data) < 0x10000
    return tag.encode() + struct.pack('<H', len(data)) + data
def record(tag, formid, subs):
    body = b''.join(subs)
    return tag.encode() + struct.pack('<IIIIHH', len(body), 0, formid, 0, FORM_VERSION, 0) + body
def group(tag, records):
    body = b''.join(records)
    return b'GRUP' + struct.pack('<I4sIIHH', 24 + len(body), tag.encode(), 0, 0, 0, 0) + body


recs = {}
def add(tag, edid, subs):
    fid = OWN | IDS[edid]
    recs.setdefault(tag, []).append(record(tag, fid, [sub('EDID', zstr(edid))] + subs))
    return fid


def wstr(s): b = s.encode('latin1'); return struct.pack('<H', len(b)) + b
def prop_obj(name, formid): return wstr(name) + bytes([1, 1]) + struct.pack('<HhI', 0, -1, formid)
def prop_int(name, value): return wstr(name) + bytes([3, 1]) + struct.pack('<i', value)
def vmad(script, props):
    out = struct.pack('<hhH', 6, 2, 1) + wstr(script) + b'\0' + struct.pack('<H', len(props))
    return out + b''.join(props)


# What the plugin draws. Order = static index in the plugin (StatKind): 0 mannequin, 1 rig, 2 ghost, 3 cursor, 4.. pouches.
stats = [add('STAT', 'RigVR_Mannequin', [sub('OBND', b'\0' * 12), sub('MODL', zstr('RigVR\\Custom\\Mannequin.nif'))]),
         add('STAT', 'RigVR_Rig', [sub('OBND', b'\0' * 12), sub('MODL', zstr('RigVR\\Rig.nif'))]),
         add('STAT', 'RigVR_Ghost', [sub('OBND', b'\0' * 12), sub('MODL', zstr('RigVR\\Ghost.nif'))]),
         add('STAT', 'RigVR_Cursor', [sub('OBND', b'\0' * 12), sub('MODL', zstr('RigVR\\Cursor.nif'))])]
stats += [add('STAT', 'RigVR_Pouch' + p, [sub('OBND', b'\0' * 12), sub('MODL', zstr('RigVR\\Pouch' + p + '.nif'))])
          for p in POUCHES]

QUEST = OWN | IDS['RigVR_Quest']


def item(edid, name, model, kind, value, weight):
    return add('MISC', edid, [sub('VMAD', vmad('RigVRItem', [prop_obj('Main', QUEST), prop_int('Kind', kind)])),
                              sub('OBND', b'\0' * 12), sub('FULL', zstr(name)), sub('MODL', zstr(model)),
                              sub('DATA', struct.pack('<If', value, weight))])


kit = item('RigVR_Kit', 'Rig Mannequin (drop to set up)', 'RigVR\\Mannequin.nif', 0, 50, 5.0)
# the old pouch items (0.1-0.3) stay as records so old saves load; the script takes them out of the inventory
old_items = [item('RigVR_Item' + p, NAMES[p], 'RigVR\\Pouch' + p + '.nif', -1, 15, 0.5) for p in ('Mag', 'Med', 'Grenade', 'Bandolier')]
# kind 1..6 in the plugin, then the rig itself (kind 7); models from the user's Blender scene (Meshes\RigVR\Custom)
KINDS = [('MagPistol', 'Rig: Pistol Mag Pouch', 0.4), ('MagRifle', 'Rig: Rifle Mag Pouch', 0.5),
         ('MagUniversal', 'Rig: Universal Mag Pouch', 0.5), ('Holster', 'Rig: Pistol Holster', 0.5),
         ('Dump', 'Rig: Dump Bag', 0.8), ('Sling', 'Rig: Carbine Holder', 0.6)]
items = [item('RigVR_Item' + k, n, 'RigVR\\Custom\\ItemPouch' + k + '.nif', i + 1, 15, w) for i, (k, n, w) in enumerate(KINDS)]
items.append(item('RigVR_ItemRigBase', 'Rig: Chest Rig', 'RigVR\\Custom\\ItemRigBase.nif', 7, 40, 3.0))

# UIWorkshopModeFocusGreen, ItemSnapSuccess, ItemPickUpMetal1Small (sound descriptors of Fallout4.esm, as WeaponVise uses)
SOUNDS = [('RigVR_SndSnap', 0x1926A2), ('RigVR_SndPickUp', 0x0196B0), ('RigVR_SndFocus', 0x05FD82), ('RigVR_SndPutIn', 0x0196B0)]
sounds = [add('SOUN', e, [sub('OBND', b'\0' * 12), sub('SDSC', struct.pack('<I', d))]) for e, d in SOUNDS]

def flst(edid, forms):
    return add('FLST', edid, [sub('LNAM', struct.pack('<I', f)) for f in forms])
flst_stats = flst('RigVR_Stats', stats)
flst_items = flst('RigVR_PouchItems', items)
flst_sounds = flst('RigVR_Sounds', sounds)
flst_old = flst('RigVR_OldItems', old_items)

props = [prop_obj('RigVR_Stats', flst_stats), prop_obj('RigVR_PouchItems', flst_items),
         prop_obj('RigVR_Sounds', flst_sounds), prop_obj('Kit', kit), prop_obj('RigVR_OldItems', flst_old)]
add('QUST', 'RigVR_Quest', [sub('VMAD', vmad('RigVRMain', props)), sub('FULL', zstr('Rig VR')),
                            sub('DNAM', bytes.fromhex('110000000000000000000000')), sub('NEXT', b''),
                            sub('ANAM', struct.pack('<I', 0))])

ORDER = ['SOUN', 'STAT', 'MISC', 'QUST', 'FLST']
nrec = sum(len(recs[t]) for t in ORDER)
tes4 = record('TES4', 0, [sub('HEDR', struct.pack('<fII', 1.0, nrec, max(IDS.values()) + 1)), sub('CNAM', zstr('Aleshon')),
                          sub('SNAM', zstr('RigVR: a load-bearing rig on a mannequin, pouches you reach by hand (VR).')),
                          sub('MAST', zstr('Fallout4.esm')), sub('DATA', b'\0' * 8)])
with open(OUT, 'wb') as f:
    f.write(tes4)
    for t in ORDER:
        f.write(group(t, recs[t]))
print('wrote', OUT, nrec, 'records')
