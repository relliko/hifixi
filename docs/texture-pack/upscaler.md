# The upscaler: Real-ESRGAN, fetched on the player's OK

`hifixi-texpack make` (docs/texture-pack-plan.md M7) makes the game's textures bigger with Real-ESRGAN's x4plus model,
run as a separate program. This page is the licence decision for it, written down.

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
