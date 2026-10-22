"""
drawcull's own numbers for HiFi XI's drawcull test (tests/drawcull_test.cpp): drawcull.lua (1.5) run under lupa's
LuaJIT against each FFXiMain dump mapped at its own base, as drawcull's tests/t_square.py runs it (ashita.memory on
real memory here; the addon's memory from 0x30000000, as the C++ test has its). Where a dump holds patches
(FFXiMain_041C0000.bin: drawcull 1.4's culling call, mesh detail sites and visibility read), the game's own bytes are
put back in the mapped copy first, so the addon patches what it would patch in a fresh game.

Prints what the C++ test holds as its golden values: for each dump, drawcull's stub memory (160 bytes) and the bytes it
writes at each site; the world and entity multipliers and the mesh detail factor for a list of settings, as bits.

    python tests/drawcull_golden.py
"""

import ctypes
import os
import re
import struct
import sys

from lupa import luajit21

LIBS = r'C:\Games\PhoenixXI\addons\libs'
ADDONS = r'C:\Games\PhoenixXI\addons'
DUMPS = [r'C:\Games\PhoenixXI\FFXiMain_04490000.bin', r'C:\Games\PhoenixXI\FFXiMain_041C0000.bin']
ALLOC_AT = 0x30000000
GAME_GLOBAL = 0x4568FC      # the global holding gGame (image offset)
SCALE_FN = 0x1892B0         # the culling call's target: the client's distance scale
CULL_SITE = 0x17BF53        # call <scale> (image offset)
LOD_SITES = [0x17CB69, 0x17CB81, 0x17DFDE, 0x17DFFA]
LOD_STOCK = ['D9442414D899CC000000', 'D9442414D899C8000000', 'D9442458D89FCC000000', 'D9442458D89FC8000000']
VIS_SITE = 0x17C0E5
VIS_STOCK = '8B852C910300'

WORLD_LOD = [(1, 0), (10, 0), (3, 0), (2.5, 0), (1.3, 0), (0.7, 0), (7.77, 0), (5, 2), (5, 0.5), (5, 1.5), (1, 12.34), (0.3, 1.7)]
ENTITY = [1, 3, 0.25, 1.3, 7.77]

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.VirtualAlloc.restype = ctypes.c_void_p
k32.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_uint32]
k32.VirtualFree.restype = ctypes.c_int
k32.VirtualFree.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32]


def map_at(addr, size):
    p = k32.VirtualAlloc(addr, size, 0x3000, 0x40)
    if p != addr:
        raise SystemExit("can't map 0x%08X here (got %r, error %d)" % (addr, p, ctypes.get_last_error()))


def unmap(addr):
    k32.VirtualFree(addr, 0, 0x8000)


def rd(addr, n): return ctypes.string_at(addr, n)
def u32(addr): return struct.unpack('<I', rd(addr, 4))[0]
def put(addr, b): ctypes.memmove(addr, b, len(b))
def put32(addr, v): put(addr, struct.pack('<I', v & 0xFFFFFFFF))


def find(image, pattern):
    want = [None if pattern[i:i + 2] == '??' else int(pattern[i:i + 2], 16) for i in range(0, len(pattern), 2)]
    rx = b''.join(b'.' if b is None else re.escape(bytes([b])) for b in want)
    m = re.search(rx, image, re.S)
    return None if m is None else m.start()


def lua_runtime(base, size, extra=''):
    lua = luajit21.LuaRuntime(unpack_returned_tuples=True)
    g = lua.globals()
    allocs = []

    def mem_find(module, sz, pattern, offset, usage):
        o = find(rd(base, size), pattern)
        return 0 if o is None else base + o + offset

    def mem_alloc(n):
        at = ALLOC_AT + sum((s + 0xFF) & ~0xFF for s in allocs)
        allocs.append(n)
        return at

    g.py_find = mem_find
    g.py_alloc = mem_alloc
    lua.execute(r'''
        package.path = [[%s\?.lua;]] .. package.path
        local ffi = require('ffi')
        printed = {}
        print = function (s) printed[#printed + 1] = tostring(s); end
        events = {}
        addon = {}
        ashita = {
            events = { register = function (name, id, fn) events[name] = fn; end },
            memory = {
                find = function (...) return py_find(...); end,
                alloc = function (n) return py_alloc(n); end,
                unprotect = function (a, n) return true, 0x40; end,
                protect = function (a, n, p) return true; end,
                read_uint32 = function (a) return tonumber(ffi.cast('uint32_t*', ffi.cast('uintptr_t', a))[0]); end,
                read_int32 = function (a) return tonumber(ffi.cast('int32_t*', ffi.cast('uintptr_t', a))[0]); end,
                write_uint32 = function (a, v) ffi.cast('uint32_t*', ffi.cast('uintptr_t', a))[0] = v; end,
                write_float = function (a, v) ffi.cast('float*', ffi.cast('uintptr_t', a))[0] = v; end,
                read_array = function (a, n)
                    local t = {}; local p = ffi.cast('uint8_t*', ffi.cast('uintptr_t', a))
                    for i = 0, n - 1 do t[i + 1] = p[i]; end
                    return t;
                end,
                write_array = function (a, t)
                    local p = ffi.cast('uint8_t*', ffi.cast('uintptr_t', a))
                    for i = 1, #t do p[i - 1] = t[i]; end
                end,
            },
        }
        saves = 0
        package.loaded['settings'] = {
            load = function (d) local t = T{}; for k, v in pairs(d) do t[k] = v; end; return t; end,
            save = function () saves = saves + 1; end,
            register = function () end,
        }
        function command(s) local e = { command = s, blocked = false }; events['command'](e); return e.blocked; end
        %s
    ''' % (LIBS, extra))
    return lua, g


def hexs(b): return b.hex().upper()


def num(v):
    # (as the C++ test's options hold it: the shortest text, as typed)
    return repr(float(v)).rstrip('0').rstrip('.') if isinstance(v, float) else str(v)


def golden(path):
    base = int(re.search(r'_([0-9A-Fa-f]{8})\.bin$', path).group(1), 16)
    dump = open(path, 'rb').read()
    size = len(dump)
    map_at(base, size)
    put(base, dump)
    map_at(ALLOC_AT, 0x20000)
    put(ALLOC_AT, bytes(0x20000))
    game = ALLOC_AT + 0x10000
    put32(base + GAME_GLOBAL, game)
    put32(game + 0x2E8, 0x3F4A7EFA)
    put32(game + 0x2F0, 0x3F800000)
    # The game's own bytes where the dump holds patches
    put(base + CULL_SITE, b'\xE8' + struct.pack('<i', (base + SCALE_FN) - (base + CULL_SITE + 5)))
    for site, stock in zip(LOD_SITES, LOD_STOCK):
        put(base + site, bytes.fromhex(stock))
    put(base + VIS_SITE, bytes.fromhex(VIS_STOCK))
    out = {'base': base}

    # drawcull: load with its defaults, then each setting by its own command
    lua, g = lua_runtime(base, size)
    lua.execute('dofile([[%s]])' % os.path.join(ADDONS, 'drawcull', 'drawcull.lua'))
    g.events['load']()
    out['stubs'] = hexs(rd(ALLOC_AT, 160))
    out['cull'] = hexs(rd(base + CULL_SITE, 5))
    out['lod'] = [hexs(rd(base + s, 10)) for s in LOD_SITES]
    out['vis'] = hexs(rd(base + VIS_SITE, 6))
    world = u32(base + 0x456AB8)
    rows = []
    for w, l in WORLD_LOD:
        g.command('/drawcull world %s' % num(w))
        g.command('/drawcull detail %s' % num(l))
        rows.append((num(w), num(l), u32(base + 0x456AB8), u32(ALLOC_AT + 32)))
    out['world_lod'] = rows
    rows = []
    for e in ENTITY:
        g.command('/drawcull entity %s' % num(e))
        rows.append((num(e), u32(base + 0x456AA8)))
    out['entity'] = rows
    g.command('/drawcull square on')
    out['square'] = u32(game + 0x2E8)
    g.command('/drawcull frustum on')
    out['frustum'] = hexs(rd(base + 0x17BF75, 6))
    g.events['unload']()

    unmap(ALLOC_AT)
    unmap(base)
    return out


def main():
    results = [golden(p) for p in DUMPS]
    for r in results:
        print('// base 0x%08X' % r['base'])
        print('stubs   %s' % r['stubs'])
        print('cull    %s' % r['cull'])
        for i, s in enumerate(r['lod']):
            print('lod%d    %s' % (i + 1, s))
        print('vis     %s' % r['vis'])
        print('frustum %s' % r['frustum'])
        print('square  0x%08X' % r['square'])
    # (the same for every dump: they're numbers, not addresses)
    same = all(r['world_lod'] == results[0]['world_lod'] and r['entity'] == results[0]['entity'] for r in results)
    print('// world, lod -> world bits, detail factor bits%s' % ('' if same else ' (DIFFER BETWEEN DUMPS)'))
    for w, l, wb, kb in results[0]['world_lod']:
        print('{ "%s", "%s", 0x%08X, 0x%08X },' % (w, l, wb, kb))
    print('// entity -> bits')
    for e, eb in results[0]['entity']:
        print('{ "%s", 0x%08X },' % (e, eb))
    return 0 if same else 1


if __name__ == '__main__':
    sys.exit(main())
