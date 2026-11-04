# The upscaler: Real-ESRGAN, fetched on the player's OK

`hifixi-texpack make` (docs/texture-pack-plan.md M7) makes the game's textures bigger with Real-ESRGAN's program,
run as a separate program: with the x4plus model in M7, with HDCube (shipped with the tool, below) by default since M8,
whose colour is BC7 by Microsoft's texconv.exe (also shipped, below). This page is the licence decision for them,
written down, and how make runs them.

## What it is

- **Program:** `realesrgan-ncnn-vulkan.exe`, the Windows build of Real-ESRGAN-ncnn-vulkan, with the model
  `realesrgan-x4plus` (`models\realesrgan-x4plus.bin` and `.param`) and `vcomp140.dll` (Microsoft's OpenMP runtime the
  program loads).
- **The file:** `realesrgan-ncnn-vulkan-20220424-windows.zip`, from Real-ESRGAN's release v0.2.5.0:
  `https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-windows.zip`
- **Size:** 45,474,481 bytes.
- **SHA-256:** `abc02804e17982a3be33675e4d471e91ea374e65b70167abc09e31acb412802d`.
- A copy unpacked for our own work is in `C:\code\LSB\research\ffxi-re\tools\upscale\esrgan` (its PROVENANCE.txt has
  the same URL, size and hash).

## Licences

| Part | Who | Licence |
|---|---|---|
| Real-ESRGAN (the method, the training code and the x4plus weights) | xinntao (Xintao Wang) | BSD-3-Clause |
| Real-ESRGAN-ncnn-vulkan (the program) | xinntao | MIT |
| ncnn (built into the program) | Tencent | BSD-3-Clause |
| vcomp140.dll | Microsoft (Visual C++ runtime) | the Visual C++ redistributable's terms |

Upstream: https://github.com/xinntao/Real-ESRGAN, https://github.com/xinntao/Real-ESRGAN-ncnn-vulkan,
https://github.com/Tencent/ncnn. The release zip itself holds no licence file (only `README_windows.md`, the samples,
the six model files and the two DLLs).

## The decision (Alex, 2026-11-03)

**The builder doesn't ship the upscaler.** `hifixi-texpack get-upscaler` downloads the official zip on the player's
consent and checks it. Why:

- The program, the weights and the bundled DLL come under three licences and one runtime's terms; shipping them in the
  hifixi-enhanced-textures package would make us a redistributor of each, with their notices to carry and keep right.
  Downloading the authors' own release, on the player's OK, leaves the distribution with the authors.
- The zip's `vcomp140d.dll` is Microsoft's *debug* OpenMP runtime, which may not be redistributed at all. It is never
  kept, so nothing of ours can pass it on.
- The hash pins the exact file we tested with. A changed release, a mirror or a damaged download is refused.
- The package stays small (no 45 MB of someone else's binaries), and holds no game data: packs are made on the
  player's PC from their own files (hifixi-enhanced-textures, M7's packaging).

## What get-upscaler does

`hifixi-texpack get-upscaler [--yes] [--dir <folder>]` (`tools\texpack\fetch.cpp`):

1. Says what it fetches: the file name, size, URL, the SHA-256 it will check, the licences above, what it keeps and where.
2. Asks `Download it now? [y/N]` unless `--yes`. Anything but y: nothing is downloaded (exit 1).
3. Downloads into `<folder>.part\download.zip` (urlmon's `URLDownloadToFileW`). The `.part` folder is marked as
   get-upscaler's (`made-by-hifixi-texpack-get-upscaler.txt`); one that is already there without the mark is never
   deleted: get-upscaler says so and stops (exit 1).
4. Refuses the file unless its size is 45,474,481 bytes and its SHA-256 is the one above (Windows' BCrypt); the file
   and the `.part` folder are deleted, nothing else changed (exit 1).
5. Unpacks it with Windows' own `C:\Windows\System32\tar.exe` into the `.part` folder.
6. Keeps only `realesrgan-ncnn-vulkan.exe`, `vcomp140.dll`, `models\realesrgan-x4plus.bin`, `models\realesrgan-x4plus.param`
   and `README_windows.md` (plus any `LICENSE*` or `NOTICE*` file a later zip might add), gathered first in
   `<folder>.part\kept` with `PROVENANCE.txt` (URL, date, size, hash, what was kept, the licences). All five must be
   there, or nothing changes (exit 1). Then the folder is put into place: moved whole when there is none yet, else each
   file copied over the old one's with `PROVENANCE.txt` last. The `.part` folder is deleted.

The folder is `<the tool's folder>\upscaler` unless `--dir` says otherwise; `make` looks there unless given
`--upscaler <folder>`, and when the program, its `vcomp140.dll` or the model is missing it says to run get-upscaler
and stops (exit 1).

`--url file:///...` (tests only) reads a local file in place of the download; the size and hash checked are still the
official file's, so only that file is ever accepted. texpack_test never downloads: it serves a wrong-size and a
wrong-hash file that way, and (when `HIFIXI_ESRGAN_ZIP` names a local copy of the official zip) checks what is kept.

## HDCube: the default model since M8 (Alex, 2026-11-03), shipped with the tool

- **Model:** 4x_HDCube_200k by Venomalia (an ESRGAN RRDBNet trained on GameCube/Wii DXT textures), licence
  **CC0-1.0** (OpenModelDB 4x-HDCube). From `https://raw.githubusercontent.com/Venomalia/HDcube/main/v1/4x_HDCube_200k.pth`
  (67,020,101 bytes, SHA-256 `c696eddb2fe313613978c4fe5a10a17e8670d85487e35b304d87fc16df4a7694`).
- **Converted** for realesrgan-ncnn-vulkan (20220424) on 2026-11-03 (spandrel, torch.jit.trace, pnnx fp16, blobs
  renamed to `data`/`output`): `realesrgan-hdcube-x4.param` (SHA-256
  `44b9db926798e1db80604bc66d92facfb4f7397a8eae7e6c93f4f71e30df793d`) and `realesrgan-hdcube-x4.bin`
  (`bceee5fdefe51110416c96bffe5d9e09b6b5751c635bcbe512252894afbbcd7b`). Our copy and its PROVENANCE.txt:
  `C:\code\LSB\research\ffxi-re\tools\upscale\hdcube`. Checked on the Yagudo's textures against PyTorch: mean abs
  0.27/255.
- **Run:** `realesrgan-ncnn-vulkan.exe -n realesrgan-hdcube-x4 -m <models> -s 4` (`upscale::run`, `-s 4` for every
  model). 4x only: zones at 2x are brought down with Lanczos as for x4plus (`upscale::finish`).
- **Shipped, not downloaded:** CC0 lets us pass it on. The tool's zip holds `models-hdcube\` beside
  `hifixi-texpack.exe` (the two files and PROVENANCE.txt). `get-upscaler` copies them into `upscaler\models\` after the
  Real-ESRGAN download (`place_hdcube`, `tools\texpack\fetch.cpp`: each file's SHA-256 checked against the two above,
  PROVENANCE.txt copied as `PROVENANCE-hdcube.txt`), so a player has it with no other download; `make` does the same
  when the upscaler folder lacks it. A `models-hdcube` file with another hash is refused (make exits 1 and says to
  unpack the zip again or use `--upscaler-model x4plus`).
- **Options:** `make --upscaler-model hdcube|x4plus` (default hdcube).

## BC7: texconv.exe, shipped with the tool

The pack's colour is `BC7_UNORM` since M8 (`make --colour bc7|bc2`, default bc7; docs/texture-pack/format.md "Mips").

- **Program:** `texconv.exe` from Microsoft's DirectXTex, release `may2026` (texconv 2026.5.8.1), signed by Microsoft:
  `https://github.com/microsoft/DirectXTex/releases/download/may2026/texconv.exe`, SHA-256
  `dcfdec10244e02cf5037fba089c55fb7e1326b1c8181742d77d15fa5cb5eef06`. Licence **MIT** (below). It ships beside
  `hifixi-texpack.exe` in the tool's zip; make looks for it there unless given `--texconv <file>`.
- **How make runs it** (`tools\texpack\bc7.cpp`): each texture's levels, our own chain on 8-bit texels
  (`bc7::rgba_chain`: coverage-correct cut-outs, alpha held to the original's raw range), are written as an
  `R8G8B8A8_UNORM` DDS with its mips into `<pack>\work\dds-in`; one texconv run per batch,
  `texconv -nologo -y -f BC7_UNORM -dx10 -aw 4 -o <work>\dds-out -flist <list>`, run in `<work>` with the list naming
  `dds-in\N.dds` relative to it (texconv reads the list without UTF-8, byte by byte, so a full path through a pack
  folder such as `C:\Users\José\...` would fail every batch; `-o` and `-flist` go on the wide command line), as a child process exactly as the
  upscaler is (`upscale::run_program`: a job object that ends it with make, below-normal priority, the console output to
  `<work>\texconv.txt`, the game and the stop file checked every `--game-poll-s`: the game starting ends it and the
  batch is made again once the game has been closed; a stall past `--stall-s` stops make). Given no `-m`, texconv keeps
  a DDS's own mips (texpack_test checks it with levels of different flat colours); make reads each file back and refuses
  one whose size, format or level count isn't ours. Never SRGB. texconv uses the graphics card (DirectCompute) and
  falls back to its CPU encoder by itself on a PC without one (about 130 times slower).
- **Missing texconv.exe:** make says so in one line ("texconv.exe isn't at ...: BC2 colour this run, not BC7") and
  makes BC2; the DATs are recorded as BC2 (maker 310), so the next run with texconv.exe makes them again in BC7.
- **The quality setting, measured 2026-11-04** on the trial's 61 model textures at 4x (RTX 4070 Ti; RGB PSNR of level
  0 against our RGBA8; alpha over every level; an alpha-test flip is a texel crossing 34 or 47.8 the other way):

  | encoder | us per texel | RGB PSNR | alpha mean / max error | alpha-test flips |
  |---|---|---|---|---|
  | BC2 (texconv's, for reference) | 0.011 | 37.33 dB | 0.95 / 8 | 0.090% |
  | BC7 GPU, default | 0.120 | 44.19 dB | 0.27 / 110 | 0.055% |
  | BC7 GPU, `-aw 2` | 0.120 | 43.89 dB | 0.21 / 64 | 0.048% |
  | **BC7 GPU, `-aw 4` (chosen)** | 0.121 | 43.57 dB | 0.17 / 64 | 0.044% |
  | BC7 GPU, `-aw 8` | 0.122 | 43.25 dB | 0.14 / 45 | 0.041% |
  | BC7 GPU, `-bc x` | 0.162 | 44.33 dB | 0.27 / 110 | 0.055% |
  | BC7 GPU, `-bc q` | 0.057 | 42.79 dB | 0.35 / 81 | 0.056% |

  On the tests' 10 textures the CPU encoder gave 50.02 dB against the GPU's 49.58 dB, at 23.3 against 0.18 us per
  texel. `-aw 4` (the alpha error weighted 4 times) costs 0.6 dB of colour (still 6 dB over BC2) and nearly halves the
  worst alpha error. BC7's alpha can land up to 6 levels past the original's top at sharp edges (BC2's own 4-bit alpha
  is up to 8 off), and it flips half as many alpha tests as BC2.

### texconv's licence (DirectXTex, MIT)

    The MIT License (MIT)

    Copyright (c) Microsoft Corporation.

    Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
    documentation files (the "Software"), to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
    permit persons to whom the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all copies or substantial portions of
    the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
    WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
    COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
    OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## The tool's zip (release layout)

    hifixi-texpack.exe
    texconv.exe                         (DirectXTex may2026, MIT: the notice above goes into the zip's notices)
    models-hdcube\realesrgan-hdcube-x4.param
    models-hdcube\realesrgan-hdcube-x4.bin
    models-hdcube\PROVENANCE.txt        (research\ffxi-re\tools\upscale\hdcube's)

`hifixi-texpack get-upscaler` then adds `upscaler\` (Real-ESRGAN's program, vcomp140.dll, x4plus, the readme and
PROVENANCE.txt, downloaded) and copies HDCube into `upscaler\models\`. `make` needs no options for either: the upscaler
is `<tool>\upscaler`, texconv `<tool>\texconv.exe`. Tried 2026-11-04 in `C:\code\LSB\hifi-textures\hd-tool`:
get-upscaler from the official zip (file://), then make on ROM\1\48, ROM\6\70 and ROM3\5\24 from that folder with only
`--out`, `--ffxi` and `--only`: HDCube and BC7 found, 8 entries, verify --soul clean.

## Trial and the whole game (2026-11-04, RTX 4070 Ti, game closed)

`make` with HDCube and BC7 into `C:\code\LSB\hifi-textures\hd-trial` (logs beside it: hd-trial-run1-zone.txt,
hd-trial-run2-models.txt, hd-trial-verify.txt): Lower Jeuno (ROM\1\41, 50 textures, 1.79 M original texels at 2x) in
one run, then 20 model DATs with the Yagudo's ROM\6\70 (61 textures, 1.60 M texels at 4x) in another. 111 entries,
none left out; verify --soul clean (colour mean at most 1.30 for zones and 0.51 for models; alpha p99 at most 31
against the limit 48).

| per original texel | upscaler (HDCube) | BC7 (texconv) | CPU (keeptexels, mips, soul) | all |
|---|---|---|---|---|
| zone, 2x | 3.11 us | 0.71 us (0.18 per made texel) | 0.23 us | 4.06 us |
| models, 4x | 3.45 us | 1.96 us (0.12 per made texel) | 0.51 us | 5.93 us |

(M7's x4plus trial: 6.72 us per original texel for models, all of it.) Whole game from dats.tsv's texels
(`C:\code\LSB\hifi-textures\work\all\dats.tsv` column 4, all textures of each DAT, so an upper bound: make takes only A1
textures and makes a shared texture once): zones 491.9 M texels (497.8 M less 5.9 M another overlay has) x 4.06 us =
33 min; models 497.3 M x 5.93 us = 49 min; about **1 h 25 min** in all on this PC (upscaler 54 min, BC7 22 min, CPU 6
min), less with dedupe and with full 24 M-texel batches (the trial's batches were small, so the upscaler's start-up
weighs in). The pack: at most about 13 GB (one byte a made texel, a third more for mips).
