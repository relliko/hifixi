"""
Embeds a text file in C++ as a string literal (raw-string pieces of at most 12000 characters, under
MSVC's 16380 per piece, concatenated by the compiler):

    python tools/embed.py src/render/grass.hlsl build/engine/grass_hlsl.inc

Used by build_engine.cmd for the grass shaders: src/render/grass_render.cpp has `const char kShader[] =`
then #include "grass_hlsl.inc". The World FX add-on can also reload them from worldfx.hlsl next to it while
the game runs (../reshade-worldfx/src/grass.cpp), so this is the built-in fallback.
"""

import sys

DELIM = 'HLSL'


def main():
    src, out = sys.argv[1], sys.argv[2]
    text = open(src, encoding='utf-8').read()
    if (')' + DELIM + '"') in text:
        sys.exit('the source contains the raw-string delimiter')
    pieces, cur = [], ''
    for line in text.splitlines(keepends=True):
        if len(cur) + len(line) > 12000 and cur:
            pieces.append(cur)
            cur = ''
        cur += line
    pieces.append(cur)
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('// Made by tools/embed.py from %s: do not edit\n' % src.replace('\\', '/'))
        for p in pieces:
            f.write('R"%s(%s)%s"\n' % (DELIM, p, DELIM))


if __name__ == '__main__':
    main()
