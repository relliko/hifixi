"""chars_pose: a DAT's skeleton posed by research/ffxi-re/tools/chars.py (034620 + 0347D0 as the game poses it), its
palette (each bone's world matrix, row vectors, translation in the last row) written for tests/charinject_test.cpp:
u32 bone count, then 16 float32 per bone.

    python tests/chars_pose.py <out.bin> [dat] [motion] [frame]      (ROM2/0/52.DAT idl0 0: the Mithra NPC idling)
"""
import os
import struct
import sys

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'research', 'ffxi-re', 'tools')
sys.path.insert(0, os.path.abspath(TOOLS))
import chars  # noqa: E402


def main():
    out = sys.argv[1]
    dat = sys.argv[2] if len(sys.argv) > 2 else 'ROM2/0/52.DAT'
    motion = sys.argv[3] if len(sys.argv) > 3 else 'idl0'
    frame = float(sys.argv[4]) if len(sys.argv) > 4 else 0.0
    ch = chars.read_chunks(dat)
    skel = next(chars.Skeleton(d) for n, k, d, f in ch if k == 0x29)
    motions = [chars.Motion(n, d) for n, k, d, f in ch if k == 0x2B and n == motion]
    pal = chars.pose(skel, motions, frame)
    with open(out, 'wb') as f:
        f.write(struct.pack('<I', skel.count))
        for m in pal:
            f.write(struct.pack('<16f', *m.reshape(-1)))
    print('%d bones, motion %s -> %s' % (skel.count, [m.name for m in motions], out))


if __name__ == '__main__':
    main()
