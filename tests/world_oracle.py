"""world_oracle: what the game's sky dome (03D640, read from the disassembly: research/ffxi-re/client-notes.md "Sky") and
the names' font (research/ffxi-re/tools/ui_sprites.py, chartex.py) make of the game's own data, written as text for
tests/world_test.cpp to compare src/world/dome and font against.

    python tests/world_oracle.py <out.txt>

Domes, from the weather keyframes of a few zones (zone_weather.py; every record with a dome, segments > 2):
  'dome <zone> <weather> <time> <segments> <8 colours as D3DCOLOR hex> <8 heights as float32 bits>'
  then per strip 'strip <band> <e_lo bits> <e_hi bits> <c_lo hex> <c_hi hex> <x y z of vertex 2 (k = 1, the lower edge) bits>'
Font: 'page <name hex> <w> <h> <fnv1a-64 of the RGBA rows>' for both name pages, 'set <name hex> <textures> <records>
<bytes used>', then per record 'rec <i> <parts>' and per part 'part <quad x8> <w h u v> <flip> <colours hex> <blend hex>
<texture name hex>'.

The dome's arithmetic here is the x87 code's as read (03D66F-03D8F2): strips n = max(1, ftol((h1 - h0) x 12.048193)) a
band (none when negative); f = float32(j / n); 1 - f stored as float32 for 03DFE0; e = float32(((1 - f) h0 + f h1) x
1.570796); 03DFE0 per byte: ftol(byte x the float32), 0 unless above 0; 03E1D0 adds the bytes; the radii float32(cos e x r),
the heights float32(-(sin e x r)); vertex k at a = k x 6.283 / segments: x float32(sin a x R), z float32(cos a x R).
"""
import math
import os
import struct
import sys

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'research', 'ffxi-re', 'tools')
sys.path.insert(0, os.path.abspath(TOOLS))
import chartex  # noqa: E402
import ui_sprites  # noqa: E402
import zone_weather  # noqa: E402

ZONES = [100, 101, 230, 103, 104, 2, 24, 51, 110, 125]
RADIUS = 0.800000011920929 * (65535.0 - 0.10000000149011612)


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def bits(x):
    return '%08X' % struct.unpack('<I', struct.pack('<f', x))[0]


def fnv(b):
    h = 0xCBF29CE484222325
    for x in b:
        h ^= x
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def scale(c, f):
    out = 0
    for k in range(4):
        v = ((c >> (8 * k)) & 0xFF) * f
        out |= ((int(v) & 0xFF) if v > 0 else 0) << (8 * k)
    return out


def add(a, b):
    return sum(((((a >> k) & 0xFF) + ((b >> k) & 0xFF)) & 0xFF) << k for k in (0, 8, 16, 24))


def dome(colours, heights, segments, radius):
    out = []
    for b in range(7):
        h0, h1 = heights[b], heights[b + 1]
        n = int((h1 - h0) * 12.048192977905273)
        if n == 0:
            n = 1
        if n < 0:
            continue
        for j in range(n):
            f1, f2 = f32(j / float(n)), f32((j + 1) / float(n))
            e1 = f32(((1.0 - f1) * h0 + f1 * h1) * 1.570796012878418)
            e2 = f32(((1.0 - f2) * h0 + f2 * h1) * 1.570796012878418)
            lo = add(scale(colours[b], f32(1.0 - f1)), scale(colours[b + 1], f1))
            hi = add(scale(colours[b], f32(1.0 - f2)), scale(colours[b + 1], f2))
            r_lo = f32(math.cos(e1) * radius)
            y_lo = -f32(math.sin(e1) * radius)
            a = 1 * 6.2829999923706055 / segments
            v = (f32(math.sin(a) * r_lo), y_lo, f32(math.cos(a) * r_lo))
            out.append((b, e1, e2, lo, hi, v))
    return out


def domes(lines):
    radius = f32(f32(RADIUS))
    for zone in ZONES:
        try:
            ws = zone_weather.weathers(zone)
        except Exception:
            continue
        for (env, weather), times in sorted(ws.items()):
            for time, body in sorted(times.items()):
                if len(body) < 0xB0:
                    continue
                segments = body[0x4C + 0x12]
                if segments <= 2:
                    continue
                cols = []
                for i in range(8):
                    r, g, b, a = body[0x6C + 4 * i:0x70 + 4 * i]
                    cols.append(a << 24 | r << 16 | g << 8 | b)
                hs = list(struct.unpack_from('<8f', body, 0x8C))
                if any(math.isnan(h) for h in hs):
                    continue
                lines.append('dome %d %s %s %d %s %s' % (zone, weather, time, segments, ' '.join('%08X' % c for c in cols), ' '.join(bits(h) for h in hs)))
                for b, e1, e2, lo, hi, v in dome(cols, hs, segments, radius):
                    lines.append('strip %d %s %s %08X %08X %s' % (b, bits(e1), bits(e2), lo, hi, ' '.join(bits(x) for x in v)))


def font(lines):
    path = ui_sprites.rom_path(0x9A76)
    dat = open(path, 'rb').read()
    pages = {b'font    font    ': None, b'menu    ustatshd': None}
    for o, kind, size in ui_sprites.chunks(dat):
        if kind == 0x20:
            name = dat[o + 17:o + 33].lower()
            if name in pages and pages[name] is None:
                t = chartex.Tex(dat[o + 16:o + size])
                img = t.rgba() if t.ok else None
                pages[name] = img
                lines.append('page %s %d %d %016X' % (name.hex(), img.shape[1], img.shape[0], fnv(img.tobytes())))
        elif kind == 0x31 and dat[o + 16:o + 32].lower() == b'font    fontshp ':
            name, texs, recs, used = ui_sprites.sprite_set(dat, o, size)
            lines.append('set %s %d %d %d' % (name.hex(), len(texs), len(recs), used - 16))
            for i, parts in enumerate(recs):
                lines.append('rec %d %d' % (i, len(parts)))
                for p in parts:
                    lines.append('part %s %d %d %d %d %d %s %s %s' % (' '.join('%d %d' % q for q in p['quad']), p['w'], p['h'], p['u'], p['v'], p['flip'],
                                                                   bytes(sum((list(c) for c in p['cols']), [])).hex(), bytes(p['blend']).hex(), p['tex'].hex()))


def main():
    lines = []
    domes(lines)
    font(lines)
    with open(sys.argv[1], 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print('world_oracle: %d domes, %d strips, %d lines in all' % (sum(1 for l in lines if l.startswith('dome ')), sum(1 for l in lines if l.startswith('strip ')), len(lines)))


if __name__ == '__main__':
    main()
