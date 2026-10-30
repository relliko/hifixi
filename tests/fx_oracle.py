"""fx_oracle: what research/ffxi-re/tools/effects_geo.py and effects_play.py make of the game's effects, written for
tests/fx_geo_test.cpp and tests/fx_render_test.cpp to compare src/fx against.

    python tests/fx_oracle.py geo <out.txt> <dat> [<dat>...]
        Every geometry chunk (0x1F D3M, 0x21 D3A, 0x25 morph) of each DAT, parsed by effects_geo: one line each,
        'chunk <dat> <offset> <kind> <name hex> ...' with its counts and sums (below), or 'bad <dat> <offset> <why>'.

    python tests/fx_oracle.py play <out.bin> <frame> [--action KIND ID] [--dat DAT --gen NAME...] [--camera C]
        An effect run by effects_play to <frame>, its elements as drawn at that frame (far to near), each with its
        model's chunk and its textures' chunks, and the picture effects_play's rasteriser makes of them on black (no
        ground, no actors), W x H RGB8 at the end. Classes exported: D3M (0x1F; MMB models left out), D3A (0x21; not
        lens flares), morph (0x25), rings; the Vu classes are left out of both.

The .bin: 'FXO1', u32 W, H, f32 F (the focal length in pixels: the camera's 2 atan2(192, 350) field of view), u32
elements, u32 blobs; each blob u32 size + bytes; each element: u32 class (0 D3M, 2 D3A, 4 morph, 5 ring), u32
TEXTUREFACTOR (0xAARRGGBB), u32 letter, u32 flags, f32 uv u v (TEXTURE0's offset as the player applies it), u32 frame, f32 M (3x3, rows), f32 T (3), u32 the
model's blob (0xFFFFFFFF none), u32 textures, u32 blob each (0xFFFFFFFF not found), f32 morph weights x 5, ring: u32
segments, rings, f32 radii x 4, u32 colours x 4 (0xAABBGGRR as the element keeps them); then W x H x 3 bytes.
"""
import math
import os
import struct
import sys

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'research', 'ffxi-re', 'tools')
sys.path.insert(0, os.path.abspath(TOOLS))
import numpy as np  # noqa: E402
import effects_geo as GEO  # noqa: E402
import effects_sched as SCH  # noqa: E402

NONE = 0xFFFFFFFF


def fsum(a):
    a = np.asarray(a, np.float64)
    a = a[np.isfinite(a)]
    return float(a.sum()) if a.size else 0.0


def geo(out, dats):
    lines = []
    for rel in dats:
        path = os.path.join(SCH.FFXI, rel.replace('/', os.sep)) if not os.path.isabs(rel) else rel
        dat = open(path, 'rb').read()
        for off, name, kind, size, folder, span in GEO.walk(dat):
            if kind not in (0x1F, 0x21, 0x25):
                continue
            d = dat[off + 16:off + size]
            nh = name.encode('latin1').hex()
            try:
                g = GEO.PARSERS[kind](d)
            except Exception as e:  # noqa: BLE001
                lines.append('bad %s %d %s' % (rel, off, str(e).replace(' ', '_')))
                continue
            if kind == 0x1F:
                col = np.asarray(g['col'], np.uint64)
                runs = ' '.join('%d:%d:%s' % (s * 3, n * 3, (t or '-').encode('latin1').hex() if t else '-') for t, s, n in g['runs'])
                lines.append('chunk %s %d 31 %s %d %d %d %.6g %.6g %.6g %d %s' % (
                    rel, off, nh, g['kind'], g['ntri'], len(g['textures']), fsum(g['pos']), fsum(g['nrm']), fsum(g['uv']),
                    int(col.sum()), runs or '-'))
            elif kind == 0x21:
                fr = ' '.join('%d:%.6g:%.6g:%d' % (len(f['pos']), fsum(f['pos']), fsum(f['uv']), int(np.asarray(f['col'], np.uint64).sum()))
                              for f in g['frames'])
                lines.append('chunk %s %d 33 %s %d %d %d %d %s' % (rel, off, nh, g['units'], g['nframes'], g['flare'], len(g['frames']), fr or '-'))
            else:
                lines.append('chunk %s %d 37 %s %d %d %d %d %.6g %.6g %.6g %d' % (
                    rel, off, nh, g['K'], g['P'], g['N'], g['ntri'], fsum(g['targets'][0]['pos']), fsum(g['targets'][-1]['nrm']),
                    fsum(g['uv']), int(np.asarray(g['col'], np.uint64).sum())))
    with open(out, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print('%d chunks -> %s' % (len(lines), out))


def play(out, frame, argv):
    import argparse
    import effects_play as P
    ap = argparse.ArgumentParser()
    ap.add_argument('--action', nargs=2)
    ap.add_argument('--race', type=int, default=1)
    ap.add_argument('--dat')
    ap.add_argument('--gen', nargs='+')
    ap.add_argument('--camera', default='side')
    ap.add_argument('--size', default='320x180')
    a = ap.parse_args(argv)
    P.RNG = P.Rng(1)
    W, H = (int(x) for x in a.size.split('x'))
    ctx = {0x27: 1, 0x28: 1, 0x29: 0, 0x2B: 0, 0x38: 1}
    if a.action:
        fid, sched, own, text, c = P.resolve_action(a.action[0], a.action[1], a.race)
        ctx.update(c)
        path = P.SCH.file_table().get(fid)
    else:
        path, sched, own = P.dat_path(a.dat), 'main', []
    P.LIB.load(path)
    for p in own:
        P.LIB.load(p, 'A')
    P.LIB.load(P.dat_path('ROM/0/0'))
    eye, at = P.CAMERAS[a.camera]
    world = P.World(P.Camera(eye, at, W, H), 0.5)
    world.context = ctx
    if a.gen:
        df = P.LIB.files[path]
        for nm in a.gen:
            g = P.Gen(P.cached('gen', P.Res(df, df.by[(5, nm.ljust(4)[:4])][0]), P.GenRes), None, None, world)
            world.gens.append(g)
            g.start(0.0)
        pts = [np.array(g.g.el['pos']) @ g.attach[:3, :3] + g.attach[3, :3] for g in world.gens]
        c = np.mean(pts, 0)
        world.cam = P.Camera(c + np.array([-9.5, -5.0, -9.5]), c + np.array([0, -2.6, 0]), W, H)
    else:
        df = P.LIB.files[path]
        root_res = P.Res(df, df.by[(7, sched)][0])
        A = P.Actor('A', (0, 0, 0), 0.0)
        B = P.Actor('B', (8, 0, 0), math.pi)
        world.actors = [A, B]
        world.tasks.append(P.Task(root_res, A, B, world, label=sched))
    for fr in range(frame + 1):
        P.FRAME[0] = fr
        world.step()
    cam = world.cam
    blobs, index = [], {}

    def blob(data):
        if data is None:
            return NONE
        k = bytes(data)
        if k not in index:
            index[k] = len(blobs)
            blobs.append(k)
        return index[k]

    def tex_blob(name, res):
        how, data = GEO.find_texture(name, res.span, res.df.texs())
        if data is None:
            for p in P.LIB.order:
                d2 = P.LIB.files[p]
                if d2 is res.df:
                    continue
                how, data = GEO.find_texture(name, (0, len(d2.dat)), d2.texs(), syst=False)
                if data is not None:
                    break
        return blob(data)

    R = P.Raster(W, H, bg=(0, 0, 0))
    vis = [e for e in world.elems if e.alive and e.visible]
    vis.sort(key=lambda e: -e.key)
    recs = []
    for e in vis:
        m = e.model
        cls = None
        if e.cls == 'd3m' and m is not None and m.type == 0x1F:
            cls = 0
        elif e.cls in ('d3m', 'morph') and m is not None and m.type == 0x25:
            cls = 4
        elif e.cls == 'd3a' and m is not None and m.type == 0x21 and not m.flare:
            cls = 2
        elif e.cls == 'ring' and e.ring is not None:
            cls = 5
        if cls is None:
            continue
        tf = P.element_tf(e)
        tfw = tf[3] << 24 | tf[0] << 16 | tf[1] << 8 | tf[2]
        mb, texs = NONE, []
        if cls in (0, 2, 4):
            mb = blob(m.res.data[16:])
            names = [t for t in GEO.PARSERS[m.type](m.res.data[16:])['textures']]
            texs = [tex_blob(t, m.res) for t in names]
        uvoff = (e.uv[0] if e.uvbits & 1 else 0.0, e.uv[1] if e.uvbits & 2 else 0.0)
        rec = struct.pack('<IIIIffI', cls, tfw, e.letter & 0xFFFF, e.flags & 0xFFFFFFFF, float(uvoff[0]), float(uvoff[1]), int(e.frame))
        rec += struct.pack('<9f', *[float(x) for x in np.asarray(e.M).reshape(-1)])
        rec += struct.pack('<3f', *[float(x) for x in e.view])
        rec += struct.pack('<II', mb, len(texs)) + b''.join(struct.pack('<I', t) for t in texs)
        w = np.zeros(5)
        if e.morph_w is not None:
            w[:min(5, len(e.morph_w))] = np.asarray(e.morph_w)[:5]
        rec += struct.pack('<5f', *[float(x) for x in w])
        r = e.ring or {}
        radii = list(r.get('radii', []))[:4] + [0.0] * 4
        cols = [int(cb[3]) << 24 | int(cb[2]) << 16 | int(cb[1]) << 8 | int(cb[0]) for cb in list(r.get('cols', []))[:4]] + [0] * 4
        rec += struct.pack('<II4f4I', int(r.get('seg', 0)), int(r.get('rings', 0)), *[float(x) for x in radii[:4]], *cols[:4])
        recs.append(rec)
        # (the reference, as effects_play's render draws this element)
        zw = bool(e.flags & 0x1000) and e.cls not in ('d3a', 'vuanm')
        uvoff = (e.uv[0] if e.uvbits & 1 else 0.0, e.uv[1] if e.uvbits & 2 else 0.0)
        for V, uv, col, tex, stage in P.element_tris(e, world):
            R.tri(cam, V, uv, col, tex, tf, e.letter, zw, stage, uvoff)
    img = (np.clip(R.img, 0, 1) * 255 + 0.5).astype(np.uint8)
    with open(out, 'wb') as f:
        f.write(b'FXO1' + struct.pack('<IIfII', W, H, float(cam.F), len(recs), len(blobs)))
        for b in blobs:
            f.write(struct.pack('<I', len(b)) + b)
        for r in recs:
            f.write(r)
        f.write(img.tobytes())
    print('%d elements (%d of them drawn here), %d chunks, %d triangles -> %s' % (len(vis), len(recs), len(blobs), R.tris, out))


if __name__ == '__main__':
    if len(sys.argv) >= 3 and sys.argv[1] == 'geo':
        geo(sys.argv[2], sys.argv[3:])
    elif len(sys.argv) >= 4 and sys.argv[1] == 'play':
        play(sys.argv[2], int(sys.argv[3]), sys.argv[4:])
    else:
        sys.exit(__doc__)
