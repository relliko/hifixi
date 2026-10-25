"""
sea_test's oracle: reshade-worldfx's tools/bake_sea.py run on the zones asked for, into a folder, each one timed.

    python tests/sea_oracle.py [--water <folder of WTR2 files>] <out folder> <zone> ...

Writes <out>/sea/<zone>.bin, <out>/oracle.txt: a line a bake, "sea <zone> <file|none|error> <seconds> <what>" (none: no
water file for the zone, or no sheet kept), and <out>/sheets.txt: every kept sheet (zone, generator, model, kind,
triangles, offset, % of its corners out past the zone).
"""

import argparse
import os
import struct
import sys
import time

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'reshade-worldfx', 'tools')
sys.path.insert(0, os.path.abspath(TOOLS))
import bake_sea  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('zones', type=int, nargs='*')
    ap.add_argument('--water', default=bake_sea.WATER)
    args = ap.parse_args()
    folder = os.path.join(args.out, 'sea')
    os.makedirs(folder, exist_ok=True)
    lines, sheets = [], []
    for zone in args.zones:
        path = os.path.join(folder, '%d.bin' % zone)
        if os.path.exists(path):
            os.remove(path)
        water = os.path.join(args.water, '%d.bin' % zone)
        t = time.perf_counter()
        try:
            if not os.path.exists(water):
                status, what = 'none', 'no water file'
            else:
                kept = bake_sea.bake(zone, water)
                if not kept:
                    status, what = 'none', 'no sheet kept'
                else:
                    size = bake_sea.write(path, zone, kept)
                    status, what = 'file', '%d sheets, %d triangles, %d bytes' % (len(kept), sum(len(k[3]) for k in kept) // 3, size)
                    for g, model, kind, world, off, out in kept:
                        sheets.append('%d %s %r %s %d %+.4f %.1f' % (zone, g, model.decode('latin1'), kind, len(world) // 3, off, 100.0 * out / len(world)))
        except (OSError, ValueError, struct.error, IndexError, OverflowError) as e:
            status, what = 'error', '%s: %s' % (type(e).__name__, e)
        line = 'sea %d %s %.3f %s' % (zone, status, time.perf_counter() - t, what)
        print(line, flush=True)
        lines.append(line)
    with open(os.path.join(args.out, 'oracle.txt'), 'w') as f:
        f.write('\n'.join(lines) + '\n')
    with open(os.path.join(args.out, 'sheets.txt'), 'w') as f:
        f.write('\n'.join(sheets) + '\n')


if __name__ == '__main__':
    main()
