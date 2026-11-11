#!/usr/bin/env python3
"""Build ManualReloadF.esp (no Creation Kit needed).

Records (FormIDs fixed: a save keeps the running quest by id, never renumber):
  MISC MRF_Mag10mm / MRF_Mag10mmLarge   a dropped magazine as a physical object (vanilla models)
  SOUN MRF_SndIn / Out / SlideBack / SlideHome / PutAway   the 10mm pistol's own reload sounds
  FLST MRF_Mags / MRF_Sounds
  QUST MRF_Quest                        start-game-enabled, runs ManualReloadFMain
Plain .esp, form version 131, master Fallout4.esm; works in Fallout 4 VR.
"""
import struct, sys

OUT = sys.argv[1] if len(sys.argv) > 1 else 'ManualReloadF.esp'
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

IDS = {'MRF_Mag10mm': 0x800, 'MRF_Mag10mmLarge': 0x801, 'MRF_SndIn': 0x802, 'MRF_SndOut': 0x803,
       'MRF_SndSlideBack': 0x804, 'MRF_SndSlideHome': 0x805, 'MRF_SndPutAway': 0x806,
       'MRF_Mags': 0x807, 'MRF_Sounds': 0x808, 'MRF_Quest': 0x809,
       'MRF_DescIn': 0x80A, 'MRF_DescOut': 0x80B, 'MRF_DescSlideBack': 0x80C, 'MRF_DescSlideHome': 0x80D}
recs = {}
def add(tag, edid, subs):
    fid = OWN | IDS[edid]
    recs.setdefault(tag, []).append(record(tag, fid, [sub('EDID', zstr(edid))] + subs))
    return fid

MODELS = [('10mm', 'Weapons\\10mmPistol\\10mmMag01.nif', '10mm Magazine'),
          ('10mmLarge', 'Weapons\\10mmPistol\\10mmMagLarge.nif', '10mm Large Magazine')]
mags = [add('MISC', 'MRF_Mag' + k, [sub('OBND', b'\0' * 12), sub('FULL', zstr(name)), sub('MODL', zstr(model)),
                                    sub('DATA', struct.pack('<If', 0, 0.0))]) for k, model, name in MODELS]

# Sound descriptors of Fallout4.esm: WPNPistol10mmReloadMagIn / MagOut / BoltOpen / BoltClose, ItemPickUpMetal1Small.
# The order is the sound index the plugin sends.
# Own descriptors for the pistol's reload sounds (the game's wav files): the game's descriptors
# (WPNPistol10mmReload*, 14 dB down, the weapon reload category) were not heard when played from a script,
# so these use the category and output of the workshop sounds, which are heard, and are louder.
W = 'data\\Sound\\FX\\WPN\\Pistol10mm\\Reload\\WPN_Pistol10mm_Reload_'
def sndr(edid, name):
    return add('SNDR', edid, [sub('CNAM', bytes.fromhex('0a54ef1e')), sub('GNAM', struct.pack('<I', 0x0172A1)),
                              sub('ANAM', zstr(W + name + '_01.wav')), sub('ANAM', zstr(W + name + '_02.wav')),
                              sub('ONAM', struct.pack('<I', 0x05A28A)), sub('LNAM', struct.pack('<I', 0)),
                              sub('BNAM', struct.pack('<bBBBH', 0, 5, 0x80, 2, 150))])
SOUNDS = [('MRF_SndIn', sndr('MRF_DescIn', 'MagIn')), ('MRF_SndOut', sndr('MRF_DescOut', 'MagOut')),
          ('MRF_SndSlideBack', sndr('MRF_DescSlideBack', 'BoltOpen')),
          ('MRF_SndSlideHome', sndr('MRF_DescSlideHome', 'BoltClose')), ('MRF_SndPutAway', 0x0196B0)]
sounds = [add('SOUN', e, [sub('OBND', b'\0' * 12), sub('SDSC', struct.pack('<I', s))]) for e, s in SOUNDS]

def flst(edid, forms):
    return add('FLST', edid, [sub('LNAM', struct.pack('<I', f)) for f in forms])
flst_mags = flst('MRF_Mags', mags)
flst_sounds = flst('MRF_Sounds', sounds)

def wstr(s): b = s.encode('latin1'); return struct.pack('<H', len(b)) + b
def prop_obj(name, formid): return wstr(name) + bytes([1, 1]) + struct.pack('<HhI', 0, -1, formid)
props = {'MRF_Mags': flst_mags, 'MRF_Sounds': flst_sounds}
vmad = struct.pack('<hhH', 6, 2, 1) + wstr('ManualReloadFMain') + b'\0' + struct.pack('<H', len(props))
vmad += b''.join(prop_obj(k, v) for k, v in props.items())
add('QUST', 'MRF_Quest', [sub('VMAD', vmad), sub('FULL', zstr('Manual Reload F')),
                          sub('DNAM', bytes.fromhex('110000000000000000000000')), sub('NEXT', b''),
                          sub('ANAM', struct.pack('<I', 0))])

ORDER = ['SNDR', 'SOUN', 'MISC', 'QUST', 'FLST']
nrec = sum(len(recs[t]) for t in ORDER)
tes4 = record('TES4', 0, [sub('HEDR', struct.pack('<fII', 1.0, nrec, max(IDS.values()) + 1)), sub('CNAM', zstr('Aleshon')),
                          sub('SNAM', zstr('Manual Reload F: reload guns by hand in VR.')),
                          sub('MAST', zstr('Fallout4.esm')), sub('DATA', b'\0' * 8)])
with open(OUT, 'wb') as f:
    f.write(tes4)
    for t in ORDER:
        f.write(group(t, recs[t]))
print('wrote', OUT, nrec, 'records')
