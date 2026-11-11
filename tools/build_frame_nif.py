"""Writes the zone frame model: glowing corner brackets of a box around the spot above the vise.

usage: python tools/build_frame_nif.py out.nif
Fallout 4 NIF (20.2.0.7, user 12, BS 130): BSFadeNode -> BSTriShape + BSEffectShaderProperty + NiAlphaProperty.
The shader and alpha blocks are copied from the tritium dot of 10mmNightSights.nif (additive glow).
"""
import struct
import sys

HALF = (11.0, 11.0, 9.0)   # box half extents (game units)
ARM = 3.5                  # length of each corner bracket arm
THICK = 0.3                # half thickness of an arm
COLOR = (0.55, 1.0, 0.35, 1.0)   # Pip-Boy yellow-green
COLOR_SCALE = 1.6

SHADER = bytearray(bytes.fromhex(
    '0300000000000000ffffffff000000800100000000000000000000000000803f0000803f2300000074657874757265735c45666665'
    '6374735c476c6f7753706f744c696e6561722e64647303ff00000000803f0000803f000000000000000079c1403d0000803f00000000'
    '0000803fe17a44400000c842000000000000000000000000000000000000803f'))
ALPHA = bytearray(bytes.fromhex('0300000000000000ffffffffec1280'))


def quads():
    out = []
    for sx in (-1, 1):
        for sy in (-1, 1):
            for sz in (-1, 1):
                corner = (sx * HALF[0], sy * HALF[1], sz * HALF[2])
                inward = (-sx, -sy, -sz)
                for axis in range(3):
                    # an arm from the corner along `axis`, pointing inwards: two crossed quads
                    a = list(corner)
                    b = list(corner)
                    b[axis] += inward[axis] * ARM
                    for side in [i for i in range(3) if i != axis]:
                        off = [0.0, 0.0, 0.0]
                        off[side] = THICK
                        p0 = [a[i] - off[i] for i in range(3)]
                        p1 = [a[i] + off[i] for i in range(3)]
                        p2 = [b[i] + off[i] for i in range(3)]
                        p3 = [b[i] - off[i] for i in range(3)]
                        out.append((p0, p1, p2, p3))
    return out


def vertex(p):
    # half-precision position + bitangent x, UV, normal + bitangent y, tangent + bitangent z (20 bytes)
    return (struct.pack('<eeee', p[0], p[1], p[2], 1.0) + struct.pack('<ee', 0.5, 0.5)
            + bytes([0x7f, 0x7f, 0xff, 0x7f]) + bytes([0x7f, 0x00, 0x7f, 0x7f]))


def av_object(name, flags=0x0E):
    identity = (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0)
    return (struct.pack('<iIi', name, 0, -1) + struct.pack('<I', flags) + struct.pack('<3f', 0, 0, 0)
            + struct.pack('<9f', *identity) + struct.pack('<f', 1.0) + struct.pack('<i', -1))


def build():
    qs = quads()
    verts = b''
    tris = b''
    for k, q in enumerate(qs):
        for p in q:
            verts += vertex(p)
        base = 4 * k
        tris += struct.pack('<6H', base, base + 1, base + 2, base, base + 2, base + 3)
    nverts = 4 * len(qs)
    ntris = 2 * len(qs)
    radius = (HALF[0] ** 2 + HALF[1] ** 2 + HALF[2] ** 2) ** 0.5 + 1.0

    root = av_object(0) + struct.pack('<I', 1) + struct.pack('<i', 1)
    shape = (av_object(1) + struct.pack('<4f', 0, 0, 0, radius) + struct.pack('<iii', -1, 2, 3)
             + bytes.fromhex('05024300 00b00100'.replace(' ', ''))
             + struct.pack('<IHI', ntris, nverts, len(verts) + len(tris)) + verts + tris)
    shader = bytearray(SHADER)
    struct.pack_into('<i', shader, 0, 2)                       # name: ""
    struct.pack_into('<I', shader, 16, struct.unpack_from('<I', shader, 16)[0] | 0x10)  # double sided
    struct.pack_into('<4f', shader, 95, *COLOR)
    struct.pack_into('<f', shader, 111, COLOR_SCALE)
    alpha = bytearray(ALPHA)
    struct.pack_into('<i', alpha, 0, 2)

    types = ['BSFadeNode', 'BSTriShape', 'BSEffectShaderProperty', 'NiAlphaProperty']
    blocks = [(0, root), (1, shape), (2, bytes(shader)), (3, bytes(alpha))]
    strings = ['ZoneFrame', 'Frame:0', '']

    out = b'Gamebryo File Format, Version 20.2.0.7\n'
    out += struct.pack('<IBII', 0x14020007, 1, 12, len(blocks)) + struct.pack('<I', 130)
    for s in (b'WeaponVise\0', b'\0', b'\0', b'\0'):
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


if __name__ == '__main__':
    data = build()
    with open(sys.argv[1] if len(sys.argv) > 1 else 'ZoneFrame.nif', 'wb') as f:
        f.write(data)
    print('wrote', len(data), 'bytes')
