"""enspell_poses: a skeleton posed through a motion's frames by research/ffxi-re/tools/chars.py, for tests/enspell_view.cpp:
u32 poses, u32 bones, then each pose's palette (16 float32 a bone: row vectors, translation in the last row).

    python tests/enspell_poses.py <out.bin> [skeleton dat] [motion dat] [motions] [poses]

Default: the Hume male (ROM/27/82.DAT) in the sword battle stance (ROM/32/13.DAT btl0 + btl1), 60 poses through it.
"""
import os
import struct
import sys

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'research', 'ffxi-re', 'tools')
sys.path.insert(0, os.path.abspath(TOOLS))
import chars  # noqa: E402


def main():
    out = sys.argv[1]
    skeleton_dat = sys.argv[2] if len(sys.argv) > 2 else 'ROM/27/82.DAT'
    motion_dat = sys.argv[3] if len(sys.argv) > 3 else 'ROM/32/13.DAT'
    names = (sys.argv[4] if len(sys.argv) > 4 else 'btl0,btl1').split(',')
    count = int(sys.argv[5]) if len(sys.argv) > 5 else 60
    skel = next(chars.Skeleton(d) for n, k, d, f in chars.read_chunks(skeleton_dat) if k == 0x29)
    motions = [chars.Motion(n, d) for n, k, d, f in chars.read_chunks(motion_dat) if k == 0x2B and n in names]
    frames = max(m.frames for m in motions) if motions else 0
    with open(out, 'wb') as f:
        f.write(struct.pack('<II', count, skel.count))
        for i in range(count):
            pal = chars.pose(skel, motions, frames * i / count)
            for m in pal:
                f.write(struct.pack('<16f', *m.reshape(-1)))
    print('%d bones, motions %s (%d frames) -> %d poses in %s' % (skel.count, [m.name for m in motions], frames, count, out))


if __name__ == '__main__':
    main()
