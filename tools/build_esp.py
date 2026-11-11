#!/usr/bin/env python3
"""Build WeaponVise.esp from data/parts.csv (no Creation Kit needed).

Records:
  MISC WV_Part_*       one loose part per weapon mod with a visible model of its own
  FLST WV_Mods / WV_PartItems / WV_LooseMods   parallel lists (same order)
  FLST WV_Weapons      weapons that can be clamped
  STAT WV_Ghost_*      the same models as outlines: where a part goes, which part a hand holds
  FLST WV_Ghosts       parallel to WV_Mods
  EFSH WV_OutlineAttach / WV_OutlineRemove   edge-only glow (copied from HologramShaunFXS)
  SOUN WV_Snd*, FLST WV_Sounds   workshop UI sounds: focus, focus red, snap, pick up
  QUST WV_Quest        start-game-enabled quest running WeaponViseMain
Plain .esp, form version 131, master Fallout4.esm; works in Fallout 4 VR.
Also writes WeaponVise_parts.csv for the plugin (same rows as data/parts.csv).
"""
import csv, os, shutil, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = sys.argv[1] if len(sys.argv) > 1 else 'WeaponVise.esp'
FORM_VERSION = 131
OWN = 0x01000000
next_id = 0x900   # records added after 0.5 get ids from here

# FormIDs of 0.5 must never move: saves keep the running quest script by its id (0x816),
# and a script whose quest id points at another record loses all its properties.
PINNED = {'WV_Mods': 0x812, 'WV_PartItems': 0x813, 'WV_LooseMods': 0x814, 'WV_Weapons': 0x815, 'WV_Quest': 0x816}
for _k, _e in enumerate(['Grip_Standard', 'Mag_Large', 'Mag_Small', 'Barrel_LightShort', 'Muzzle_Suppressor',
                         'Scope_SightReflex', 'Scope_SightReflex_Circle', 'Scope_SightsGlow', 'Grip_Better1',
                         'Barrel_LightPortedShort', 'Grip_Better2', 'Muzzle_MuzzleBrake', 'Muzzle_Compensator',
                         'Mag_LargeQuick', 'Mag_SmallQuick', 'Barrel_Short', 'Barrel_PortedShort', 'Scope_ScopeRecon']):
    PINNED['WV_Part_10mm_' + _e] = 0x800 + _k

def zstr(s, enc='cp1251'): return s.encode(enc) + b'\0'
def sub(tag, data):
    assert len(data) < 0x10000
    return tag.encode() + struct.pack('<H', len(data)) + data
def record(tag, formid, subs, flags=0):
    body = b''.join(subs)
    return tag.encode() + struct.pack('<IIIIHH', len(body), flags, formid, 0, FORM_VERSION, 0) + body
def group(tag, records):
    body = b''.join(records)
    return b'GRUP' + struct.pack('<I4sIIHH', 24 + len(body), tag.encode(), 0, 0, 0, 0) + body
def new_id():
    global next_id
    fid = OWN | next_id
    next_id += 1
    return fid

recs, ids = {}, {}
def add(tag, edid, subs):
    fid = (OWN | PINNED[edid]) if edid in PINNED else new_id()
    ids[edid] = fid
    recs.setdefault(tag, []).append(record(tag, fid, [sub('EDID', zstr(edid))] + subs))
    return fid

rows = list(csv.DictReader(open(os.path.join(ROOT, 'data', 'parts.csv'), newline='')))
for r in rows:
    for k in ('omod', 'weapon', 'loose', 'fallback'):
        r[k] = int(r[k], 16)

WORKBENCH_KEYWORD = 0x0017B3A2   # AnimFurnWorkbenchWeapons (on workbenchWeaponsA/B)

def pretty(edid):
    # mod_10mm_Scope_SightReflex -> 10mm Scope SightReflex
    return edid.replace('mod_', '', 1).replace('_', ' ')

parts = [r for r in rows if r['part'] == '1']
part_items = []
for r in parts:
    part_items.append(add('MISC', 'WV_Part_' + r['edid'].replace('mod_', '', 1), [
        sub('OBND', b'\0' * 12),
        sub('FULL', zstr(pretty(r['edid']), 'latin1')),
        sub('MODL', zstr(r['model'], 'latin1')),
        sub('DATA', struct.pack('<If', 5, 0.5)),
    ]))

ghosts = []
for r in parts:
    ghosts.append(add('STAT', 'WV_Ghost_' + r['edid'].replace('mod_', '', 1), [
        sub('OBND', b'\0' * 12),
        sub('MODL', zstr(r['model'], 'latin1')),
    ]))

# Glowing corner brackets above the vise: the spot where a released weapon stays (tools/build_frame_nif.py).
add('STAT', 'WV_ZoneFrame', [sub('OBND', b'\0' * 12), sub('MODL', zstr('WeaponVise\\ZoneFrame.nif', 'latin1'))])

# HologramShaunFXS (Fallout4.esm 00183AFB): no fill (black zero-alpha texture), a glowing edge.
HOLOGRAM_DNAM = bytes.fromhex(
    '050000000200000007000000000000000000803e0000003f0000803f0000803e000000000000803f00000000000000000000003f73bed7000ad7233c0000a0410000803fcdcccc3e0000803e0000003f3333b33ecdcccc3e0200000000000000000020410000000000007f4300000000ffffff00ffffff00040000803f0000004000000040000000000000803f00002041088003000000803fcdcc4c3f')
def outline(edid, rgb):
    d = bytearray(HOLOGRAM_DNAM)
    struct.pack_into('<ffff', d, 16, 0.05, 1.0, 0.1, 1.0)   # fill: fade in, full, fade out, persistent ratio
    struct.pack_into('<ff', d, 32, 0.0, 1.0)                 # fill: no pulse
    d[52:56] = bytes(rgb) + b'\0'                            # edge colour
    return add('EFSH', edid, [sub('ICON', zstr('Effects\\ColorBlackZeroAlphaUtility.dds', 'latin1')),
                              sub('NAM7', b'\0'), sub('NAM8', b'\0'), sub('DATA', b''), sub('DNAM', bytes(d))])
outline('WV_OutlineAttach', (60, 255, 120))   # Pip-Boy green: where the part goes
outline('WV_OutlineRemove', (255, 70, 40))    # red: the part a hand holds on the weapon

SOUNDS = [('WV_SndFocus', 0x05FD82), ('WV_SndFocusRed', 0x05FD83), ('WV_SndSnap', 0x1926A2),
          ('WV_SndPickUp', 0x0196B0)]   # UIWorkshopModeFocusGreen/Red, ItemSnapSuccess, ItemPickUpMetal1Small
sounds = [add('SOUN', e, [sub('OBND', b'\0' * 12), sub('SDSC', struct.pack('<I', d))]) for e, d in SOUNDS]

def flst(edid, forms):
    return add('FLST', edid, [sub('LNAM', struct.pack('<I', f)) for f in forms])

flst('WV_Mods', [r['omod'] for r in parts])
flst('WV_PartItems', part_items)
flst('WV_LooseMods', [r['loose'] for r in parts])
flst('WV_Ghosts', ghosts)
flst('WV_Sounds', sounds)
flst('WV_Weapons', sorted({r['weapon'] for r in rows}))

def wstr(s): b = s.encode('latin1'); return struct.pack('<H', len(b)) + b
def prop_obj(name, formid): return wstr(name) + bytes([1, 1]) + struct.pack('<HhI', 0, -1, formid)

props = {
    'WorkbenchKeyword': WORKBENCH_KEYWORD,
    'WV_Mods': ids['WV_Mods'], 'WV_PartItems': ids['WV_PartItems'], 'WV_Weapons': ids['WV_Weapons'],
    'WV_LooseMods': ids['WV_LooseMods'],
    'WV_Ghosts': ids['WV_Ghosts'], 'WV_Sounds': ids['WV_Sounds'],
    'ZoneFrame': ids['WV_ZoneFrame'],
    'OutlineAttach': ids['WV_OutlineAttach'], 'OutlineRemove': ids['WV_OutlineRemove'],
}
vmad = struct.pack('<hhH', 6, 2, 1) + wstr('WeaponViseMain') + b'\0' + struct.pack('<H', len(props))
vmad += b''.join(prop_obj(k, v) for k, v in props.items())
add('QUST', 'WV_Quest', [sub('VMAD', vmad), sub('FULL', zstr('Weapon Vise', 'latin1')),
                         sub('DNAM', bytes.fromhex('110000000000000000000000')), sub('NEXT', b''),
                         sub('ANAM', struct.pack('<I', 0))])

ORDER = ['SOUN', 'MISC', 'STAT', 'QUST', 'EFSH', 'FLST']
nrec = sum(len(recs[t]) for t in ORDER)
tes4 = record('TES4', 0, [sub('HEDR', struct.pack('<fII', 1.0, nrec, next_id)), sub('CNAM', zstr('Aleshon')),
                          sub('SNAM', zstr('Weapon Vise: modify weapons by hand at a weapons workbench.', 'latin1')),
                          sub('MAST', zstr('Fallout4.esm')), sub('DATA', b'\0' * 8)])
with open(OUT, 'wb') as f:
    f.write(tes4)
    for t in ORDER:
        f.write(group(t, recs[t]))
shutil.copyfile(os.path.join(ROOT, 'data', 'parts.csv'),
                os.path.join(os.path.dirname(os.path.abspath(OUT)), 'F4SE', 'Plugins', 'WeaponVise_parts.csv')
                if os.path.isdir(os.path.join(os.path.dirname(os.path.abspath(OUT)), 'F4SE', 'Plugins'))
                else os.path.join(os.path.dirname(os.path.abspath(OUT)), 'WeaponVise_parts.csv'))
print('wrote', OUT, nrec, 'records,', len(parts), 'parts')
