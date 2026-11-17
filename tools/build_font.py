"""Writes the in-world text font: data/Font.dds (glyph atlas) and data/Font.nif (one quad per glyph and colour).

usage: python tools/build_font.py        (needs Pillow and DejaVu Sans Mono; the outputs are committed)
The DLL loads Meshes\\WeaponVise\\Font.nif, clones the node "<colour><code point hex>" (e.g. "g0410") per letter
and lines them up above the part the hand points at (monospace: every letter advances by ADVANCE of its height).
The quad of a glyph spans x 0..ADVANCE, z 0..1 (baseline at z 0.22), in the XZ plane, double sided.
"""
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from build_frame_nif import SHADER, ALPHA, av_object   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf'
CELL = 64
GRID = 16
SIZE = CELL * GRID
ADVANCE = 0.62
CHARS = [chr(c) for c in range(33, 127)] + [chr(c) for c in range(0x410, 0x450)] + ['Ё', 'ё', '№', '…']
COLORS = {'w': (1.0, 1.0, 1.0, 1.0), 'g': (0.35, 1.0, 0.4, 1.0), 'r': (1.0, 0.3, 0.22, 1.0), 'y': (1.0, 0.85, 0.3, 1.0)}
TEXTURE = 'textures\\WeaponVise\\Font.dds'


def atlas():
    img = Image.new('L', (SIZE, SIZE), 0)
    draw = ImageDraw.Draw(img)
    font = ImageFont.truetype(FONT, int(CELL * 0.78))
    for i, ch in enumerate(CHARS):
        cx, cy = (i % GRID) * CELL, (i // GRID) * CELL
        cell_w = int(CELL * ADVANCE)
        w = draw.textlength(ch, font=font)
        draw.text((cx + (cell_w - w) / 2, cy + CELL * 0.78), ch, fill=255, font=font, anchor='ls')
    # white glyphs, alpha = coverage (the shader adds the colour on top of the scene)
    levels = []
    size = SIZE
    while size >= 4:
        levels.append(img if size == SIZE else img.resize((size, size), Image.LANCZOS))
        size //= 2
    bgra = bytearray()
    for level in levels:
        for v in level.tobytes():
            bgra += bytes((v, v, v, v))
    header = struct.pack('<4sIIIIIII', b'DDS ', 124, 0x1007 | 0x8 | 0x20000, SIZE, SIZE, SIZE * 4, 0, len(levels))
    header += b'\0' * 44
    header += struct.pack('<II4sIIIII', 32, 0x41, b'\0\0\0\0', 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    header += struct.pack('<IIIII', 0x1000 | 0x400008, 0, 0, 0, 0)
    return header + bytes(bgra)


def vertex(x, z, u, v):
    return (struct.pack('<eeee', x, 0.0, z, 1.0) + struct.pack('<ee', u, v)
            + bytes([0x7f, 0x00, 0x7f, 0x7f]) + bytes([0xff, 0x7f, 0x7f, 0x7f]))


def shader_for(rgba):
    s = bytearray(SHADER)
    struct.pack_into('<i', s, 0, 2)                                                   # name ""
    struct.pack_into('<I', s, 16, struct.unpack_from('<I', s, 16)[0] | 0x10)          # double sided
    old_len = struct.unpack_from('<I', s, 36)[0]
    tex = TEXTURE.encode()
    s[36:40 + old_len] = struct.pack('<I', len(tex)) + tex
    delta = len(tex) - old_len
    struct.pack_into('<4f', s, 95 + delta, *rgba)
    struct.pack_into('<f', s, 111 + delta, 1.3)
    return bytes(s)


def build_nif():
    names = ['Font', '']
    blocks = []      # (type index, bytes)
    types = ['BSFadeNode', 'NiNode', 'BSTriShape', 'BSEffectShaderProperty', 'NiAlphaProperty']

    def name_index(n):
        names.append(n)
        return len(names) - 1

    color_keys = list(COLORS)
    nglyph = len(CHARS) * len(color_keys)
    # block layout: 0 root, 1..4 one node per colour (a node shows at most 256 children to the DLL),
    # then per glyph a node and a shape, then shaders, then alpha
    groups = len(color_keys)
    first_shader = 1 + groups + 2 * nglyph
    alpha_index = first_shader + len(color_keys)
    children = [[] for _ in color_keys]
    glyph_blocks = []
    for ci, ck in enumerate(color_keys):
        for gi, ch in enumerate(CHARS):
            node_index = 1 + groups + 2 * (ci * len(CHARS) + gi)
            children[ci].append(node_index)
            u0 = (gi % GRID) * CELL / SIZE
            v0 = (gi // GRID) * CELL / SIZE
            u1 = u0 + CELL * ADVANCE / SIZE
            v1 = v0 + CELL / SIZE
            verts = (vertex(0, 0, u0, v1) + vertex(ADVANCE, 0, u1, v1)
                     + vertex(ADVANCE, 1, u1, v0) + vertex(0, 1, u0, v0))
            tris = struct.pack('<6H', 0, 1, 2, 0, 2, 3)
            node = (av_object(name_index('%s%04X' % (ck, ord(ch)))) + struct.pack('<I', 1)
                    + struct.pack('<i', node_index + 1))
            shape = (av_object(1) + struct.pack('<4f', ADVANCE / 2, 0, 0.5, 0.6)
                     + struct.pack('<iii', -1, first_shader + ci, alpha_index)
                     + bytes.fromhex('0502430000b00100')
                     + struct.pack('<IHI', 2, 4, len(verts) + len(tris)) + verts + tris)
            glyph_blocks += [(1, node), (2, shape)]
    root = av_object(0) + struct.pack('<I', groups) + b''.join(struct.pack('<i', 1 + g) for g in range(groups))
    group_blocks = []
    for ci, ck in enumerate(color_keys):
        kids = children[ci]
        group_blocks.append((1, av_object(name_index('colour_' + ck)) + struct.pack('<I', len(kids))
                             + b''.join(struct.pack('<i', c) for c in kids)))
    blocks = [(0, root)] + group_blocks + glyph_blocks
    for ck in color_keys:
        blocks.append((3, shader_for(COLORS[ck])))
    alpha = bytearray(ALPHA)
    struct.pack_into('<i', alpha, 0, 1)
    blocks.append((4, bytes(alpha)))
    names_b = [n.encode('latin1') for n in names]

    out = b'Gamebryo File Format, Version 20.2.0.7\n'
    out += struct.pack('<IBII', 0x14020007, 1, 12, len(blocks)) + struct.pack('<I', 130)
    for s in (b'WeaponVise\0', b'\0', b'\0', b'\0'):
        out += bytes([len(s)]) + s
    out += struct.pack('<H', len(types))
    for t in types:
        out += struct.pack('<I', len(t)) + t.encode()
    out += b''.join(struct.pack('<H', t) for t, _ in blocks)
    out += b''.join(struct.pack('<I', len(b)) for _, b in blocks)
    out += struct.pack('<II', len(names_b), max(len(s) for s in names_b))
    for s in names_b:
        out += struct.pack('<I', len(s)) + s
    out += struct.pack('<I', 0)
    out += b''.join(b for _, b in blocks)
    out += struct.pack('<Ii', 1, 0)
    return out


if __name__ == '__main__':
    os.makedirs(os.path.join(ROOT, 'data'), exist_ok=True)
    with open(os.path.join(ROOT, 'data', 'Font.dds'), 'wb') as f:
        f.write(atlas())
    with open(os.path.join(ROOT, 'data', 'Font.nif'), 'wb') as f:
        f.write(build_nif())
    print('font:', len(CHARS), 'glyphs x', len(COLORS), 'colours')
