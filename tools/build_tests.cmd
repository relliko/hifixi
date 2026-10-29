@echo off
rem Builds chosen offline tests only, after "build.cmd quick" (which builds the engine they link): the names of build.cmd's
rem test blocks to build, any of grass zoneinject charinject. "build.cmd" builds them all; this is for iterating on one.
setlocal
cd /d "%~dp0.."
set "VSDIR="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR exit /b 1
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul 2>&1 || exit /b 1
set "SDK=%~dp0..\..\reshade-worldfx\deps\winsdk"
if exist "%SDK%\microsoft.windows.sdk.cpp\c\Include" (
  for /d %%v in ("%SDK%\microsoft.windows.sdk.cpp\c\Include\10.*") do set "SDKINC=%%v"
)
if defined SDKINC (
  set "INCLUDE=%INCLUDE%;%SDKINC%\ucrt;%SDKINC%\um;%SDKINC%\shared"
  set "LIB=%LIB%;%SDK%\microsoft.windows.sdk.cpp.x86\c\ucrt\x86;%SDK%\microsoft.windows.sdk.cpp.x86\c\um\x86"
)
if not exist build\test mkdir build\test

:next
if "%~1"=="" exit /b 0
if /i "%~1"=="grass" (
  cl /nologo /std:c++17 /O2 /MT /EHsc /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG /I src\render ^
    tests\grass_test.cpp build\engine\worldfx_engine.lib /Fo"build\test\\" /Fe"build\grass_test.exe" /link /NOLOGO || exit /b 1
  echo Built build\grass_test.exe
)
if /i "%~1"=="zoneinject" (
  cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG /I src\render ^
    tests\zoneinject_test.cpp src\zone\inject.cpp src\zone\zoneshadow.cpp src\zone\draw.cpp src\zone\frame.cpp src\zone\zone_gpu.cpp src\bake\zonemesh.cpp src\bake\dat.cpp ^
    src\game\game.cpp build\engine\worldfx_engine.lib /Fo"build\test\\" /Fe"build\zoneinject_test.exe" /link /NOLOGO user32.lib d3d12.lib dxgi.lib || exit /b 1
  echo Built build\zoneinject_test.exe
)
if /i "%~1"=="charinject" (
  if not exist build\test\chars_pose.bin python tests\chars_pose.py build\test\chars_pose.bin || exit /b 1
  cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /I src\render ^
    tests\charinject_test.cpp src\chars\chardraw.cpp src\chars\charinject.cpp src\chars\cache.cpp src\chars\charframe.cpp src\chars\os2.cpp src\chars\tex.cpp build\engine\worldfx_engine.lib ^
    /Fo"build\test\\" /Fe"build\charinject_test.exe" /link /NOLOGO user32.lib d3d12.lib dxgi.lib || exit /b 1
  echo Built build\charinject_test.exe
)
shift
goto next
