r"""Reference outputs for hifixi-texpack make's C++ ports (tools\texpack\keeptexels.cpp, upscale.cpp's Lanczos), made
once from the real scripts and checked in as tests\data\texmake_ref.bin, so texpack_test needs no Python for them:
  - keeptexels: 20 made-up textures (an original of painted blocks, gradients and hard edges; its "upscale" Pillow's
    Lanczos of it with noise), 12 at 4x and 8 at 2x, run through research\ffxi-re\tools\upscale\keeptexels.py itself
    (as a program, on PNG folders, as bakeall.py runs it);
  - Lanczos: Pillow's Image.resize(LANCZOS) as upscale.py calls it: the colour 4x -> 2x (RGB) and the alpha 1x -> 2x
    and 4x (L), on random pictures.
  python -I tests\texmake_oracle.py [out file]     (default tests\data\texmake_ref.bin)
Format (little-endian): 'KTX1', u32 count, then per case u32 w, h, s, the original (w*h*4), the upscale and the
script's output ((w*s)*(h*s)*4 each); 'LNZ1', u32 count, then per case u32 w, h, c, W, H, the input (w*h*c), Pillow's
output (W*H*c)."""
import os, struct, subprocess, sys, tempfile
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
KEEPTEXELS = r'C:\code\LSB\research\ffxi-re\tools\upscale\keeptexels.py'
out_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, 'data', 'texmake_ref.bin')
rng = np.random.RandomState(7)


def painted(w, h):
    """An original texture: blocks of painted colour, a gradient, mottling, a hard diagonal edge; alpha in the game's
    0..0x80 with a cut-out patch."""
    img = np.zeros((h, w, 4), np.float64)
    bs = rng.choice([2, 4])
    for y in range(0, h, bs):
        for x in range(0, w, bs):
            img[y:y + bs, x:x + bs, :3] = rng.randint(30, 220, 3)
    yy, xx = np.mgrid[0:h, 0:w]
    img[..., :3] += (xx / max(w - 1, 1) * 40)[..., None]
    img[..., :3] += rng.randint(-12, 13, (h, w, 3))
    edge = (xx + yy * rng.uniform(0.5, 1.5)) > w * 0.6
    img[edge, :3] = img[edge, :3] * 0.3 + 160
    img[..., 3] = 0x80
    img[(xx < w // 3) & (yy < h // 3), 3] = 0
    return np.clip(img, 0, 255).astype(np.uint8)


def upscaled(o, s):
    """A stand-in for the AI's upscale: Lanczos of the original with noise."""
    h, w = o.shape[:2]
    big = np.asarray(Image.fromarray(o, 'RGBA').resize((w * s, h * s), Image.LANCZOS)).astype(np.int32)
    big[..., :3] += rng.randint(-20, 21, big[..., :3].shape)
    return np.clip(big, 0, 255).astype(np.uint8)


sizes = [(8, 8), (12, 8), (16, 16), (8, 16), (20, 12), (16, 24), (24, 24), (4, 8), (32, 16), (16, 32)]
cases = []
for i in range(20):
    w, h = sizes[i % len(sizes)]
    s = 4 if i < 12 else 2
    o = painted(w, h)
    cases.append((w, h, s, o, upscaled(o, s)))

blob = bytearray(b'KTX1') + struct.pack('<I', len(cases))
with tempfile.TemporaryDirectory() as tmp:
    d_orig, d_up, d_out = (os.path.join(tmp, n) for n in ('orig', 'up', 'out'))
    for d in (d_orig, d_up):
        os.makedirs(d)
    for i, (w, h, s, o, u) in enumerate(cases):
        Image.fromarray(o, 'RGBA').save(os.path.join(d_orig, '%02d.png' % i))
        Image.fromarray(u, 'RGBA').save(os.path.join(d_up, '%02d.png' % i))
    subprocess.run([sys.executable, KEEPTEXELS, d_orig, d_up, d_out], check=True, stdout=subprocess.DEVNULL)
    for i, (w, h, s, o, u) in enumerate(cases):
        r = np.asarray(Image.open(os.path.join(d_out, '%02d.png' % i)).convert('RGBA'))
        blob += struct.pack('<III', w, h, s) + o.tobytes() + u.tobytes() + r.tobytes()

lanczos = []
for w, h in [(12, 8), (16, 16), (20, 12)]:
    rgb = rng.randint(0, 256, (h * 4, w * 4, 3)).astype(np.uint8)
    lanczos.append((w * 4, h * 4, 3, w * 2, h * 2, rgb, np.asarray(Image.fromarray(rgb).resize((w * 2, h * 2), Image.LANCZOS))))
    a = rng.randint(0, 0x81, (h, w)).astype(np.uint8)
    for f in (2, 4):
        lanczos.append((w, h, 1, w * f, h * f, a, np.asarray(Image.fromarray(a).resize((w * f, h * f), Image.LANCZOS))))
blob += b'LNZ1' + struct.pack('<I', len(lanczos))
for w, h, c, W, H, src, ref in lanczos:
    blob += struct.pack('<IIIII', w, h, c, W, H) + src.tobytes() + ref.tobytes()

os.makedirs(os.path.dirname(out_path), exist_ok=True)
open(out_path, 'wb').write(blob)
print('wrote %s: %d keeptexels cases, %d Lanczos cases, %d bytes' % (out_path, len(cases), len(lanczos), len(blob)))
