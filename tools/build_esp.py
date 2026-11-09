#!/usr/bin/env python3
"""Build WeaponVise.esp from data/parts.csv (no Creation Kit needed).

Records:
  MISC WV_Part_*       one loose part per weapon mod with a visible model of its own
  FLST WV_Mods / WV_PartItems   parallel lists (same order, no duplicates)
  FLST WV_Weapons      weapons that can be clamped
  QUST WV_Quest        start-game-enabled quest running WeaponViseMain
Plain .esp, form version 131, master Fallout4.esm; works in Fallout 4 VR.
Also writes WeaponVise_parts.csv for the plugin (same rows as data/parts.csv).
"""
import csv, os, shutil, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = sys.argv[1] if len(sys.argv) > 1 else 'WeaponVise.esp'
FORM_VERSION = 131
OWN = 0x01000000
next_id = 0x800

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
    fid = new_id()
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
        sub('DATA', struct.pack('<If', 0, 0.0)),
    ]))

def flst(edid, forms):
    return add('FLST', edid, [sub('LNAM', struct.pack('<I', f)) for f in forms])

flst('WV_Mods', [r['omod'] for r in parts])
flst('WV_PartItems', part_items)
flst('WV_Weapons', sorted({r['weapon'] for r in rows}))

def wstr(s): b = s.encode('latin1'); return struct.pack('<H', len(b)) + b
def prop_obj(name, formid): return wstr(name) + bytes([1, 1]) + struct.pack('<HhI', 0, -1, formid)

props = {
    'WorkbenchKeyword': WORKBENCH_KEYWORD,
    'WV_Mods': ids['WV_Mods'], 'WV_PartItems': ids['WV_PartItems'], 'WV_Weapons': ids['WV_Weapons'],
}
vmad = struct.pack('<hhH', 6, 2, 1) + wstr('WeaponViseMain') + b'\0' + struct.pack('<H', len(props))
vmad += b''.join(prop_obj(k, v) for k, v in props.items())
add('QUST', 'WV_Quest', [sub('VMAD', vmad), sub('FULL', zstr('Weapon Vise', 'latin1')),
                         sub('DNAM', bytes.fromhex('110000000000000000000000')), sub('NEXT', b''),
                         sub('ANAM', struct.pack('<I', 0))])

ORDER = ['MISC', 'FLST', 'QUST']
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
