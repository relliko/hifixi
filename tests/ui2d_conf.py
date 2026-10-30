"""The game's dgVoodoo.conf for tests/ui2d_dgv.cpp: as the game has it (its filtering and the rest), but drawn by WARP
through Direct3D 11 (OutputAPI d3d11warp: no GPU) and windowed.

    python tests/ui2d_conf.py <the game's dgVoodoo.conf> <out>
"""
import re
import sys


def main():
    src, out = sys.argv[1], sys.argv[2]
    text = open(src, encoding='latin-1').read()
    text = re.sub(r'(?m)^(OutputAPI\s*=\s*).*$', r'\g<1>d3d11warp', text)
    text = re.sub(r'(?m)^(FullScreenMode\s*=\s*).*$', r'\g<1>false', text)
    text = re.sub(r'(?m)^(dgVoodooWatermark\s*=\s*).*$', r'\g<1>false', text)
    open(out, 'w', encoding='latin-1', newline='\r\n').write(text.replace('\r\n', '\n'))


if __name__ == '__main__':
    main()
