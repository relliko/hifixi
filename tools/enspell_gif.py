"""enspell_gif: tests/enspell_view.cpp's frames (frame000.bmp, ...) into a looping GIF at 30 frames a second.

    python tools/enspell_gif.py <frames folder> <out.gif> [width]
"""
import glob
import os
import sys

from PIL import Image


def main():
    folder, out = sys.argv[1], sys.argv[2]
    width = int(sys.argv[3]) if len(sys.argv) > 3 else 640
    names = sorted(glob.glob(os.path.join(folder, 'frame*.bmp')))
    if not names:
        sys.exit('no frames in %s' % folder)
    frames = []
    for name in names:
        im = Image.open(name).convert('RGB')
        frames.append(im.resize((width, im.height * width // im.width), Image.LANCZOS))
    frames[0].save(out, save_all=True, append_images=frames[1:], duration=33, loop=0)
    print('%d frames -> %s (%.1f MB)' % (len(frames), out, os.path.getsize(out) / 1e6))


if __name__ == '__main__':
    main()
