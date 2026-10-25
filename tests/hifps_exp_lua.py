"""
The hifps addon's own experimental toggles (C:\\Games\\PhoenixXI\\addons\\hifps\\hifps.lua), run under lupa's
LuaJIT on an FFXiMain dump, step by step, for tests/hifps_exp_test.cpp to hold HiFi XI's C++ port
(src/game/hifps/exp.cpp) against, byte for byte.

    python tests/hifps_exp_lua.py <in.txt> <out.bin>

(plain python, not -I: lupa from site-packages.) The test writes in.txt: the dump, the test's fakes (the client's
renderer and device, Ashita's draw methods and plugin list, the CPU feature object: regions mapped here at the same
addresses, and the dump's bytes it changed), QueryPerformanceCounter's address and frequency in the test, Ashita's
folder, and the steps: each an action (what the addon's config window or /hifps does: exp_toggle, a tick of
exp_tick, disable...), what the host says then (the frame rate, the plugins, the loaded addons, the player's and the
target's actors) and the edits made before it (bytes, files). After each step out.bin gets the memory the test
compares (the image's bytes that differ from the dump file, the fakes, the addon's allocations) and the addon's state
as text (exp.on/err, exp_stat, exp.users/checked/notes/force).

hifps.lua's own graphics settings (lamps, night lighting) stay at their defaults (1.0: stock), so it lays nothing down
for them: HiFi XI doesn't do them. hifps.lua's top-level locals are reached by loading its source with a line appended
that exports them. The addon's files are only read.
"""

import ctypes
import os
import re
import struct
import sys

from lupa import luajit21

ADDON = r'C:\Games\PhoenixXI\addons\hifps'
LIBS = r'C:\Games\PhoenixXI\addons\libs'
NAMES = ['ashita', 'occlusion', 'walls', 'sse', 'batch', 'dedupe']

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.VirtualAlloc.restype = ctypes.c_void_p
k32.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_uint32]
k32.GetCurrentProcess.restype = ctypes.c_void_p
k32.ReadProcessMemory.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.ReadProcessMemory.restype = ctypes.c_int
PROCESS = k32.GetCurrentProcess()


def map_at(addr, size):
    p = k32.VirtualAlloc(addr, size, 0x3000, 0x40)
    if p != addr:
        sys.exit("FAIL  can't map 0x%08X here (got %r, error %d)" % (addr, p, ctypes.get_last_error()))


def mem_read(addr, n):
    """The bytes, or None when any can't be read (as the C++ port's read())"""
    if addr < 0x10000 or n <= 0:
        return None if n > 0 else b''
    buf = ctypes.create_string_buffer(n)
    got = ctypes.c_size_t(0)
    if not k32.ReadProcessMemory(PROCESS, addr, buf, n, ctypes.byref(got)) or got.value != n:
        return None
    return buf.raw


def mem_write(addr, data):
    ctypes.memmove(addr, bytes(data), len(data))


def pattern_re(pat):
    out = b''
    for i in range(0, len(pat), 2):
        h = pat[i:i + 2]
        out += b'.' if h == '??' else re.escape(bytes([int(h, 16)]))
    return re.compile(out, re.S)


def runs(a, b, base):
    """The runs of bytes where a and b differ, as (address, bytes of b)"""
    out = []
    n = len(a)
    i = 0
    while i < n:
        if a[i:i + 4096] == b[i:i + 4096]:
            i += 4096
            continue
        end = min(n, i + 4096)
        while i < end:
            if a[i] != b[i]:
                j = i
                while j < n and a[j] != b[j]:
                    j += 1
                out.append((base + i, b[i:j]))
                i = j
            else:
                i += 1
    return out


def parse_in(path):
    cfg = {'regions': [], 'edits': [], 'files': [], 'steps': []}
    for line in open(path, encoding='utf-8').read().splitlines():
        if not line.strip():
            continue
        key, _, rest = line.partition(' ')
        if key in ('dump', 'install'):
            cfg[key] = rest
        elif key in ('qpc', 'freq', 'alloc'):
            cfg[key] = int(rest, 16) if key != 'freq' else int(rest)
        elif key == 'ashita':
            b, n = rest.split(' ')
            cfg['ashita_base'], cfg['ashita_size'] = int(b, 16), int(n, 16)
        elif key == 'region':
            a, h = rest.split(' ')
            cfg['regions'].append((int(a, 16), bytes.fromhex(h)))
        elif key == 'edit':
            step, where, off, h = rest.split(' ')
            cfg['edits'].append((int(step), where, int(off, 16), bytes.fromhex(h)))
        elif key == 'file':
            step, p, h = rest.split(' ')
            cfg['files'].append((int(step), p, bytes.fromhex(h)))
        elif key == 'step':
            parts = rest.split(' ')
            s = {'index': int(parts[0]), 'action': parts[1].replace('_', ' ')}
            for kv in parts[2:]:
                k, _, v = kv.partition('=')
                s[k] = v
            cfg['steps'].append(s)
        else:
            sys.exit('FAIL  in.txt: unknown line %r' % line)
    return cfg


def main():
    cfg = parse_in(sys.argv[1])
    out_path = sys.argv[2]
    dump_path = cfg['dump']
    base = int(re.search(r'_([0-9A-Fa-f]+)\.bin$', dump_path).group(1), 16)
    dump = open(dump_path, 'rb').read()
    # (everything mapped before the Lua state exists: LuaJIT keeps its own memory low)
    map_at(base, len(dump))
    ctypes.memmove(base, dump, len(dump))
    for a, h in cfg['regions']:
        map_at(a, len(h))
        ctypes.memmove(a, h, len(h))
    allocs = []
    alloc_next = [cfg['alloc']]

    def alloc(n):
        at = alloc_next[0]
        map_at(at, n)
        alloc_next[0] += (n + 0xFFFF) & ~0xFFFF
        allocs.append((at, n))
        return at

    # The test's edits to the dump before the first step (its own bytes in the mapped copy: the fakes' pointers)
    for step, where, off, h in cfg['edits']:
        if step < 0:
            mem_write(off, h)
    original = bytes(dump)
    fakes = [(a, len(h)) for a, h in cfg['regions']]

    host = {'fps': 0.0, 'plugins': 0, 'addons': None, 'actors': None}

    lua = luajit21.LuaRuntime(unpack_returned_tuples=True)
    g = lua.globals()

    def py_find(pat, off):
        image = ctypes.string_at(base, len(dump))
        m = pattern_re(pat).search(image)
        return 0 if m is None else base + m.start() + int(off)

    def u(a):
        return int(a) & 0xFFFFFFFF

    def py_arr(a, n):
        b = mem_read(u(a), int(n))
        if b is None:
            raise RuntimeError('read_array of unreadable memory at %08X' % u(a))
        return lua.table_from(list(b))

    def py_u32(a):
        b = mem_read(u(a), 4)
        return 0 if b is None else struct.unpack('<I', b)[0]

    def py_i32(a):
        b = mem_read(u(a), 4)
        return 0 if b is None else struct.unpack('<i', b)[0]

    def py_u8(a):
        b = mem_read(u(a), 1)
        return 0 if b is None else b[0]

    def py_f32(a):
        b = mem_read(u(a), 4)
        return 0.0 if b is None else struct.unpack('<f', b)[0]

    def py_warr(a, t):
        mem_write(u(a), bytes(int(t[i]) & 0xFF for i in range(1, len(t) + 1)))

    g.py_find = py_find
    g.py_arr = py_arr
    g.py_u32 = py_u32
    g.py_i32 = py_i32
    g.py_u8 = py_u8
    g.py_f32 = py_f32
    g.py_w8 = lambda a, v: mem_write(u(a), bytes([int(v) & 0xFF]))
    g.py_w32 = lambda a, v: mem_write(u(a), struct.pack('<I', int(v) & 0xFFFFFFFF))
    g.py_wf = lambda a, v: mem_write(u(a), struct.pack('<f', float(v)))
    g.py_warr = py_warr
    g.py_alloc = alloc
    g.py_base = lambda name: (base if name.lower() == 'ffximain.dll' else cfg['ashita_base'] if name.lower() == 'ashita.dll' else 0)
    g.py_size = lambda name: (len(dump) if name.lower() == 'ffximain.dll' else cfg['ashita_size'] if name.lower() == 'ashita.dll' else 0)
    g.py_install = cfg['install']
    g.py_qpc = cfg['qpc']
    g.py_freq = cfg['freq']
    g.py_host = lambda key: host[key]
    g.py_addons = lambda: None if host['addons'] is None else lua.table_from(host['addons'])
    g.py_exists = lambda p: os.path.exists(p)
    g.py_mkdir = lambda p: os.makedirs(p, exist_ok=True) or True

    def py_dir(path, pattern, recursive):
        if not os.path.isdir(path):
            return None
        rx = re.compile(pattern)
        found = []
        for root, dirs, files in os.walk(path):
            for f in files:
                if rx.search(f):
                    found.append(os.path.relpath(os.path.join(root, f), path))
            if not recursive:
                break
        return lua.table_from(sorted(found))
    g.py_dir = py_dir

    lua.execute(r'''
        package.path = [[%s\?.lua;%s\?.lua;]] .. package.path
        local real = require('ffi')
        local qpc_fn = real.cast('void*', py_qpc)
        local C = setmetatable({
            GetModuleHandleA = function (name) if (name == 'kernel32.dll') then return real.cast('void*', 0x1000); end return nil; end,
            GetProcAddress = function (m, name) if (name == 'QueryPerformanceCounter') then return qpc_fn; end return nil; end,
        }, { __index = real.C })
        package.loaded['ffi'] = setmetatable({ C = C }, { __index = real })
        package.loaded['imgui'] = {}
        printed = {}
        print = function (s) printed[#printed + 1] = tostring(s); end
        events = {}
        addon = {}
        ashita = {
            events = { register = function (name, id, fn) events[name] = fn; end },
            memory = {
                find = function (m, s, pat, off, usage) return py_find(pat, off or 0); end,
                read_uint8 = function (a) return py_u8(a); end,
                read_uint32 = function (a) return py_u32(a); end,
                read_int32 = function (a) return py_i32(a); end,
                read_float = function (a) return py_f32(a); end,
                read_array = function (a, n) return py_arr(a, n); end,
                write_uint8 = function (a, v) py_w8(a, v); end,
                write_uint32 = function (a, v) py_w32(a, v); end,
                write_float = function (a, v) py_wf(a, v); end,
                write_array = function (a, t) py_warr(a, t); end,
                unprotect = function (a, n) return true, 0x40; end,
                protect = function (a, n, p) return true; end,
                alloc = function (n) return py_alloc(n); end,
                dealloc = function (a) return true; end,
                get_base = function (name) return py_base(name); end,
                get_size = function (name) return py_size(name); end,
            },
            fs = {
                exists = function (p) return py_exists(p); end,
                create_dir = function (p) return py_mkdir(p); end,
                get_directory = function (p, pat, rec) return py_dir(p, pat, rec); end,
            },
        }
        local entity = {
            GetActorPointer = function (self, i)
                local a = py_host('actors')
                if (i == 5) then return a[1]; end
                if (i == 7) then return a[2]; end
                return 0;
            end,
            GetStatus = function (self, i) return py_host('actors')[3]; end,
        }
        local party = { GetMemberTargetIndex = function (self, i) if (py_host('actors') == nil) then error('no party'); end return 5; end }
        local target = { GetTargetIndex = function (self, i) return py_host('actors')[2] ~= 0 and 7 or 0; end }
        local mm = { GetEntity = function () return entity; end, GetParty = function () return party; end, GetTarget = function () return target; end }
        local pm = { Count = function () return py_host('plugins'); end }
        AshitaCore = {
            GetMemoryManager = function () return mm; end,
            GetPluginManager = function () return pm; end,
            GetInstallPath = function () return py_install; end,
        }
        local am = {
            Count = function (self) local l = py_addons(); return l and #l or nil; end,
            Get = function (self, i) local l = py_addons(); return l and l[i + 1] or ''; end,
            GetFileName = function (self, name) return nil; end,
        }
        function set_addons_known(known) rawset(_G, 'AddonManager', known and am or nil); end
    ''' % (LIBS, ADDON))

    src = open(os.path.join(ADDON, 'hifps.lua'), encoding='utf-8').read()
    src += ('\nHIFPS = { exp = exp, state = state,exp_toggle = exp_toggle, exp_tick = exp_tick, '
            'exp_stat = exp_stat, exp_all_off = exp_all_off, restore_exp = restore_exp, walls_keep = walls_keep, '
            'disable = disable };\n')
    fn = lua.eval('function (src) local f, e = loadstring(src, "@hifps.lua"); if (f == nil) then error(e); end f(); end')
    fn(src)
    H = g.HIFPS
    H.state.freq = cfg['freq']

    helpers = lua.eval(r'''function (H, names)
        local t = {}
        local function s(v) return v == nil and '' or tostring(v); end
        for i = 1, #names do
            local n = names[i]
            t[#t + 1] = 'on.' .. n .. '=' .. (H.exp.on[n] and '1' or '0')
            t[#t + 1] = 'err.' .. n .. '=' .. s(H.exp.err[n])
            t[#t + 1] = 'stat.' .. n .. '=' .. s(H.exp_stat(n))
        end
        t[#t + 1] = 'ashita.users=' .. table.concat(H.exp.users or {}, '; ')
        t[#t + 1] = 'ashita.checked=' .. s(H.exp.checked)
        t[#t + 1] = 'ashita.notes=' .. table.concat(H.exp.notes or {}, '; ')
        t[#t + 1] = 'ashita.force=' .. (H.exp.force and '1' or '0')
        return t
    end''')
    names = lua.table_from(NAMES)

    def where_base(name):
        if name == 'abs':
            return 0
        if name == 'walls':
            return int(H.exp.walls.base)
        if name == 'proxy':
            return int(H.exp.proxy.base)
        sys.exit('FAIL  in.txt: unknown base %r' % name)

    out = open(out_path, 'wb')
    for s in cfg['steps']:
        i = s['index']
        # What the host says now
        host['fps'] = float(s.get('fps', host['fps']))
        if 'plugins' in s:
            host['plugins'] = int(s['plugins'])
        if 'addons' in s:
            host['addons'] = None if s['addons'] == '?' else [a for a in s['addons'].split(',') if a]
            g.set_addons_known(host['addons'] is not None)
        if 'actors' in s:
            host['actors'] = None if s['actors'] == 'none' else lua.table_from([int(v, 16) for v in s['actors'].split(',')])
        for step, where, off, h in cfg['edits']:
            if step == i:
                mem_write(where_base(where) + off, h)
        for step, p, h in cfg['files']:
            if step == i:
                os.makedirs(os.path.dirname(p), exist_ok=True)
                open(p, 'wb').write(h)
        a = s['action'].split(' ')
        if a[0] == 'start':
            pass
        elif a[0] == 'on':
            H.exp_toggle(a[1], True, True)
        elif a[0] == 'off':
            H.exp_toggle(a[1], False, True)
        elif a[0] == 'force':
            H.exp_toggle('ashita', True, True, True)
        elif a[0] == 'tick':
            if H.exp.walls is not None and H.exp.walls.site is not None:
                H.walls_keep()
            H.state.fps = host['fps']
            H.exp_tick()
        elif a[0] == 'disable':
            H.exp_all_off()
        elif a[0] == 'enable':
            H.restore_exp()
        elif a[0] == 'unload':
            H.disable()
        else:
            sys.exit('FAIL  in.txt: unknown action %r' % s['action'])
        # The memory the test compares: the image where it isn't the dump, the fakes, the addon's allocations
        chunks = runs(original, ctypes.string_at(base, len(dump)), base)
        for a0, n in fakes:
            chunks.append((a0, ctypes.string_at(a0, n)))
        for a0, n in sorted(allocs):
            chunks.append((a0, ctypes.string_at(a0, n)))
        texts = [str(t) for t in helpers(H, names).values()]
        name = s['action'].encode()
        out.write(struct.pack('<II', i, len(name)) + name)
        out.write(struct.pack('<I', len(chunks)))
        for a0, b in chunks:
            out.write(struct.pack('<II', a0, len(b)) + b)
        out.write(struct.pack('<I', len(texts)))
        for t in texts:
            tb = t.encode('utf-8')
            out.write(struct.pack('<I', len(tb)) + tb)
    out.close()
    print('ok    the addon ran %d steps on %s' % (len(cfg['steps']), os.path.basename(dump_path)))


if __name__ == '__main__':
    main()
