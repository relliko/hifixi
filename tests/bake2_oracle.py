"""
bake2_test's oracle: reshade-worldfx's Python bakers (tools/bake_shadow_geo.py as it bakes by default, the compact SHD2
file; tools/bake_lamps.py) run on the zones asked for, into a folder, each one timed.

    python tests/bake2_oracle.py <out folder> [--shadow <zone> ...] [--lamps <zone> ...]

Writes <out>/shadow/<zone>.bin and <out>/lamps/<zone>.bin, and <out>/oracle.txt: a line a bake,
"<kind> <zone> <file|none|error> <seconds> <what>" (none: the baker wrote nothing for the zone, as when it has nothing
placed that casts, or no lamps).
"""

import argparse
import os
import struct
import sys
import time

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'reshade-worldfx', 'tools')
sys.path.insert(0, os.path.abspath(TOOLS))
import bake_lamps  # noqa: E402
import bake_shadow_geo  # noqa: E402


def shadow(zone, path):
    """bake_shadow_geo.main()'s default for one zone: (status, what)."""
    ranges, tris, placements, masks, counts = bake_shadow_geo.bake_compact(zone)
    if not placements:
        return 'none', 'nothing placed casts'
    size = bake_shadow_geo.write_compact(path, zone, ranges, tris, placements, masks)
    return 'file', '%d models (%d triangles), %d placements, %d masks, %d bytes' % (len(ranges), len(tris), len(placements), len(masks), size)


def lamps(zone, path):
    """bake_lamps.main() for one zone: (status, what)."""
    r = bake_lamps.bake(zone, path)
    if r is None:
        return 'none', 'no MZB'
    if r[0] == 0:
        return 'none', 'no lamps (%d ids without a generator)' % r[1]
    return 'file', '%d lamps (%d ids without a generator), %d bytes' % (r[0], r[1], os.path.getsize(path))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--shadow', type=int, nargs='*', default=[])
    ap.add_argument('--lamps', type=int, nargs='*', default=[])
    args = ap.parse_args()
    lines = []
    for kind, fn, zones in (('shadow', shadow, args.shadow), ('lamps', lamps, args.lamps)):
        folder = os.path.join(args.out, kind)
        os.makedirs(folder, exist_ok=True)
        for zone in zones:
            path = os.path.join(folder, '%d.bin' % zone)
            if os.path.exists(path):
                os.remove(path)
            t = time.perf_counter()
            try:
                status, what = fn(zone, path)
            except (OSError, ValueError, struct.error, IndexError, OverflowError) as e:
                status, what = 'error', str(e)
            line = '%s %d %s %.3f %s' % (kind, zone, status, time.perf_counter() - t, what)
            print(line, flush=True)
            lines.append(line)
    with open(os.path.join(args.out, 'oracle.txt'), 'w') as f:
        f.write('\n'.join(lines) + '\n')


if __name__ == '__main__':
    main()
