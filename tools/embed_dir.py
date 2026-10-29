"""
Embeds every file of a folder with the given extensions in C++ as { "name", "text" } pairs (each text in raw-string
pieces of at most 12000 characters, as embed.py makes them), for an array of { const char *name, *text; }:

    python tools/embed_dir.py src/upscale/fsr2/shaders build/gen/fsr2_shaders.inc .h .hlsl

Used by build.cmd for FSR 2's shaders (src/upscale/fsr2_shaders.cpp serves them to D3DCompile's #include lines).
"""

import os
import sys

DELIM = 'HLSL'


def pieces(text):
    out, cur = [], ''
    for line in text.splitlines(keepends=True):
        if len(cur) + len(line) > 12000 and cur:
            out.append(cur)
            cur = ''
        cur += line
    out.append(cur)
    return out


def main():
    folder, out, exts = sys.argv[1], sys.argv[2], tuple(sys.argv[3:])
    names = sorted(n for n in os.listdir(folder) if n.endswith(exts))
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('// Made by tools/embed_dir.py from %s: do not edit\n' % folder.replace('\\', '/'))
        for name in names:
            text = open(os.path.join(folder, name), encoding='utf-8').read()
            if (')' + DELIM + '"') in text:
                sys.exit('%s contains the raw-string delimiter' % name)
            f.write('{ "%s",\n' % name)
            for p in pieces(text):
                f.write('R"%s(%s)%s"\n' % (DELIM, p, DELIM))
            f.write('},\n')


if __name__ == '__main__':
    main()
