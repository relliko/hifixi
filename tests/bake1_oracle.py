"""
bake1_test's oracle: reshade-worldfx's Python bakers (tools/bake_grass.py, bake_water.py, bake_wet.py, bake_rain.py,
each run as its own command line runs it) on the zones asked for, into a folder, each one timed.

    python tests/bake1_oracle.py <out folder> <zone> ...

Writes <out>/<kind>/<zone>.bin and <out>/oracle.txt: a line a bake, "<kind> <zone> <file|none|error> <seconds> <what>".
none: the baker wrote nothing for the zone (no collision, no rainy weather, no path or stone...: it exits without a
file), or a file with nothing in it (no blades, no water: tools/bake_all.py deletes those). Rain reads the water baked
here (--water <out>/water), as bake_all bakes water before rain.

OpenBLAS runs on one thread here: bake_water.py's conjugate gradients take dot products from BLAS, and a long one
(a sea zone's: Al'Taieu's 900,000 points) split over threads adds up differently with the number of threads, so the
file would depend on the PC. On one thread it's the same on any PC with AVX2 (OpenBLAS's Haswell kernel), and that's
what src/bake/water.cpp computes.
"""

import os

os.environ['OPENBLAS_NUM_THREADS'] = '1'  # (before numpy is loaded)

import contextlib
import io
import runpy
import struct
import sys
import time

TOOLS = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'reshade-worldfx', 'tools'))
sys.path.insert(0, TOOLS)
KINDS = ('grass', 'water', 'wet', 'rain')
ANY_WEATHER = False  # (--any-weather: wet and rain made whatever the zone's own weather, as HiFi XI does for sunlit zones)


def run(kind, zone, path, out):
    """One bake as `python tools/bake_<kind>.py <zone> <path>` runs it: (status, what)."""
    argv = [os.path.join(TOOLS, 'bake_%s.py' % kind), str(zone), path]
    if kind == 'rain':
        argv += ['--water', os.path.join(out, 'water')]
    if ANY_WEATHER and kind in ('wet', 'rain'):
        argv += ['--any-weather']
    said = io.StringIO()
    old = sys.argv
    sys.argv = argv
    try:
        with contextlib.redirect_stdout(said):
            runpy.run_path(argv[0], run_name='__main__')
    except SystemExit as e:
        if e.code not in (None, 0):
            return 'none', str(e.code)
    except ValueError as e:
        return 'none', 'ValueError: %s' % e
    except (OSError, struct.error, IndexError, OverflowError) as e:
        return 'error', '%s: %s' % (type(e).__name__, e)
    finally:
        sys.argv = old
    if not os.path.exists(path):
        return 'none', 'no file'
    count = struct.unpack_from('<I', open(path, 'rb').read(20), 16)[0]
    if count == 0:
        os.remove(path)
        return 'none', 'nothing in it'
    lines = said.getvalue().strip().splitlines()
    return 'file', (lines[0] if lines else '')[:200]


def main():
    global ANY_WEATHER
    args = [a for a in sys.argv[1:] if a != '--any-weather']
    ANY_WEATHER = len(args) != len(sys.argv) - 1
    out = args[0]
    zones = [int(z) for z in args[1:]]
    lines = []
    for kind in KINDS:
        os.makedirs(os.path.join(out, kind), exist_ok=True)
    for zone in zones:
        for kind in KINDS:
            path = os.path.join(out, kind, '%d.bin' % zone)
            if os.path.exists(path):
                os.remove(path)
            t = time.perf_counter()
            status, what = run(kind, zone, path, out)
            line = '%s %d %s %.3f %s' % (kind, zone, status, time.perf_counter() - t, what.replace('\n', ' '))
            print(line, flush=True)
            lines.append(line)
    with open(os.path.join(out, 'oracle.txt'), 'w') as f:
        f.write('\n'.join(lines) + '\n')


if __name__ == '__main__':
    main()
