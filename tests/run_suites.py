"""Runs every offline suite build.cmd built, side by side, and prints one line each.

  python -I tests\\run_suites.py [hifixi folder] [log folder]     (defaults: this repo, build\\suites)

The suites that need an argument get it (the oracle files build.cmd made, our d3d8.dll); zoneinject_test and packstream_test run again on
the GPU (ZONE_GPU=1, alone, after the rest). A suite that fails is run once more on
its own: zonedraw_test's "mapped at its base" sometimes fails when the address it wants is taken, and passes alone.
The exit code is the number of suites that failed. Each suite's whole output is in the log folder.
"""
import concurrent.futures
import glob
import os
import subprocess
import sys

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.dirname(here)
logs = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else os.path.join(root, 'build', 'suites')
os.makedirs(logs, exist_ok=True)
build = os.path.join(root, 'build')

ARGS = {
    'cloth_test': [r'build\test\cloth_oracle.bin'],
    'actors_draw_test': [r'build\test\cloth_oracle.bin'],
    'chars_data_test': [r'build\test\chars_oracle.txt'],
    'charinject_test': [r'build\test\chars_pose.bin'],
    'fx_render_test': [r'build\test\fx_fire.bin', r'build\test\fx_cure.bin', r'build\test\fx_fastblade.bin',
                       r'build\test\fx_fountain.bin'],
}
runs = []
for exe in sorted(glob.glob(os.path.join(build, '*_test.exe'))):
    name = os.path.splitext(os.path.basename(exe))[0]
    runs.append((name, [exe] + ARGS.get(name, []), None))
runs.append(('engine_check', [os.path.join(build, 'engine_check.exe')], None))
runs.append(('d3d8_unit', [os.path.join(build, 'd3d8', 'd3d8_unit.exe')], None))
runs.append(('d3d8_host', [os.path.join(build, 'd3d8', 'd3d8_host.exe'), r'build\d3d8\d3d8.dll'], None))
runs.append(('zoneinject_gpu', [os.path.join(build, 'zoneinject_test.exe')], {'ZONE_GPU': '1'}))
runs.append(('packstream_gpu', [os.path.join(build, 'packstream_test.exe')], {'ZONE_GPU': '1'}))
GPU = {'zoneinject_gpu', 'packstream_gpu'}

missing = [r[0] for r in runs if not os.path.exists(r[1][0])]
runs = [r for r in runs if os.path.exists(r[1][0])]


def run(item):
    name, cmd, env_extra = item
    env = dict(os.environ)
    if env_extra:
        env.update(env_extra)
    try:
        p = subprocess.run(cmd, cwd=root, env=env, capture_output=True, timeout=1800)
        out = p.stdout.decode('utf-8', 'replace') + p.stderr.decode('utf-8', 'replace')
        rc = p.returncode
    except subprocess.TimeoutExpired:
        out, rc = 'TIMEOUT', -1
    with open(os.path.join(logs, name + '.log'), 'w', encoding='utf-8') as f:
        f.write(out)
    lines = [l.strip() for l in out.splitlines() if l.strip()]
    fails = [l for l in lines if l.startswith('FAIL')]
    return name, rc, (fails[0] if fails else (lines[-1] if lines else ''))


results = {}
with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
    for name, rc, last in pool.map(run, [r for r in runs if r[0] not in GPU]):
        results[name] = (rc, last)
for r in runs:
    if r[0] in GPU:
        name, rc, last = run(r)
        results[name] = (rc, last)
for name, (rc, last) in list(results.items()):
    if rc != 0:
        _, rc2, last2 = run(next(r for r in runs if r[0] == name))
        if rc2 == 0:
            results[name] = (0, last2 + '  (passed on the second run; first: ' + last + ')')

bad = 0
for name in sorted(results):
    rc, last = results[name]
    bad += rc != 0
    print(f'{"ok  " if rc == 0 else "FAIL"} {name:22} rc={rc} {last[:150]}')
for name in missing:
    print(f'miss {name:22} not built')
print(f'{len(results)} suites, {bad} failed' + (f', {len(missing)} not built' if missing else '') + f'; logs in {logs}')
sys.exit(bad)
