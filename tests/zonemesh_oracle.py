"""The zone's meshes, textures and normal maps as HiFi XI's native zone renderer loads them (hifixi/src/bake/zonemesh.cpp),
read independently in Python as its test's oracle (tests/zonemesh_test.cpp). notes-zone.md has the formats.

    python zonemesh_oracle.py <map> [<map> ...]     one line of figures and checksums per map number

A map number is a zone, or a second map's number (interiors: 412 is the Fishermen's Guild); its DAT is file id
map + 100 (+ 0x144F7 from 600), its bump DAT map + 0x9B97 (+ 0x14817 from 600).

What's kept, in this order:
  - models: each MMB chunk (type 0x2E, unscrambled), the first of its name (header bytes 16-31), with at least one piece
    that has a triangle; kind = header byte 4 (1: 36-byte vertices, 2: 48-byte, swaying);
  - pieces: in file order; kinds 1 and 3 are triangle strips cut into triangles without the degenerate ones (the
    winding kept: odd ones swapped), kind 2 triangle lists; a triangle with a corner past the vertices is dropped;
    each kept piece's vertices all appended (48 bytes: position, normal, colour, UV, sway vector), its indices made
    absolute;
  - textures: those the kept pieces name, in first-use order, the first chunk of that name: 0xA1 DXT3 (blocks as
    stored) or 0xB1 palettized (expanded to BGRA through its 256-entry palette); none for a name with neither;
  - normal maps (03ADC0): the bump DAT's first height map (chunk 0x5D) whose name's last 8 characters are the
    texture's last 8, made into B8G8R8A8 texels as 01E0A0 makes them, in first-use order;
  - placements: the MZB's object records naming a kept model, finite: flags 1 alpha-tested ('_'), 2 mirrored
    (scale x * scale z < 0), 4 clamped ('#').
"""
import math
import os
import struct
import sys
import zlib

sys.path.insert(0, r'C:\code\LSB\reshade-worldfx\tools')
import zone_collision as zc  # noqa: E402
import bake_shadow_geo as bsg  # noqa: E402


def dat_path(fid, ffxi=zc.FFXI):
    vt = open(ffxi + r'\VTABLE.DAT', 'rb').read()
    if fid < len(vt) and vt[fid] == 1:
        ft = open(ffxi + r'\FTABLE.DAT', 'rb').read()
        v = struct.unpack_from('<H', ft, fid * 2)[0]
        return r'%s\ROM\%d\%d.DAT' % (ffxi, v >> 7, v & 0x7F)
    for n in range(2, 10):
        try:
            vtn = open(r'%s\ROM%d\VTABLE%d.DAT' % (ffxi, n, n), 'rb').read()
            ftn = open(r'%s\ROM%d\FTABLE%d.DAT' % (ffxi, n, n), 'rb').read()
        except OSError:
            continue
        if fid < len(vtn) and vtn[fid] == n:
            v = struct.unpack_from('<H', ftn, fid * 2)[0]
            return r'%s\ROM%d\%d\%d.DAT' % (ffxi, n, v >> 7, v & 0x7F)
    return None


def map_dat(m):
    return m + 100 if m < 600 else m + 0x144F7


def bump_dat(m):
    return m + 0x9B97 if m < 600 else m + 0x14817


def pieces(b):
    """[(texture name, the count dword, vertex bytes, index list)] of an unscrambled MMB, every group."""
    stride = 48 if b[4] == 2 else 36
    n = struct.unpack_from('<I', b, 32)[0]
    ptr = struct.unpack_from('<I', b, 60)[0]
    if ptr == 0:
        offs = [o for o in struct.unpack_from('<8I', b, 64) if o][:n] if n else [64]
    else:
        offs, o = [ptr], 64
        while o < ptr:
            v = struct.unpack_from('<I', b, o)[0]
            if v and v != ptr:
                offs.append(v)
            o += 4
    out = []
    for po in offs:
        if po + 32 > len(b):
            continue
        count = struct.unpack_from('<I', b, po)[0]
        if count > 100:
            continue
        m = po + 32
        for _ in range(count):
            if m + 20 > len(b):
                break
            tex, w = bytes(b[m:m + 16]), struct.unpack_from('<I', b, m + 16)[0]
            vc = w & 0xFFFFFFF
            end = m + 20 + vc * stride
            if end + 4 > len(b):
                break
            ic = struct.unpack_from('<H', b, end)[0]
            if end + 4 + ic * 2 > len(b):
                break
            idx = list(struct.unpack_from('<%dH' % ic, b, end + 4))
            out.append((tex, w, bytes(b[m + 20:end]), idx))
            m = end + 4 + ic * 2 + (2 if ic & 1 else 0)
    return out


def triangles(idx, vc, strip):
    out = []
    if strip:
        for k in range(len(idx) - 2):
            a, b, c = idx[k], idx[k + 1], idx[k + 2]
            t = (b, a, c) if k & 1 else (a, b, c)
            if t[0] != t[1] and t[1] != t[2] and t[0] != t[2] and max(t) < vc:
                out.append(t)
    else:
        for k in range(0, len(idx) - 2, 3):
            t = (idx[k], idx[k + 1], idx[k + 2])
            if t[0] != t[1] and t[1] != t[2] and t[0] != t[2] and max(t) < vc:
                out.append(t)
    return out


def vertex48(raw, i, kind):
    if kind == 2:
        f = struct.unpack_from('<9fI2f', raw, i * 48)
        pos, sway, nrm, col, uv = f[0:3], f[3:6], f[6:9], f[9], f[10:12]
    else:
        f = struct.unpack_from('<6fI2f', raw, i * 36)
        pos, nrm, col, uv, sway = f[0:3], f[3:6], f[6], f[7:9], (0.0, 0.0, 0.0)
    return struct.pack('<6fI2f3f', *pos, *nrm, col, *uv, *sway)


def sobel(h, w, ht):
    """01E0A0: B8G8R8A8 texels (as u32) of a height map, wrapping at the edges."""
    out = []
    for y in range(ht):
        up, mid, dn = ((y - 1) % ht) * w, y * w, ((y + 1) % ht) * w
        for x in range(w):
            l, r = (x - 1) % w, (x + 1) % w
            gy = (h[up + l] + 2 * h[up + x] + h[up + r]) - (h[dn + l] + 2 * h[dn + x] + h[dn + r])
            gx = (h[up + l] + 2 * h[mid + l] + h[dn + l]) - (h[up + r] + 2 * h[mid + r] + h[dn + r])
            k = 127.0 / math.sqrt(float(gx * gx + gy * gy + 0xFE01))
            R = int(gy * k) + 0x80
            G = int(gx * k) + 0x80
            B = 0x80 - int(-255.0 * k)
            out.append(((R & 0xFF) << 16) | ((G & 0xFF) << 8) | (B & 0xFF))
    return out


def load(m):
    p = dat_path(map_dat(m))
    if p is None:
        return None
    dat = open(p, 'rb').read()
    models, seen = [], set()
    for k, _, body in bsg.chunks(dat):
        if k != 0x2E or len(body) <= 64:
            continue
        b = bsg.decrypt_mmb(body)
        name = bytes(b[16:32])
        if name in seen:
            continue
        try:
            ps = pieces(b)
        except struct.error:
            continue
        kind = b[4]
        kept = []
        for tex, w, raw, idx in ps:
            vc = w & 0xFFFFFFF
            tri = triangles(idx, vc, kind != 2)
            if tri:
                kept.append((tex, w >> 28, [vertex48(raw, i, kind) for i in range(vc)], tri))
        if kept:
            seen.add(name)
            models.append((name, kind, kept))
    imgs = {}
    for k, _, b in bsg.chunks(dat):
        if k != 0x20 or len(b) < 69 or b[0] not in (0xA1, 0xB1):
            continue
        name = bytes(b[1:17])
        if name in imgs:
            continue
        w, h = struct.unpack_from('<ii', b, 21)
        if w <= 0 or h <= 0:
            continue
        if b[0] == 0xA1:
            if bytes(b[57:61]) != b'3TXD' or w % 4 or h % 4 or 69 + (w // 4) * (h // 4) * 16 > len(b):
                continue
            imgs[name] = (w, h, 0, bytes(b[69:69 + (w // 4) * (h // 4) * 16]))
        else:
            if 61 + 1024 + w * h > len(b):
                continue
            pal = b[61:61 + 1024]
            px = b[61 + 1024:61 + 1024 + w * h]
            imgs[name] = (w, h, 1, b''.join(bytes(pal[4 * i:4 * i + 4]) for i in px))
    bp = dat_path(bump_dat(m))
    bumps = []
    if bp is not None:
        for k, _, b in bsg.chunks(open(bp, 'rb').read()):
            if k == 0x5D and len(b) >= 0x20 and bytes(b[0:2]) == b'bp':
                w, h = struct.unpack_from('<HH', b, 4)
                if w and h and 0x20 + w * h <= len(b):
                    bumps.append((bytes(b[0x18:0x20]), w, h, bytes(b[0x20:0x20 + w * h])))
    return models, imgs, bumps, dat


def figures(m):
    got = load(m)
    if got is None:
        return '%d none' % m
    models, imgs, bumps, dat = got
    verts, inds = [], []
    tex_order, nm_order = [], []
    tex_index, nm_index = {}, {}
    n_pieces = missing = with_map = 0
    kinds = {1: 0, 2: 0}
    for name, kind, ps in models:
        kinds[kind] = kinds.get(kind, 0) + 1
        for tex, bits, vs, tri in ps:
            base = len(verts)
            verts.extend(vs)
            for t in tri:
                inds.extend(i + base for i in t)
            n_pieces += 1
            if tex in imgs:
                if tex not in tex_index:
                    tex_index[tex] = len(tex_order)
                    tex_order.append(tex)
                for bname, w, h, data in bumps:
                    if bname == tex[8:]:
                        with_map += 1
                        if bname not in nm_index:
                            nm_index[bname] = len(nm_order)
                            nm_order.append((bname, w, h, data))
                        break
            else:
                missing += 1
    crc_v = zlib.crc32(b''.join(verts))
    crc_i = zlib.crc32(struct.pack('<%dI' % len(inds), *inds))
    crc_t = 0
    for t in tex_order:
        crc_t = zlib.crc32(imgs[t][3], crc_t)
    crc_n = 0
    for bname, w, h, data in nm_order:
        crc_n = zlib.crc32(struct.pack('<%dI' % (w * h), *sobel(data, w, h)), crc_n)
    # placements
    mz = zc.find_chunk(dat, 0x1C)
    names = {n for n, _, _ in models}
    placed = tested = mirrored = 0
    if mz is not None:
        zc.decrypt_mzb(mz)
        n = struct.unpack_from('<I', mz, 4)[0] & 0xFFFFFF
        for i in range(n):
            a = 0x20 + i * 0x64
            if a + 0x64 > len(mz):
                break
            name = bytes(mz[a:a + 16])
            t = struct.unpack_from('<9f', mz, a + 16)
            if name not in names or not all(math.isfinite(v) for v in t):
                continue
            placed += 1
            tested += name[:1] == b'_'
            mirrored += t[6] * t[8] < 0
    return ('%d models %d kind1 %d kind2 %d pieces %d vertices %d triangles %d textures %d missing %d normalmaps %d mapped %d '
            'placed %d tested %d mirrored %d crc %08x %08x %08x %08x') % (
        m, len(models), kinds.get(1, 0), kinds.get(2, 0), n_pieces, len(verts), len(inds) // 3, len(tex_order), missing,
        len(nm_order), with_map, placed, tested, mirrored, crc_v, crc_i, crc_t, crc_n)


if __name__ == '__main__':
    for a in sys.argv[1:]:
        print(figures(int(a)))
        sys.stdout.flush()
