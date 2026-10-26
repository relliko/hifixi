"""chars_oracle: what research/ffxi-re/tools/chars.py and chartex.py make of some DATs' character meshes (Os2) and
textures (0x20), written as text for tests/chars_data_test.cpp to compare src/chars/os2 and tex against.

    python tests/chars_oracle.py <out.txt>

Per mesh: 'mesh <dat> <chunk index> <name hex> n1 n2 mirrored'; its vertices 'v <bones...> <float32 bits...>'; its
triangles 't i0 i1 i2 <uv bits x6> <texture name hex or -> <material colour, kind, light bits or ->'.
Per texture: 'tex <dat> <chunk index> <name hex> w h <fnv1a-64 of the RGBA rows> ' or 'tex ... none'.
"""
import os
import random
import struct
import sys

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'research', 'ffxi-re', 'tools')
sys.path.insert(0, os.path.abspath(TOOLS))
import chars  # noqa: E402
import chartex  # noqa: E402

FIXED = ['ROM2/0/52.DAT', 'ROM/148/92.DAT', 'ROM/310/0.DAT']
# Textures of every layout and bit depth found in the DATs (a scan of every third folder): (DAT, chunk index)
EXTRA_TEX = [('ROM/0/36.DAT', 13), ('ROM/24/99.DAT', 0), ('ROM/288/36.DAT', 0), ('ROM/0/54.DAT', 5), ('ROM/0/18.DAT', 1),
             ('ROM/0/0.DAT', 2), ('ROM/0/0.DAT', 4), ('ROM/0/0.DAT', 106), ('ROM/177/18.DAT', 0), ('ROM/180/0.DAT', 1),
             ('ROM/177/81.DAT', 1)]


def fnv(b):
    h = 0xCBF29CE484222325
    for x in b:
        h ^= x
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def fbits(x):
    return '%08X' % struct.unpack('<I', struct.pack('<f', x))[0]


def sampled(count, seed=25):
    """DATs holding an Os2 mesh, picked at random (seeded) from the ROM folders' first files."""
    rnd = random.Random(seed)
    out = []
    tries = 0
    while len(out) < count and tries < 4000:
        tries += 1
        rom = rnd.choice(['ROM', 'ROM2', 'ROM3'])
        rel = '%s/%d/%d.DAT' % (rom, rnd.randrange(0, 120), rnd.randrange(0, 128))
        path = chars.FF + '\\' + rel.replace('/', '\\')
        if not os.path.exists(path) or rel in out or rel in FIXED:
            continue
        try:
            if any(k == 0x2A for _, k, _, _ in chars.read_chunks(rel)):
                out.append(rel)
        except Exception:
            pass
    return out


def texture_line(rel, i, data):
    try:
        t = chartex.Tex(data)
        img = t.rgba() if t.ok else None
    except Exception:
        img = None
    if img is None:
        return 'tex %s %d %s none' % (rel, i, data[1:17].hex())
    return 'tex %s %d %s %d %d %016X' % (rel, i, data[1:17].hex(), img.shape[1], img.shape[0], fnv(img.tobytes()))


def main():
    lines = []
    for rel in FIXED + sampled(60):
        ch = chars.read_chunks(rel)
        for i, (name, kind, data, folder) in enumerate(ch):
            if kind == 0x2A:
                try:
                    m = chars.Mesh(name, data)
                except Exception as e:
                    lines.append('mesh %s %d %s error' % (rel, i, name.encode('latin1').hex()))
                    continue
                lines.append('mesh %s %d %s %d %d %d %d' % (rel, i, name.encode('latin1').hex(), m.n1, m.n2, int(m.mirrored), len(m.tris)))
                for k in range(m.n1 + m.n2):
                    r = m.refs[k]
                    b1 = m.bone(r[0])
                    b2 = m.bone(r[1]) if k >= m.n1 else b1
                    fl = list(m.v1[k]) if k < m.n1 else list(m.v2[k - m.n1])
                    lines.append('v %d %d %d %d %d %d %s' % (b1 + b2 + (' '.join(fbits(x) for x in fl),)))
                for t, uv, tn, mat in zip(m.tris, m.uvs, m.tex, m.mats):
                    lines.append('t %d %d %d %s %s %s' % (
                        t[0], t[1], t[2], ' '.join(fbits(x) for p in uv for x in p),
                        tn.encode('latin1').hex() if tn else '-',
                        ('%08X %d %s' % (mat['colour'], mat['kind'], fbits(mat['light']))) if mat else '-'))
            elif kind == 0x20:
                lines.append(texture_line(rel, i, data))
    for rel, i in EXTRA_TEX:
        lines.append(texture_line(rel, i, chars.read_chunks(rel)[i][2]))
    open(sys.argv[1], 'w').write('\n'.join(lines) + '\n')
    print('%d lines -> %s' % (len(lines), sys.argv[1]))


if __name__ == '__main__':
    main()
