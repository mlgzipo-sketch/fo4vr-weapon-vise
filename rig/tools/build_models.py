"""Writes RigVR's placeholder models: glowing outlines (holograms) of the mannequin, the rig and the pouches.

usage: python rig/tools/build_models.py <out dir>   (writes <out dir>/Meshes/RigVR/*.nif)

Each model is a set of line segments drawn as thin crossed quads with an additive effect shader
(the same blocks as WeaponVise's ZoneFrame, which is seen in game). No collision: the plugin moves them.
Anyone can replace a file with a real model of the same name (Blender + the Fallout 4 nif plugin):
the front of a pouch is +Y, its middle is the origin, the mannequin stands on its origin facing +Y.
Fallout 4 NIF (20.2.0.7, user 12, BS 130): BSFadeNode -> BSTriShape + BSEffectShaderProperty + NiAlphaProperty.
"""
import math
import os
import struct
import sys

SHADER = bytes.fromhex(
    '0300000000000000ffffffff000000800100000000000000000000000000803f0000803f2300000074657874757265735c45666665'
    '6374735c476c6f7753706f744c696e6561722e64647303ff00000000803f0000803f000000000000000079c1403d0000803f00000000'
    '0000803fe17a44400000c842000000000000000000000000000000000000803f')
ALPHA = bytes.fromhex('0300000000000000ffffffffec1280')


def box(cx, cy, cz, hx, hy, hz):
    """The 12 edges of a box."""
    out = []
    for a in (-1, 1):
        for b in (-1, 1):
            out.append(((cx - hx, cy + a * hy, cz + b * hz), (cx + hx, cy + a * hy, cz + b * hz)))
            out.append(((cx + a * hx, cy - hy, cz + b * hz), (cx + a * hx, cy + hy, cz + b * hz)))
            out.append(((cx + a * hx, cy + b * hy, cz - hz), (cx + a * hx, cy + b * hy, cz + hz)))
    return out


def ring(cx, cy, cz, rx, ry, n=16):
    pts = [(cx + rx * math.sin(2 * math.pi * i / n), cy + ry * math.cos(2 * math.pi * i / n), cz) for i in range(n)]
    return [(pts[i], pts[(i + 1) % n]) for i in range(n)]


def quads(segments, thick):
    out = []
    for a, b in segments:
        d = [b[i] - a[i] for i in range(3)]
        length = math.sqrt(sum(x * x for x in d)) or 1.0
        d = [x / length for x in d]
        # two directions across the segment
        up = (0.0, 0.0, 1.0) if abs(d[2]) < 0.9 else (1.0, 0.0, 0.0)
        s1 = [d[1] * up[2] - d[2] * up[1], d[2] * up[0] - d[0] * up[2], d[0] * up[1] - d[1] * up[0]]
        n1 = math.sqrt(sum(x * x for x in s1))
        s1 = [x / n1 * thick for x in s1]
        s2 = [d[1] * s1[2] - d[2] * s1[1], d[2] * s1[0] - d[0] * s1[2], d[0] * s1[1] - d[1] * s1[0]]
        for off in (s1, s2):
            out.append(([a[i] - off[i] for i in range(3)], [a[i] + off[i] for i in range(3)],
                        [b[i] + off[i] for i in range(3)], [b[i] - off[i] for i in range(3)]))
    return out


def vertex(p):
    return (struct.pack('<eeee', p[0], p[1], p[2], 1.0) + struct.pack('<ee', 0.5, 0.5)
            + bytes([0x7f, 0x7f, 0xff, 0x7f]) + bytes([0x7f, 0x00, 0x7f, 0x7f]))


def av_object(name, flags=0x0E):
    identity = (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0)
    return (struct.pack('<iIi', name, 0, -1) + struct.pack('<I', flags) + struct.pack('<3f', 0, 0, 0)
            + struct.pack('<9f', *identity) + struct.pack('<f', 1.0) + struct.pack('<i', -1))


def nif(segments, color, scale=1.6, thick=0.25):
    qs = quads(segments, thick)
    assert 4 * len(qs) < 65536
    verts = b''.join(vertex(p) for q in qs for p in q)
    tris = b''.join(struct.pack('<6H', 4 * k, 4 * k + 1, 4 * k + 2, 4 * k, 4 * k + 2, 4 * k + 3) for k in range(len(qs)))
    pts = [p for q in qs for p in q]
    centre = [(min(p[i] for p in pts) + max(p[i] for p in pts)) / 2 for i in range(3)]
    radius = max(math.sqrt(sum((p[i] - centre[i]) ** 2 for i in range(3))) for p in pts) + 1.0
    root = av_object(0) + struct.pack('<I', 1) + struct.pack('<i', 1)
    shape = (av_object(1) + struct.pack('<4f', centre[0], centre[1], centre[2], radius) + struct.pack('<iii', -1, 2, 3)
             + bytes.fromhex('0502430000b00100')
             + struct.pack('<IHI', 2 * len(qs), 4 * len(qs), len(verts) + len(tris)) + verts + tris)
    shader = bytearray(SHADER)
    struct.pack_into('<i', shader, 0, 2)
    struct.pack_into('<I', shader, 16, struct.unpack_from('<I', shader, 16)[0] | 0x10)   # double sided
    struct.pack_into('<4f', shader, 95, *color)
    struct.pack_into('<f', shader, 111, scale)
    alpha = bytearray(ALPHA)
    struct.pack_into('<i', alpha, 0, 2)
    types = ['BSFadeNode', 'BSTriShape', 'BSEffectShaderProperty', 'NiAlphaProperty']
    blocks = [(0, root), (1, shape), (2, bytes(shader)), (3, bytes(alpha))]
    strings = ['RigVR', 'Shape:0', '']
    out = b'Gamebryo File Format, Version 20.2.0.7\n'
    out += struct.pack('<IBII', 0x14020007, 1, 12, len(blocks)) + struct.pack('<I', 130)
    for s in (b'RigVR\0', b'\0', b'\0', b'\0'):
        out += bytes([len(s)]) + s
    out += struct.pack('<H', len(types))
    for t in types:
        out += struct.pack('<I', len(t)) + t.encode()
    out += b''.join(struct.pack('<H', t) for t, _ in blocks)
    out += b''.join(struct.pack('<I', len(b)) for _, b in blocks)
    out += struct.pack('<II', len(strings), max(len(s) for s in strings))
    for s in strings:
        out += struct.pack('<I', len(s)) + s.encode()
    out += struct.pack('<I', 0)
    out += b''.join(b for _, b in blocks)
    out += struct.pack('<Ii', 1, 0)
    return out


# Heights match the plugin: belt anchor 66, chest anchor 92 (game units, about 1.43 cm each).
def mannequin():
    s = []
    s += ring(0, 0, 0.5, 13, 13, 12)                       # stand
    s += [((0, 0, 0.5), (0, 0, 60))]                       # pole
    s += box(0, 0, 66, 12, 7, 5)                           # hips
    s += box(0, 0, 88, 13, 7, 15)                          # torso 73..103
    s += [((-19, 0, 103), (19, 0, 103))]                   # shoulders
    s += [((-19, 0, 103), (-20, 2, 78)), ((19, 0, 103), (20, 2, 78))]   # arms
    s += [((0, 0, 103), (0, 0, 109))]                      # neck
    s += box(0, 0.5, 116, 5, 6, 7)                         # head
    s += [((-3, 6.5, 118), (3, 6.5, 118))]                 # a line for the face: it looks +Y
    return s


def rig():
    s = []
    s += ring(0, 0, 66, 13.5, 8.5, 16)                     # belt
    s += ring(0, 0, 63, 13.5, 8.5, 16)
    for x in (-11, 11):                                    # front straps and panel
        s += [((x, 8, 70), (x * 0.6, 8, 100)), ((x, -8, 70), (x * 0.6, -8, 100))]
        s += [((x * 0.6, 8, 100), (x * 0.6, 0, 104)), ((x * 0.6, -8, 100), (x * 0.6, 0, 104))]
    s += [((-11, 8.5, 80), (11, 8.5, 80)), ((-11, 8.5, 98), (11, 8.5, 98))]
    s += [((-11, 8.5, 80), (-11, 8.5, 98)), ((11, 8.5, 80), (11, 8.5, 98))]
    return s


# kind -> (half sizes, colour). Order = pouch type 1..6 in the plugin.
POUCHES = [
    ('Mag', (3.0, 2.0, 4.5), (0.35, 1.0, 0.35, 1.0)),        # magazines: green
    ('Med', (4.0, 2.5, 4.0), (1.0, 0.25, 0.25, 1.0)),        # first aid: red
    ('Grenade', (3.0, 2.5, 3.5), (1.0, 0.9, 0.2, 1.0)),      # grenades: yellow
    ('Holster', (3.0, 2.0, 7.5), (0.3, 0.6, 1.0, 1.0)),      # holster: blue
    ('Dump', (5.0, 3.5, 5.0), (1.0, 0.55, 0.15, 1.0)),       # dump pouch: orange
    ('Bandolier', (7.0, 1.5, 2.0), (0.8, 0.55, 0.35, 1.0)),  # ammo loops: brown
]


def pouch(h):
    s = box(0, 0, 0, *h)
    # a flap line on the front so the outline reads as a pouch
    s += [((-h[0], h[1] + 0.3, h[2] * 0.5), (h[0], h[1] + 0.3, h[2] * 0.5))]
    return s


def build(out_dir):
    d = os.path.join(out_dir, 'Meshes', 'RigVR')
    os.makedirs(d, exist_ok=True)
    files = {
        'Mannequin.nif': nif(mannequin(), (0.55, 0.95, 1.0, 1.0), 1.2),
        'Rig.nif': nif(rig(), (0.95, 0.8, 0.5, 1.0), 1.3),
        'Ghost.nif': nif(box(0, 0, 0, 3.0, 2.0, 4.0), (0.6, 0.6, 0.6, 1.0), 0.6, 0.15),
        'Cursor.nif': nif(box(0, 0, 0, 5.0, 4.0, 6.0), (1.0, 1.0, 1.0, 1.0), 2.0, 0.2),
    }
    for name, half, color in POUCHES:
        files['Pouch' + name + '.nif'] = nif(pouch(half), color, 1.6)
    for name, data in files.items():
        with open(os.path.join(d, name), 'wb') as f:
            f.write(data)
    print('wrote', len(files), 'models to', d)


if __name__ == '__main__':
    build(sys.argv[1] if len(sys.argv) > 1 else '.')
