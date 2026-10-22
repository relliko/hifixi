"""The hifps addon itself (C:/Games/PhoenixXI/addons/hifps: hifps.lua with its sites.lua and sigs.lua) run under
LuaJIT (lupa) against an FFXiMain dump, for tests/hifps_step_test.cpp to diff HiFi XI's port with: what it resolves,
what it patches (byte for byte) and what it writes each frame. As the addon's own tools/check_client.py does it: the
dump mapped at its own base in this 64-bit process, ashita.memory reading and writing it.

Usage: python tests/hifps_golden.py <FFXiMain_XXXXXXXX.bin> [--write <rva>:<hex>]... [--t0 <seconds>]
                                    [--frames <seconds>,<seconds>,...]

Set up as the C++ test sets up its mapped copy:
  - the game object (the fps divisor's +30h, the frame count's +34h) faked at 2F000000 (divisor 1, frame count
    00012345), the global that points at it written so;
  - --write puts bytes into the mapped copy (a scenario: sites hooked, a loop changed...), after that;
  - the addon's memory handed out from 30000000 (64 KB apart, as game.cpp's alloc_code in the tests);
  - its clock: --t0 when it loads (enable), then each of --frames for a present; its frame limit 0 (no waits) and no
    counter drawn.
Prints, one per line: 'sig' (sigs.lua as loaded), 'callers'/'map'/'extra' (sites.lua's resolve), 'chat' (what it
said), 'on', 'getter', 'site', 'imm', 'op', 'pulse', 'divisor', 'patch' (the image's changed byte runs after load),
'mem' (its 144 bytes), per frame 'frame <i> mem' and 'frame <i> imm', 'fps', and after unload 'unload', the image's
byte runs that differ from before load ('unloaded') and the memory it freed ('dealloc').
"""
import ctypes, ctypes.wintypes as wt, os, re, struct, sys, tempfile
from lupa import luajit21

HIFPS = 'C:/Games/PhoenixXI/addons/hifps'
LIBS = 'C:/Games/PhoenixXI/addons/libs'
FAKE, ARENA = 0x2F000000, 0x30000000
FRAME_COUNT = 0x00012345

path = sys.argv[1]
args = sys.argv[2:]
writes, t0, frames = [], 1000.0, []
while args:
    if args[0] == '--write':
        rva, hexs = args[1].split(':')
        writes.append((int(rva, 16), bytes.fromhex(hexs)))
    elif args[0] == '--t0':
        t0 = float(args[1])
    elif args[0] == '--frames':
        frames = [float(x) for x in args[1].split(',') if x]
    else:
        sys.exit('unknown argument ' + args[0])
    args = args[2:]

BASE = int(re.search(r'_([0-9A-Fa-f]{8})\.bin$', path).group(1), 16)
img = open(path, 'rb').read()

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.VirtualAlloc.restype = ctypes.c_void_p
k32.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, wt.DWORD, wt.DWORD]
def alloc_at(at, size):
    p = k32.VirtualAlloc(at, size, 0x3000, 0x40)
    assert p == at, 'address %08X not free' % at
    ctypes.memset(at, 0, size)
alloc_at(BASE, (len(img) + 0xFFFF) & ~0xFFFF)
ctypes.memmove(BASE, img, len(img))
alloc_at(FAKE, 0x10000)
alloc_at(ARENA, 0x100000)
def rd(a, n): return bytes((ctypes.c_char * n).from_address(a))
def wr(a, b): ctypes.memmove(a, bytes(b), len(b))

def find(pat, n, off=0):
    rx = re.compile(b''.join(b'.' if t == '??' else re.escape(bytes([int(t, 16)])) for t in re.findall('..', pat)), re.S)
    hits = []
    data = rd(BASE, len(img))
    for m in rx.finditer(data):
        hits.append(BASE + m.start())
        if len(hits) > n:
            break
    return hits[n] + off if n < len(hits) else 0

# The game object, faked
dp = find('81EC000100003BC174218B0D', 0)
gp = struct.unpack('<I', rd(dp + 0x0C, 4))[0]
wr(gp, struct.pack('<I', FAKE))
wr(FAKE + 0x30, struct.pack('<I', 1))
wr(FAKE + 0x34, struct.pack('<I', FRAME_COUNT))
for rva, b in writes:
    wr(BASE + rva, b)
before = rd(BASE, len(img))

lua = luajit21.LuaRuntime(unpack_returned_tuples=True)
lua.execute("package.path = [[%s/?.lua;%s/?.lua;]] .. package.path" % (HIFPS, LIBS))
alloc_next = [ARENA]
def alloc(n):
    r = alloc_next[0]
    alloc_next[0] += (int(n) + 0xFFFF) & ~0xFFFF
    return r
clock = [t0]
freed = []
install = tempfile.mkdtemp(prefix='hifps_golden_')  # (its settings would go here: none are read or written)
lua.globals().py = lua.table_from({'find': lambda pat, off, n: find(pat, int(n), int(off)), 'alloc': alloc,
                                    'dealloc': lambda a: freed.append(int(a) & 0xFFFFFFFF),
                                    'base': BASE, 'size': len(img), 'install': install,
                                    'now': lambda: clock[0]})
events, chatlog = lua.execute(r'''
local ffi = require('ffi');
local events, chatlog = {}, {};
local function ua(a) a = tonumber(a); return a < 0 and a + 4294967296 or a; end
local function P(a, t) return ffi.cast(t .. '*', ffi.cast('uintptr_t', ua(a))); end
ashita = {
    events = { register = function (ev, name, fn) events[ev] = fn; end },
    fs = { exists = function () return false; end, create_dir = function () return true; end },
    memory = {
        find = function (m, s, pat, off, n) return py.find(pat, off, n); end,
        get_base = function () return py.base; end,
        get_size = function () return py.size; end,
        read_uint8 = function (a) return P(a, 'uint8_t')[0]; end,
        read_uint32 = function (a) return P(a, 'uint32_t')[0]; end,
        read_int32 = function (a) return P(a, 'int32_t')[0]; end,
        read_float = function (a) return tonumber(P(a, 'float')[0]); end,
        read_array = function (a, n) local q = P(a, 'uint8_t'); local r = {}; for i = 1, n do r[i] = q[i - 1]; end return r; end,
        write_array = function (a, t) local q = P(a, 'uint8_t'); for i = 1, #t do q[i - 1] = t[i]; end end,
        write_float = function (a, v) P(a, 'float')[0] = v; end,
        write_uint8 = function (a, v) P(a, 'uint8_t')[0] = v; end,
        write_uint32 = function (a, v) P(a, 'uint32_t')[0] = ua(v) % 4294967296; end,
        unprotect = function () return true, 0x40; end,
        protect = function () return true; end,
        alloc = function (n) return py.alloc(n); end,
        dealloc = function (a) py.dealloc(a); end,
    },
};
addon = {};
package.loaded['chat'] = {
    header = function (n) local s = {}; function s:append(x) table.insert(chatlog, x); return self; end return s; end,
    message = function (s) return s; end, error = function (s) return 'ERROR: ' .. s; end, success = function (s) return s; end,
};
package.loaded['imgui'] = setmetatable({}, { __index = function () return function () return true; end; end });
_G.print = function (x) end
require('common');
AshitaCore = { GetInstallPath = function () return py.install .. '\\'; end };
dofile(... .. '/hifps.lua');
return events, chatlog;
''', HIFPS)

def upvalue(fn, name):
    return lua.execute('''
local fn, name = ...;
local i = 1;
while true do
    local n, v = debug.getupvalue(fn, i);
    if (n == nil) then return nil, 0; end
    if (n == name) then return v, i; end
    i = i + 1;
end''', fn, name)
present = events['d3d_present']
STATE, _ = upvalue(present, 'state')
_, now_i = upvalue(present, 'now')
lua.execute('local fn, i = ...; debug.setupvalue(fn, i, function () return py.now(); end)', present, now_i)

out = []
SIGS = lua.eval("require('sigs')")
for rva in sorted(SIGS.keys()):
    s = SIGS[rva]
    out.append('sig %06X %s %d %s' % (rva, s[1], s[2], s[3]))

# sites.lua's resolve, as enable() calls it
getters = [find('8B0D????????D94128D81D????????DFE0F6C4057A07D905????????C3D94128C3', i) for i in range(4)]
getters = sorted(g for g in getters if g)
if len(getters) == 2:
    LOCATE = lua.eval("require('sites')")
    found = lua.execute('''
local L, base, g1, g2 = ...;
local ffi = require('ffi');
return L.resolve(ffi.cast('const uint8_t*', ffi.cast('uintptr_t', base)), { g1, g2 });
''', LOCATE, BASE, getters[0] - BASE, getters[1] - BASE)
    out.append('callers %d' % found.callers)
    for old in sorted(found.map.keys()):
        out.append('map %06X %06X' % (old, found.map[old]))
    for i in range(1, len(found.extra) + 1):
        out.append('extra %06X' % found.extra[i])

events['load']()
lua.execute('local fn, i = ...; debug.setupvalue(fn, i, false)', present, upvalue(present, 'restore_pending')[1])
on = STATE.mem is not None
out.append('on %d' % (1 if on else 0))
def u(v): return int(v) & 0xFFFFFFFF
if on:
    STATE.limit = 0
    STATE.counter = False
    for i in range(1, len(STATE.getters) + 1):
        out.append('getter %08X' % u(STATE.getters[i].addr))
    for i in range(1, len(STATE.sites) + 1):
        s = STATE.sites[i]
        out.append('site %08X %s %08X' % (u(s.addr), s.kind, u(s.target)))
    for i in range(1, len(STATE.smooth) + 1):
        out.append('imm %08X' % u(STATE.smooth[i].addr))
    for i in range(1, len(STATE.ops) + 1):
        o = STATE.ops[i]
        out.append('op %08X %08X %s %08X' % (u(o.addr), u(o.const), {'ease': 'e', 'lin': 'l', 'pow': 'p'}[o.mode], u(o.slot)))
    out.append('pulse %08X' % (u(STATE.pulse.addr) if STATE.pulse is not None else 0))
    out.append('divisor %08X %08X' % (u(STATE.div_ptr), u(STATE.div_orig)))
    after = rd(BASE, len(img))
    i = 0
    while i < len(img):
        if after[i] != before[i]:
            j = i
            while j < len(img) and after[j] != before[j]:
                j += 1
            out.append('patch %08X %s' % (BASE + i, after[i:j].hex().upper()))
            i = j
        else:
            i += 1
    mem = u(STATE.mem)
    out.append('mem %08X %s' % (mem, rd(mem, 144).hex().upper()))
    for n, t in enumerate(frames):
        clock[0] = t
        present()
        out.append('frame %d mem %s' % (n, rd(mem, 144).hex().upper()))
        for i in range(1, len(STATE.smooth) + 1):
            a = u(STATE.smooth[i].addr)
            out.append('frame %d imm %08X %s' % (n, a, rd(a, 4).hex().upper()))
    out.append('fps %.17g' % STATE.fps)
    events['unload']()
    after = rd(BASE, len(img))
    out.append('unload')
    i = 0
    while i < len(img):
        if after[i] != before[i]:
            j = i
            while j < len(img) and after[j] != before[j]:
                j += 1
            out.append('unloaded %08X %s' % (BASE + i, after[i:j].hex().upper()))
            i = j
        else:
            i += 1
    for a in freed:
        out.append('dealloc %08X' % a)
for i in range(1, len(chatlog) + 1):
    out.append('chat ' + chatlog[i])
try:
    os.rmdir(install)
except OSError:
    pass
sys.stdout.write('\n'.join(out) + '\n')
