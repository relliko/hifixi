"""The oracle for src/ui2d/fonthigh (tests/uidraw_test.cpp): the game's three text fonts read and decoded by Python alone
(the PNGs inflated with zlib and unfiltered here, not with WIC), as FNV-1a 64 hashes of what the C++ side must make.

    python tests/ui2d_fonts.py <out.txt>

Lines: 'png <file id hex> <w> <h> <hash of the B, G, R, A rows top down>' and 'moji <w> <h> <hash of the DXT3 blocks>'.
Nothing when the game's files aren't there (the test then skips).
"""
import os
import struct
import sys
import zlib

FFXI = r'C:\Games\PhoenixXI\SquareEnix\FINAL FANTASY XI'


def rom_path(fid):
    """A file id's path through VTABLE.DAT / FTABLE.DAT (research/ffxi-re/tools/ffxi_fonts.py rom_path)."""
    try:
        v = open(os.path.join(FFXI, 'VTABLE.DAT'), 'rb').read()
        f = open(os.path.join(FFXI, 'FTABLE.DAT'), 'rb').read()
    except OSError:
        return None
    if len(v) <= fid or v[fid] != 1 or len(f) < fid * 2 + 2:
        return None
    e = struct.unpack_from('<H', f, fid * 2)[0]
    return os.path.join(FFXI, 'ROM', str(e >> 7), '%d.DAT' % (e & 0x7F))


def fnv_fast(data):
    """FNV-1a 64, as src/game/uidraw.cpp's fnv."""
    h = 1469598103934665603
    m = 0xFFFFFFFFFFFFFFFF
    p = 1099511628211
    for b in data:
        h = ((h ^ b) * p) & m
    return h


def png_bgra(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n'
    o, idat, w = 8, [], 0
    while o < len(d):
        n, kind = struct.unpack_from('>I4s', d, o)
        body = d[o + 8:o + 8 + n]
        if kind == b'IHDR':
            w, h, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
            assert depth == 8 and ctype == 6 and interlace == 0, 'an 8-bit RGBA PNG'
        elif kind == b'IDAT':
            idat.append(body)
        o += 12 + n
    raw = zlib.decompress(b''.join(idat))
    stride = w * 4
    out = bytearray(stride * h)
    prev = bytearray(stride)
    at = 0
    for y in range(h):
        f = raw[at]
        line = bytearray(raw[at + 1:at + 1 + stride])
        at += 1 + stride
        if f == 1:
            for i in range(4, stride):
                line[i] = (line[i] + line[i - 4]) & 255
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride):
                line[i] = (line[i] + ((line[i - 4] if i >= 4 else 0) + prev[i]) // 2) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - 4] if i >= 4 else 0
                b = prev[i]
                c = prev[i - 4] if i >= 4 else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        # RGBA -> BGRA
        row = bytearray(line)
        row[0::4], row[2::4] = line[2::4], line[0::4]
        out[y * stride:(y + 1) * stride] = row
    return w, h, bytes(out)


def moji(path):
    s = open(path, 'rb').read()
    o = 0
    while o + 16 <= len(s):
        word = struct.unpack_from('<I', s, o + 4)[0]
        size, kind = ((word >> 7) & 0x7FFFF) * 16, word & 0x7F
        if size < 16:
            break
        p = o + 16
        if kind == 0x20 and s[p + 1:p + 17] == b'font    moji    ':
            w, h = struct.unpack_from('<ii', s, p + 17 + 4)
            q = p + 17 + 40
            return w, h, s[q + 12:q + 12 + w * h]
        o += size
    return None


def main():
    out = []
    for fid in (0x9A77, 0x9A78):
        p = rom_path(fid)
        if p and os.path.exists(p):
            w, h, px = png_bgra(p)
            out.append('png %X %d %d %d' % (fid, w, h, fnv_fast(px)))
    p = rom_path(0x9A76)
    if p and os.path.exists(p):
        m = moji(p)
        if m:
            out.append('moji %d %d %d' % (m[0], m[1], fnv_fast(m[2])))
    open(sys.argv[1], 'w').write('\n'.join(out) + '\n')


if __name__ == '__main__':
    main()
