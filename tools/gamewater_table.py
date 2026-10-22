"""
Makes src/game/gamewater_meshes.inc from the worldfx Ashita addon's gamewater.lua (made by reshade-worldfx's
tools/game_water.py: each zone's water meshes, the game's own water layers lying on the water World FX draws), for
src/game/gamewater.cpp, which includes it in its table:

    python tools/gamewater_table.py [--lua ..\\reshade-worldfx\\ashita\\worldfx\\gamewater.lua] [--out src\\game\\gamewater_meshes.inc]

Each zone a line: { zone, names, "name" "name" ... }, every name exactly 4 bytes (spaces kept: 'ka2 '), bytes that
aren't printable ASCII as octal escapes. A name that isn't 4 bytes long is left out (the addon skipped it), and said.
The file is read as bytes (Latin-1), as Lua reads it.
"""

import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LUA = os.path.join(ROOT, '..', 'reshade-worldfx', 'ashita', 'worldfx', 'gamewater.lua')
OUT = os.path.join(ROOT, 'src', 'game', 'gamewater_meshes.inc')

ENTRY = re.compile(r'^\s*\[(\d+)\]\s*=\s*\{(.*)\}\s*,?\s*$')
SIMPLE_ESCAPES = {'n': '\n', 't': '\t', 'r': '\r', 'a': '\a', 'b': '\b', 'f': '\f', 'v': '\v', '\\': '\\', "'": "'", '"': '"'}


def lua_strings(text, where):
    """The strings of a Lua list ('a', "b", ...): its quoted strings with their escapes undone"""
    out, i = [], 0
    while i < len(text):
        c = text[i]
        if c in ' \t,':
            i += 1
            continue
        if c not in '\'"':
            sys.exit('%s: not a string at "%s"' % (where, text[i:i + 20]))
        quote, i, s = c, i + 1, ''
        while True:
            if i >= len(text):
                sys.exit('%s: a string not closed' % where)
            c = text[i]
            if c == quote:
                i += 1
                break
            if c != '\\':
                s += c
                i += 1
                continue
            e = text[i + 1:i + 2]
            if e.isdigit():
                digits = re.match(r'\d{1,3}', text[i + 1:]).group(0)
                if int(digits) > 255:
                    sys.exit('%s: escape \\%s past a byte' % (where, digits))
                s += chr(int(digits))
                i += 1 + len(digits)
            elif e == 'x' and re.match(r'[0-9A-Fa-f]{2}', text[i + 2:i + 4] or ''):
                s += chr(int(text[i + 2:i + 4], 16))
                i += 4
            elif e in SIMPLE_ESCAPES:
                s += SIMPLE_ESCAPES[e]
                i += 2
            else:
                sys.exit('%s: escape \\%s not known' % (where, e))
        out.append(s)
    return out


def read_table(path):
    """zone -> its names, as the Lua table has them"""
    text = open(path, 'rb').read().decode('latin1')
    table, opened, closed = {}, False, False
    for n, line in enumerate(text.splitlines(), 1):
        where = '%s:%d' % (os.path.basename(path), n)
        s = line.strip()
        if s == '' or s.startswith('--'):
            continue
        if s == 'return {':
            opened = True
            continue
        if s in ('};', '}'):
            closed = True
            continue
        m = ENTRY.match(line)
        if m is None or not opened or closed:
            sys.exit('%s: not a zone\'s entry: %s' % (where, s))
        zone = int(m.group(1))
        if zone in table or zone > 0xFFFF:
            sys.exit('%s: zone %d twice or past 65535' % (where, zone))
        table[zone] = lua_strings(m.group(2), where)
    if not opened or not closed:
        sys.exit('%s: not a table (return { ... })' % path)
    return table


def c_name(name):
    return '"' + ''.join(c if 0x20 <= ord(c) < 0x7F and c not in '"\\?' else '\\%03o' % ord(c) for c in name) + '"'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--lua', default=LUA)
    ap.add_argument('--out', default=OUT)
    args = ap.parse_args()
    table = read_table(args.lua)
    lines = ['// Made by tools/gamewater_table.py from reshade-worldfx/ashita/worldfx/gamewater.lua: do not edit',
             '// { zone, names, its water meshes\' names (4 bytes each) }']
    names_total = 0
    for zone in sorted(table):
        names = []
        for n in table[zone]:
            if len(n) == 4:
                names.append(n)
            else:
                print('zone %d: %r left out (not 4 bytes long: the addon skipped it)' % (zone, n), file=sys.stderr)
        names_total += len(names)
        lines.append('{ %d, %d, %s },' % (zone, len(names), ' '.join(c_name(n) for n in names) if names else '""'))
    with open(args.out, 'w', encoding='ascii', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    print('%s: %d zones, %d names' % (args.out, len(table), names_total))


if __name__ == '__main__':
    main()
