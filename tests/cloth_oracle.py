"""cloth_oracle: the game's cloth step (FFXiMain 023290 -> 023700, 024900; research/ffxi-re/notes-actor-extras.md 1
"One step") in Python, frame by frame, for tests/cloth_test.cpp: there the dumps' own 023290 runs through HiFi XI's
model stub (src/game/charmodel) on the same DAT laid out as loaded, and each frame's vertices it hands over must be
these.

The skeleton is posed by research/ffxi-re/tools/chars.py (its bind pose) and carried along a path: every bone's
translation moved by the frame's offset, the model's move (model +0x34) the step from the last frame's. The wind
(environment +0x124 = (-1, 0, 0), strength +0x134) gusts and stops, the actor's no-wind flag (+0x17C) comes on late,
and some frames aren't pose frames ([0x48E7460] = 0: nothing stepped, the last frame's vertices drawn again). The
bone spheres (023BD0) are left out: the test zeroes the meshes' sphere counts for this run (its second run, with
them, is checked for sense only).

Each stored float is rounded to float32 where the game stores one (its x87 sums in between are taken in float64).

    python tests/cloth_oracle.py <out.bin> [dat ...]       (ROM/7/95.DAT, the San d'Orian banner; ROM/203/121.DAT, a
                                                             Gigas with two cloths of one- and two-bone vertices)

Out: per DAT: u32 name length, the name; u32 bones, cloth meshes, frames; per mesh u32 its ordinal among the DAT's
Os2 chunks, u32 vertices; per frame: the palette (bones x 16 f32), the move (3 f32), the wind's strength (f32), u32
no wind, u32 pose frame; then per mesh its vertices' positions (3 f32 each) and normals (3 f32 each) after the frame.
"""
import os
import struct
import sys

import numpy as np

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'research', 'ffxi-re', 'tools')
sys.path.insert(0, os.path.abspath(TOOLS))
import chars  # noqa: E402

FRAMES = 48
f32 = np.float32


def r(x):
    """A float stored as the game stores it"""
    return np.float32(x)


class Cloth:
    """One type-1 mesh (offsets from the chunk's data; notes-actor-extras "Format")"""

    def __init__(self, d):
        at = [struct.unpack_from('<IH', d, o) for o in (6, 0xC, 0x12, 0x18, 0x1E, 0x24)]
        self.cmd_at = at[0][0] * 2
        self.n1, self.n2 = struct.unpack_from('<2H', d, at[2][0] * 2)
        self.n = self.n1 + self.n2
        refs = d[at[3][0] * 2: at[3][0] * 2 + 4 * self.n]
        self.bone = [refs[4 * i] & 0x7F for i in range(self.n)]
        self.bone2 = [refs[4 * i + 2] & 0x7F for i in range(self.n)]
        o = at[4][0] * 2
        self.v1 = [np.array(struct.unpack_from('<3f', d, o + 12 * i), dtype=np.float32) for i in range(self.n1)]
        o += 12 * self.n1
        self.v2 = [np.array(struct.unpack_from('<8f', d, o + 32 * i), dtype=np.float32) for i in range(self.n2)]
        self.p1, self.p2, coff, _, ca, cb = struct.unpack_from('<2HI3H', d, 0x2A)
        self.k_a, self.k_b, self.damping, self.gravity, self.k = (r(x) for x in struct.unpack_from('<5f', d, 0x38))
        c = coff * 2
        self.list_a = [struct.unpack_from('<2Hf', d, c + 8 * i) for i in range(ca // 4)]
        self.list_b = [struct.unpack_from('<2Hf', d, c + 8 * (ca // 4) + 8 * i) for i in range(cb // 4)]
        self.d = d
        # work: position, previous, normal (float32)
        self.pos = np.zeros((self.n, 3), dtype=np.float32)
        self.prev = np.zeros((self.n, 3), dtype=np.float32)
        self.nrm = np.zeros((self.n, 3), dtype=np.float32)
        self.reset = True

    # 0235C0 / 023620: a vertex skinned with the palette (row vectors, translation in row 3)
    def skin(self, i, pal):
        if i < self.n1:
            x, y, z = (float(v) for v in self.v1[i])
            m = pal[self.bone[i]]
            return np.array([x * m[0][k] + y * m[1][k] + z * m[2][k] + m[3][k] for k in range(3)], dtype=np.float32)
        v = [float(t) for t in self.v2[i - self.n1]]
        a, b = pal[self.bone[i]], pal[self.bone2[i]]
        return np.array([v[0] * a[0][k] + v[2] * a[1][k] + v[4] * a[2][k] + v[6] * a[3][k] +
                         v[1] * b[0][k] + v[3] * b[1][k] + v[5] * b[2][k] + v[7] * b[3][k] for k in range(3)],
                        dtype=np.float32)

    def free(self, i):
        return self.p1 <= i < self.n1 or self.n1 + self.p2 <= i < self.n

    # 023290
    def step(self, pal, move, wind, strength, no_wind):
        if self.reset:
            for i in range(self.n):
                self.pos[i] = self.skin(i, pal)
                self.prev[i] = self.pos[i]
            self.reset = False
        else:
            share = r(0.75 - float(self.k))
            a = np.array([r(float(share) * float(move[k])) for k in range(3)], dtype=np.float32)
            c = np.array([r(float(move[k]) * 0.75) for k in range(3)], dtype=np.float32)
            for i in range(self.n):
                if self.free(i):
                    self.pos[i] = self.pos[i] + a
                    self.prev[i] = self.prev[i] + c
                else:
                    self.pos[i] = self.skin(i, pal)
            self.verlet(wind, strength, no_wind)
        self.normals()

    # 023700 (without 023BD0's spheres)
    def verlet(self, wind, strength, no_wind):
        s = r(float(strength) * 0.1)
        for i in range(self.n):
            if not self.free(i):
                continue
            vel = (self.pos[i] - self.prev[i]).astype(np.float32)
            vel = (self.damping * vel).astype(np.float32)
            self.prev[i] = self.pos[i]
            self.pos[i] = self.pos[i] + vel
            self.pos[i][1] = r(float(self.gravity) * 0.1 + float(self.pos[i][1]))
            if not no_wind:
                w = (self.nrm[i] + wind).astype(np.float32)
                if float(w[0]) ** 2 + float(w[1]) ** 2 + float(w[2]) ** 2 < 0.01:
                    w = (wind - self.nrm[i]).astype(np.float32)
                w = (s * w).astype(np.float32)
                self.pos[i] = self.pos[i] + w
        for _ in range(4):
            for lst, k in ((self.list_a, self.k_a), (self.list_b, self.k_b)):
                for ea, eb, rest in lst:
                    a, b = ea & 0x7FFF, eb & 0x7FFF
                    d = (self.pos[b] - self.pos[a]).astype(np.float32)
                    length = (float(d[0]) ** 2 + float(d[1]) ** 2 + float(d[2]) ** 2) ** 0.5
                    if length > 0.0001:
                        d = (r((rest - length) * float(k) / length) * d).astype(np.float32)
                        if not ea & 0x8000:
                            self.pos[a] = self.pos[a] - d
                        if not eb & 0x8000:
                            self.pos[b] = self.pos[b] + d

    # 024900: (0, 1, 0) plus each triangle's cross product (027550), normalized (0274B0)
    def normals(self):
        acc = np.zeros((self.n, 3), dtype=np.float32)
        acc[:, 1] = 1.0

        def cross(p, q):
            return np.array([float(q[2]) * float(p[1]) - float(p[2]) * float(q[1]),
                             float(p[2]) * float(q[0]) - float(q[2]) * float(p[0]),
                             float(p[0]) * float(q[1]) - float(q[0]) * float(p[1])], dtype=np.float32)

        def add(tri, flip):
            i0, i1, i2 = (t & 0x7FFF for t in tri)
            e1 = (self.pos[i1] - self.pos[i0]).astype(np.float32)
            e2 = (self.pos[i2] - self.pos[i0]).astype(np.float32)
            c = cross(e1, e2) if flip else cross(e2, e1)
            for v in (i0, i1, i2):
                acc[v] = acc[v] + c

        d, o = self.d, self.cmd_at
        while True:
            op = struct.unpack_from('<H', d, o)[0]
            if op == 0xFFFF:
                break
            if op & 0x8000:
                o += 18 if (op >> 4) & 0xF == 0 else 46
                continue
            cnt = struct.unpack_from('<H', d, o + 2)[0]
            if op == 0x0054:
                for t in range(cnt):
                    add(struct.unpack_from('<3H', d, o + 4 + 30 * t), False)
                o += (2 + 15 * cnt) * 2
            elif op == 0x5453:
                idx = list(struct.unpack_from('<3H', d, o + 4))
                add(idx, False)
                flip = True
                for t in range(1, cnt):
                    idx = [idx[1], idx[2], struct.unpack_from('<H', d, o + 4 + 30 + 10 * (t - 1))[0]]
                    add(idx, flip)
                    flip = not flip
                o += (5 * cnt + 12) * 2
            elif op == 0x0043:
                o += (2 + 5 * cnt) * 2
            elif op == 0x4353:
                o += (cnt + 6) * 2
            else:
                break
        for i in range(self.n):
            v = acc[i]
            length = (float(v[0]) ** 2 + float(v[1]) ** 2 + float(v[2]) ** 2) ** 0.5
            s = 9999999.0 if length == 0.0 else 1.0 / length
            self.nrm[i] = np.array([s * float(v[k]) for k in range(3)], dtype=np.float32)


def scenario(f):
    """The frame's offset along the path, wind strength, no-wind flag, pose frame"""
    offset = np.array([0.06 * f, 0.0, 0.035 * f], dtype=np.float32) if f < 30 else np.array([0.06 * 29, 0.0, 0.035 * 29], dtype=np.float32)
    strength = 0.0325 if f < 14 else (0.012 if f < 26 else 0.0)
    no_wind = 1 if f >= 40 else 0
    pose = 0 if f in (9, 10, 33) else 1
    return offset, strength, no_wind, pose


def run(rel, out):
    ch = chars.read_chunks(rel)
    skel = next(chars.Skeleton(d) for n, k, d, fo in ch if k == 0x29)
    bind = chars.pose(skel, [], 0).astype(np.float32)
    os2 = [(n, d) for n, k, d, fo in ch if k == 0x2A]
    cloths = [(i, Cloth(d)) for i, (n, d) in enumerate(os2) if d[2] & 0x7F == 1]
    wind = np.array([-1.0, 0.0, 0.0], dtype=np.float32)
    name = rel.encode()
    out.write(struct.pack('<I', len(name)) + name)
    out.write(struct.pack('<3I', skel.count, len(cloths), FRAMES))
    for i, c in cloths:
        out.write(struct.pack('<2I', i, c.n))
    last = np.zeros(3, dtype=np.float32)
    for f in range(FRAMES):
        offset, strength, no_wind, pose = scenario(f)
        pal = bind.copy()
        pal[:, 3, :3] = (pal[:, 3, :3] + offset).astype(np.float32)
        move = (offset - last).astype(np.float32) if f > 0 else np.zeros(3, dtype=np.float32)
        last = offset
        out.write(pal.astype('<f4').tobytes())
        out.write(struct.pack('<3f', *move))
        out.write(struct.pack('<f', strength))
        out.write(struct.pack('<2I', no_wind, pose))
        for _, c in cloths:
            if pose:
                c.step(pal, move, wind, r(strength), no_wind)
            out.write(c.pos.astype('<f4').tobytes())
            out.write(c.nrm.astype('<f4').tobytes())
    return len(cloths)


def main():
    path = sys.argv[1]
    dats = sys.argv[2:] or ['ROM/7/95.DAT', 'ROM/203/121.DAT']
    with open(path, 'wb') as out:
        out.write(struct.pack('<I', len(dats)))
        for rel in dats:
            n = run(rel, out)
            print('%s: %d cloth meshes, %d frames' % (rel, n, FRAMES))
    print('->', path)


if __name__ == '__main__':
    main()
