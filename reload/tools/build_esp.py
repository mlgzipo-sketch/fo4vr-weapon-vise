#!/usr/bin/env python3
"""Build ManualReload.esp (no Creation Kit needed).

Records (FormIDs fixed: saves keep the running quest and the ghosts by id, never renumber):
  MISC MR_Mag10mm / MR_Mag10mmLarge     a magazine as a physical object (vanilla models)
  STAT MR_Ghost10mm / MR_Ghost10mmLarge the same models as outlines at the magazine well
  EFSH MR_Outline                       green edge glow (from HologramShaunFXS, as in WeaponVise)
  SOUN MR_SndSnap / MR_SndDrop / MR_SndSlide
  FLST MR_Mags / MR_Ghosts / MR_Sounds
  QUST MR_Quest                         start-game-enabled, runs ManualReloadMain
Plain .esp, form version 131, master Fallout4.esm; works in Fallout 4 VR.
"""
import struct, sys

OUT = sys.argv[1] if len(sys.argv) > 1 else 'ManualReload.esp'
FORM_VERSION = 131
OWN = 0x01000000

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

IDS = {'MR_Mag10mm': 0x800, 'MR_Mag10mmLarge': 0x801, 'MR_Ghost10mm': 0x802, 'MR_Ghost10mmLarge': 0x803,
       'MR_Outline': 0x804, 'MR_SndSnap': 0x805, 'MR_SndDrop': 0x806, 'MR_SndSlide': 0x807,
       'MR_Mags': 0x808, 'MR_Ghosts': 0x809, 'MR_Sounds': 0x80A, 'MR_Quest': 0x80B}
recs = {}
def add(tag, edid, subs):
    fid = OWN | IDS[edid]
    recs.setdefault(tag, []).append(record(tag, fid, [sub('EDID', zstr(edid))] + subs))
    return fid

MODELS = [('10mm', 'Weapons\\10mmPistol\\10mmMag01.nif', '10mm Magazine'),
          ('10mmLarge', 'Weapons\\10mmPistol\\10mmMagLarge.nif', '10mm Large Magazine')]
mags = [add('MISC', 'MR_Mag' + k, [sub('OBND', b'\0' * 12), sub('FULL', zstr(name)), sub('MODL', zstr(model)),
                                   sub('DATA', struct.pack('<If', 0, 0.0))]) for k, model, name in MODELS]
ghosts = [add('STAT', 'MR_Ghost' + k, [sub('OBND', b'\0' * 12), sub('MODL', zstr(model))]) for k, model, name in MODELS]

# HologramShaunFXS (Fallout4.esm 00183AFB): no fill, a glowing edge (same bytes as WeaponVise).
HOLOGRAM_DNAM = bytes.fromhex(
    '050000000200000007000000000000000000803e0000003f0000803f0000803e000000000000803f00000000000000000000003f73bed7000ad7233c0000a0410000803fcdcccc3e0000803e0000003f3333b33ecdcccc3e0200000000000000000020410000000000007f4300000000ffffff00ffffff00040000803f0000004000000040000000000000803f00002041088003000000803fcdcc4c3f')
d = bytearray(HOLOGRAM_DNAM)
struct.pack_into('<ffff', d, 16, 0.05, 1.0, 0.1, 1.0)
struct.pack_into('<ff', d, 32, 0.0, 1.0)
d[52:56] = bytes((60, 255, 120)) + b'\0'
outline = add('EFSH', 'MR_Outline', [sub('ICON', zstr('Effects\\ColorBlackZeroAlphaUtility.dds')),
                                      sub('NAM7', b'\0'), sub('NAM8', b'\0'), sub('DATA', b''), sub('DNAM', bytes(d))])

# ItemSnapSuccess, ItemPickUpMetal1Small (sound descriptors of Fallout4.esm, as used by WeaponVise)
SOUNDS = [('MR_SndSnap', 0x1926A2), ('MR_SndDrop', 0x0196B0), ('MR_SndSlide', 0x0196B0)]
sounds = [add('SOUN', e, [sub('OBND', b'\0' * 12), sub('SDSC', struct.pack('<I', s))]) for e, s in SOUNDS]

def flst(edid, forms):
    return add('FLST', edid, [sub('LNAM', struct.pack('<I', f)) for f in forms])
flst_mags = flst('MR_Mags', mags)
flst_ghosts = flst('MR_Ghosts', ghosts)
flst_sounds = flst('MR_Sounds', sounds)

def wstr(s): b = s.encode('latin1'); return struct.pack('<H', len(b)) + b
def prop_obj(name, formid): return wstr(name) + bytes([1, 1]) + struct.pack('<HhI', 0, -1, formid)
props = {'MR_Mags': flst_mags, 'MR_Ghosts': flst_ghosts, 'MR_Sounds': flst_sounds, 'Outline': outline}
vmad = struct.pack('<hhH', 6, 2, 1) + wstr('ManualReloadMain') + b'\0' + struct.pack('<H', len(props))
vmad += b''.join(prop_obj(k, v) for k, v in props.items())
add('QUST', 'MR_Quest', [sub('VMAD', vmad), sub('FULL', zstr('Manual Reload')),
                         sub('DNAM', bytes.fromhex('110000000000000000000000')), sub('NEXT', b''),
                         sub('ANAM', struct.pack('<I', 0))])

ORDER = ['SOUN', 'MISC', 'STAT', 'QUST', 'EFSH', 'FLST']
nrec = sum(len(recs[t]) for t in ORDER)
tes4 = record('TES4', 0, [sub('HEDR', struct.pack('<fII', 1.0, nrec, max(IDS.values()) + 1)), sub('CNAM', zstr('Aleshon')),
                          sub('SNAM', zstr('Manual Reload: reload guns by hand in VR.')),
                          sub('MAST', zstr('Fallout4.esm')), sub('DATA', b'\0' * 8)])
with open(OUT, 'wb') as f:
    f.write(tes4)
    for t in ORDER:
        f.write(group(t, recs[t]))
print('wrote', OUT, nrec, 'records')
