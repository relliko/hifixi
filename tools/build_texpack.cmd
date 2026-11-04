@echo off
rem Builds the texture pack's tool, build\hifixi-texpack.exe (tools\texpack: convert, make, get-upscaler, verify, stats,
rem compact, prune; docs\texture-pack-plan.md section 7), its test, build\texpack_test.exe, and the test's stand-in for
rem the upscaler, build\fake_esrgan.exe (tests\fake_esrgan.cpp), all 64-bit: the tool never shares the
rem game's address space, and reads the game's key tables from FFXiMain.dll's file (tools\texpack\keys.cpp), not from a
rem game image (src\bake\dat.cpp, whose game:: code is 32-bit only). build.cmd calls it last; it can run alone.
rem Its own object folder, build\obj64: the same sources built 32-bit into build\test would clash at the link (LNK1112).
rem The Windows SDK is the installed one vcvarsall amd64 picks (the NuGet copy build.cmd adds has only 32-bit libraries).
setlocal
cd /d "%~dp0.."

set "VSDIR="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo Visual Studio with the C++ tools not found.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" amd64 >nul 2>&1 || exit /b 1

if not exist build mkdir build
if not exist build\obj64 mkdir build\obj64
if not exist build\obj64\texpack mkdir build\obj64\texpack
if not exist build\obj64\texpack_test mkdir build\obj64\texpack_test
if not exist build\obj64\fake_esrgan mkdir build\obj64\fake_esrgan

set "PACK=src\pack\pack.cpp src\pack\pack_write.cpp src\pack\bc2.cpp src\pack\hash.cpp src\pack\validate.cpp"
set "MAKE=tools\texpack\make.cpp tools\texpack\fetch.cpp tools\texpack\bc7.cpp tools\texpack\upscale.cpp tools\texpack\keeptexels.cpp tools\texpack\image_io.cpp"
cl /nologo /MP /std:c++20 /O2 /MT /EHsc /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG ^
  tools\texpack\texpack.cpp tools\texpack\keys.cpp %MAKE% %PACK% src\bake\dat_core.cpp src\bake\zonemesh.cpp src\chars\tex.cpp src\chars\os2.cpp ^
  /Fo"build\obj64\texpack\\" /Fe"build\hifixi-texpack.exe" /link /NOLOGO user32.lib advapi32.lib ole32.lib windowscodecs.lib bcrypt.lib urlmon.lib || exit /b 1
echo Built build\hifixi-texpack.exe
rem The tool on a few of the game's DATs and hand-made overlays (tests\texpack_test.cpp; tests\pack_oracle.py on what it writes)
cl /nologo /MP /std:c++20 /O2 /MT /EHsc /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG ^
  tests\texpack_test.cpp %PACK% src\bake\dat_core.cpp tools\texpack\bc7.cpp tools\texpack\upscale.cpp tools\texpack\keeptexels.cpp tools\texpack\image_io.cpp src\chars\tex.cpp ^
  /Fo"build\obj64\texpack_test\\" /Fe"build\texpack_test.exe" /link /NOLOGO user32.lib ole32.lib windowscodecs.lib d3d12.lib dxgi.lib || exit /b 1
echo Built build\texpack_test.exe
cl /nologo /MP /std:c++20 /O2 /MT /EHsc /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG ^
  tests\fake_esrgan.cpp tools\texpack\image_io.cpp ^
  /Fo"build\obj64\fake_esrgan\\" /Fe"build\fake_esrgan.exe" /link /NOLOGO user32.lib ole32.lib windowscodecs.lib || exit /b 1
echo Built build\fake_esrgan.exe
