"""install.py --unlist / --relist (C:\\code\\LSB\\hifi-textures\\tools\\install.py; docs/texture-pack/characters.md
"install.py --unlist / --relist"), only ever on copies of the game's pivot.ini under build\\test\\install, their root_path
pointed at a scratch folder there too (so nothing the script does can reach the real overlay folder):
  - --unlist: only the hifi-textures line gone, every other byte and the CRLFs kept, a backup beside it byte for byte the
    old file; again: nothing to do, no second backup;
  - --relist: byte for byte the original again; refused (and nothing changed) once pivot.ini changed after the unlist;
  - an overlay listed after hifi-textures moves down a number at --unlist (Pivot stops at the first missing number) and
    back at --relist; --status names a gap;
  - the game check: refused with a .txt just written in --logs, and while a process named pol.exe runs (a copy of
    PING.EXE); --ignore-game refused for the game's own pivot.ini (asked of the script's gate() directly: never run
    against it); --undo refused without --delete-folder;
  - --status names the backup and the game check's verdict.
The game's pivot.ini is only read. Skipped (said so) without install.py or pivot.ini; the process part skipped while the
real game runs (the rest then with --ignore-game, which the copies allow).

  python -I tests\\install_unlist_test.py
"""
import importlib.util
import os
import shutil
import subprocess
import sys
import time

INSTALL = r'C:\code\LSB\hifi-textures\tools\install.py'
REAL_INI = r'C:\Games\PhoenixXI\config\pivot\pivot.ini'
here = os.path.dirname(os.path.abspath(__file__))
work = os.path.join(os.path.dirname(here), 'build', 'test', 'install')

failures = passes = 0


def check(ok, what):
    global failures, passes
    print('%s  %s' % ('ok  ' if ok else 'FAIL', what))
    if ok:
        passes += 1
    else:
        failures += 1


def game_running():
    out = subprocess.run(['tasklist', '/FO', 'CSV', '/NH'], capture_output=True).stdout.decode('mbcs', 'replace').lower()
    return '"pol.exe"' in out or '"phoenix-loader.exe"' in out


def main():
    if not os.path.exists(INSTALL) or not os.path.exists(REAL_INI):
        print('--    skipped: %s or %s not on this PC' % (INSTALL, REAL_INI))
        print('\nall 0 passed (skipped)')
        return 0
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(os.path.join(work, 'logs'))
    os.makedirs(os.path.join(work, 'DATs', 'hifi-textures'))
    ini = os.path.join(work, 'pivot.ini')
    logs = os.path.join(work, 'logs')
    # (a copy of the real one, read only; its root_path the scratch DATs folder)
    real = open(REAL_INI, 'rb').read()
    lines = real.decode('ascii').split('\r\n')
    lines = ['root_path=' + os.path.join(work, 'DATs') if l.lower().startswith('root_path=') else l for l in lines]
    original = '\r\n'.join(lines).encode('ascii')
    if b'hifi-textures' not in original:
        # (already unlisted on this PC: the test lists it itself, as the overlay's install did: the next number after the
        # last, since Pivot stops reading [overlays] at the first missing one and the gap checks below rely on none)
        listed = [int(l.split(b'=', 1)[0]) for l in original.split(b'\r\n') if b'=' in l and l.split(b'=', 1)[0].strip().isdigit()]
        original = original.rstrip(b'\r\n') + b'\r\n%d=hifi-textures\r\n' % (max(listed) + 1 if listed else 0)
    open(ini, 'wb').write(original)
    old_log = os.path.join(logs, 'old.txt')
    open(old_log, 'w').write('an old log')
    t = time.time() - 3600
    os.utime(old_log, (t, t))
    running = game_running()
    extra = ['--ignore-game'] if running else []
    if running:
        print('--    the game runs: the process check is skipped, the rest with --ignore-game (copies only)')

    def run(*args):
        p = subprocess.run([sys.executable, '-I', INSTALL, '--ini', ini, '--logs', logs] + list(args), capture_output=True, cwd=work, timeout=600)
        return p.returncode, (p.stdout + p.stderr).decode('utf-8', 'replace')

    def backups():
        return sorted(f for f in os.listdir(work) if f.startswith('pivot.ini.') and f.endswith('.before-unlist'))

    # ---- --unlist
    rc, out = run('--unlist', *extra)
    now = open(ini, 'rb').read()
    want = b''.join(l + b'\r\n' for l in original.split(b'\r\n')[:-1] if not l.lower().endswith(b'=hifi-textures'))
    b = backups()
    check(rc == 0 and now == want and len(b) == 1 and open(os.path.join(work, b[0]), 'rb').read() == original,
          '--unlist: only the hifi-textures line gone (%d bytes -> %d), a backup %s byte for byte the old file' % (len(original), len(now), b[0] if b else 'none'))
    check(now.count(b'\r\n') == now.count(b'\n') and b'hifi-textures' not in now and os.path.isdir(os.path.join(work, 'DATs', 'hifi-textures')),
          '  CRLF kept, every other line as it was; the overlay folder kept')
    rc, out = run('--unlist', *extra)
    check(rc == 0 and 'nothing to do' in out and open(ini, 'rb').read() == want and len(backups()) == 1, '--unlist again: nothing to do, no second backup')

    # ---- --status
    rc, out = run('--status', *extra)
    check(rc == 0 and 'not listed' in out and b[0] in out and 'the game check:' in out, '--status: not listed, the backup and the game check named')

    # ---- --relist
    rc, out = run('--relist', *extra)
    check(rc == 0 and open(ini, 'rb').read() == original, '--relist: byte for byte the original again')
    rc, out = run('--unlist', *extra)
    changed = open(ini, 'rb').read() + b'7=someone-else\r\n'
    open(ini, 'wb').write(changed)
    rc, out = run('--relist', *extra)
    check(rc != 0 and 'changed since the unlist' in out and open(ini, 'rb').read() == changed, '--relist after pivot.ini changed: refused, nothing changed')
    open(ini, 'wb').write(original)
    for f in backups():
        os.remove(os.path.join(work, f))

    # ---- An overlay listed after hifi-textures: moved down a number (Pivot stops reading at the first missing one), and
    # --relist puts the numbers back byte for byte; --status names a gap
    def numbered(data):
        out = []
        for l in data.split(b'\r\n'):
            k, _, v = l.partition(b'=')
            if v and k.strip().isdigit():
                out.append((int(k), v.decode('ascii')))
        return out

    ours = next(n for n, v in numbered(original) if v.lower() == 'hifi-textures')
    after = max(n for n, _ in numbered(original)) + 1
    later = original + b'%d=later-mod\r\n' % after
    open(ini, 'wb').write(later)
    rc, out = run('--unlist', *extra)
    now = open(ini, 'rb').read()
    nums = sorted(n for n, _ in numbered(now))
    check(rc == 0 and b'%d=later-mod\r\n' % (after - 1) in now and nums == list(range(len(nums))) and 'moved down a number' in out,
          '--unlist with an overlay after it (%d=later-mod, ours %d): moved down to %d, no gap in [overlays] (%s)' % (after, ours, after - 1, ', '.join(map(str, nums))))
    rc, out = run('--relist', *extra)
    check(rc == 0 and open(ini, 'rb').read() == later, '  --relist: byte for byte as before, later-mod back at %d' % after)
    gapped = original.replace(b'%d=hifi-textures' % ours, b'%d=hifi-textures' % (after + 1))
    open(ini, 'wb').write(gapped)
    rc, out = run('--status', *extra)
    check(rc == 0 and 'has a gap at %d' % ours in out and 'hifi-textures never loaded' in out, '--status names a gap: ' + next((l for l in out.splitlines() if 'gap' in l), 'none'))
    open(ini, 'wb').write(original)
    for f in backups():
        os.remove(os.path.join(work, f))

    # ---- The game check
    fresh = os.path.join(logs, 'phoenix.txt')
    open(fresh, 'w').write('the game just wrote this')
    rc, out = run('--unlist')
    check(rc != 0 and ('phoenix.txt' in out or running) and open(ini, 'rb').read() == original and not backups(),
          '--unlist with a log written just now: refused (%s), nothing changed, no backup' % out.strip().splitlines()[-1] if out.strip() else '')
    t = time.time() - 120
    os.utime(fresh, (t, t))
    if not running:
        pol = os.path.join(work, 'pol.exe')
        shutil.copyfile(os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', 'PING.EXE'), pol)
        p = subprocess.Popen([pol, '-n', '30', '127.0.0.1'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            time.sleep(0.5)
            rc, out = run('--unlist')
            refused = rc != 0 and 'pol.exe is running' in out and open(ini, 'rb').read() == original and not backups()
            rc2, out2 = run('--relist')
            refused_relist = rc2 != 0 and 'pol.exe is running' in out2
        finally:
            p.kill()
            p.wait()
        check(refused and refused_relist, '--unlist and --relist while a pol.exe runs: refused, nothing changed')
        rc, out = run('--unlist')
        check(rc == 0 and b'hifi-textures' not in open(ini, 'rb').read(), '  the game closed (a log 2 minutes old): --unlist done')
        rc, out = run('--relist')
        check(rc == 0 and open(ini, 'rb').read() == original, '  and --relist')
    else:
        print('--    skipped: the process check (the real game runs)')

    # ---- --ignore-game never for the real pivot.ini: the script's own gate asked (nothing run against it)
    spec = importlib.util.spec_from_file_location('install_under_test', INSTALL)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    why_real = m.gate(REAL_INI, logs, True)
    why_real_case = m.gate(REAL_INI.upper(), logs, True)
    why_copy = m.gate(ini, logs, True)
    check(why_real != '' and why_real_case != '' and why_copy == '' and m.PIVOT_INI == REAL_INI,
          '--ignore-game: refused for the game\'s own pivot.ini (any case), allowed for a copy')
    # (the log half of the check on its own: with the real game running the process half answers first above)
    t = time.time() - 120
    os.utime(fresh, (t, t))
    quiet = m.log_recent(logs)
    os.utime(fresh, None)
    recent = m.log_recent(logs)
    os.utime(fresh, (t, t))
    check(quiet == '' and 'phoenix.txt' in recent, 'the log check: a log 2 minutes old passes, one written just now refuses (%s)' % recent)

    # ---- --undo needs --delete-folder (the copy's root is the scratch folder: nothing real could go)
    rc, out = run('--undo')
    check(rc != 0 and '--delete-folder' in out and open(ini, 'rb').read() == original and os.path.isdir(os.path.join(work, 'DATs', 'hifi-textures')),
          '--undo without --delete-folder: refused, nothing changed, the folder kept')
    check(open(REAL_INI, 'rb').read() == real, 'the game\'s pivot.ini unchanged')

    print('\nall %d passed' % passes if failures == 0 else '\n%d failed' % failures)
    return failures


if __name__ == '__main__':
    sys.exit(main())
