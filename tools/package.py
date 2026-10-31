"""
HiFi XI's release zip, laid out as Ashita's folder (unzipped there as it is):

    plugins/hifixi.dll              the plugin (build/hifixi.dll)
    bootloader/dxgi.dll             its starter (build/starter/dxgi.dll)
    bootloader/d3d8.dll             its Direct3D 8 translator (build/d3d8/d3d8.dll)
    bootloader/d3d8-shader-cache/   the translator's shaders compiled ahead (build/d3d8/d3d8-shader-cache/)
    bootloader/worldfx-sunlit.txt   which zones have open sky (data/)
    bootloader/worldfx-clearsky.txt each zone's clear-sky light by the hour (data/)
    docs/hifixi/AMD-FidelityFX-FSR1-LICENSE.txt, AMD-FidelityFX-FSR2-LICENSE.txt, AMD-FidelityFX-SDK-LICENSE.txt
                                    the MIT licences of AMD's code built in (the SDK's: FSR 3.1, frame generation)
    README.md

    python tools/package.py <version> [--out <folder>] [--plugin <dll>] [--starter <dll>] [--d3d8 <dll>] [--cache <folder>]

--plugin, --starter, --d3d8 and --cache: the exact binaries that were tested, in place of build/'s (a rebuild of the same source
differs in its timestamps).

Paths in the zip use forward slashes (Python's zipfile: PowerShell 5.1's Compress-Archive writes backslashes).
Nothing of the game's own data goes in: the zone files are made on each player's PC.
"""

import argparse
import hashlib
import os
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

FILES = [
    ('build/hifixi.dll', 'plugins/hifixi.dll'),
    ('build/starter/dxgi.dll', 'bootloader/dxgi.dll'),
    ('build/d3d8/d3d8.dll', 'bootloader/d3d8.dll'),
    ('data/worldfx-sunlit.txt', 'bootloader/worldfx-sunlit.txt'),
    ('data/worldfx-clearsky.txt', 'bootloader/worldfx-clearsky.txt'),
    ('src/upscale/fsr1/LICENSE.txt', 'docs/hifixi/AMD-FidelityFX-FSR1-LICENSE.txt'),
    ('src/upscale/fsr2/LICENSE.txt', 'docs/hifixi/AMD-FidelityFX-FSR2-LICENSE.txt'),
    ('src/framegen/ffx/LICENSE.txt', 'docs/hifixi/AMD-FidelityFX-SDK-LICENSE.txt'),
    ('README.md', 'README.md'),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('version')
    ap.add_argument('--out', default=os.path.join(ROOT, 'build', 'release'))
    ap.add_argument('--plugin')
    ap.add_argument('--starter')
    ap.add_argument('--d3d8')
    ap.add_argument('--cache', default=os.path.join(ROOT, 'build', 'd3d8', 'd3d8-shader-cache'))
    args = ap.parse_args()
    files = [(os.path.join(ROOT, src), dst) for src, dst in FILES]
    files = [(args.plugin if dst == 'plugins/hifixi.dll' and args.plugin else args.starter if dst == 'bootloader/dxgi.dll' and args.starter else
              args.d3d8 if dst == 'bootloader/d3d8.dll' and args.d3d8 else src, dst)
             for src, dst in files]
    # (the translator's shader cache: every file in the folder)
    if os.path.isdir(args.cache):
        for name in sorted(os.listdir(args.cache)):
            full = os.path.join(args.cache, name)
            if os.path.isfile(full):
                files.append((full, 'bootloader/d3d8-shader-cache/' + name))
    os.makedirs(args.out, exist_ok=True)
    path = os.path.join(args.out, 'hifixi-v%s.zip' % args.version.lstrip('v'))
    missing = [src for src, _ in files if not os.path.isfile(src)]
    if missing:
        sys.exit('missing: ' + ', '.join(missing))
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        for src, dst in files:
            z.write(src, dst)
    print(path)
    for src, dst in files:
        data = open(src, 'rb').read()
        print('  %-32s %9d  %s' % (dst, len(data), hashlib.sha256(data).hexdigest()[:16]))


if __name__ == '__main__':
    main()
