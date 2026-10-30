@echo off
rem Builds HiFi XI's own d3d8.dll (src\d3d8: Direct3D 8 on Direct3D 12 in dgVoodoo's place; docs\own-frame\d3d8.md) into
rem build\d3d8\d3d8.dll, 32-bit like the game, and its offline tests (tests\d3d8_*) into build\d3d8\. Called from
rem build.cmd; on its own it sets up the compiler as build.cmd does (Visual Studio's x86 tools, the NuGet Windows SDK
rem reshade-worldfx uses). The Direct3D 8 headers are the Ashita SDK's (deps\ashita\plugins\sdk\d3d8).
rem "build_d3d8.cmd dll" (or "quick", as build.cmd passes it on): the DLL alone.
setlocal
cd /d "%~dp0"

if not defined VCToolsInstallDir (
  for /f "usebackq delims=" %%i in (`call "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
)
if not defined VCToolsInstallDir (
  call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul 2>&1 || exit /b 1
  set "SDK=%~dp0..\reshade-worldfx\deps\winsdk"
)
if defined SDK if exist "%SDK%\microsoft.windows.sdk.cpp\c\Include" (
  for /d %%v in ("%SDK%\microsoft.windows.sdk.cpp\c\Include\10.*") do set "SDKINC=%%v"
)
if defined SDKINC (
  set "INCLUDE=%INCLUDE%;%SDKINC%\ucrt;%SDKINC%\um;%SDKINC%\shared"
  set "LIB=%LIB%;%SDK%\microsoft.windows.sdk.cpp.x86\c\ucrt\x86;%SDK%\microsoft.windows.sdk.cpp.x86\c\um\x86"
)
set "D3D8INC=%~dp0deps\ashita\plugins\sdk\d3d8\includes"

if not exist build\d3d8\obj mkdir build\d3d8\obj
if not exist build\d3d8\test mkdir build\d3d8\test

set "D3D8SRC=src\d3d8\d3d8.cpp src\d3d8\device.cpp src\d3d8\device_draw.cpp src\d3d8\resource.cpp src\d3d8\present.cpp src\d3d8\gpu.cpp src\d3d8\format.cpp src\d3d8\ffp.cpp src\d3d8\sm1.cpp src\d3d8\compile.cpp"
cl /nologo /std:c++17 /O2 /MT /EHsc /W4 /wd4100 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG /I "%D3D8INC%" ^
  /LD %D3D8SRC% /Fo"build\d3d8\obj\\" /Fe"build\d3d8\d3d8.dll" /link /NOLOGO /DEF:src\d3d8\d3d8.def d3d12.lib dxgi.lib user32.lib gdi32.lib || exit /b 1
del /q build\d3d8\d3d8.exp build\d3d8\d3d8.lib 2>nul
echo Built build\d3d8\d3d8.dll
if /i "%~1"=="dll" exit /b 0
if /i "%~1"=="quick" exit /b 0

rem The tests: Direct3D 8 programs run through a d3d8.dll named on their command line (ours, or dgVoodoo's copied into
rem a scratch folder), D3DX 8's assembler from the Ashita SDK for the shader tests
set "D3DX8LIB=%~dp0deps\ashita\plugins\sdk\d3d8\lib\d3dx8.lib"
cl /nologo /std:c++17 /O2 /MT /EHsc /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /I "%D3D8INC%" ^
  tests\d3d8_frame.cpp /Fo"build\d3d8\test\\" /Fe"build\d3d8\d3d8_frame.exe" /link /NOLOGO user32.lib gdi32.lib "%D3DX8LIB%" legacy_stdio_definitions.lib /NODEFAULTLIB:libci.lib advapi32.lib || exit /b 1
echo Built build\d3d8\d3d8_frame.exe
rem The translator's pieces without a device (formats, the generated HLSL, shader model 1): build\d3d8\d3d8_unit.exe
cl /nologo /std:c++17 /O2 /MT /EHsc /W4 /wd4100 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /I "%D3D8INC%" ^
  tests\d3d8_unit.cpp src\d3d8\format.cpp src\d3d8\ffp.cpp src\d3d8\sm1.cpp src\d3d8\compile.cpp /Fo"build\d3d8\test\\" /Fe"build\d3d8\d3d8_unit.exe" ^
  /link /NOLOGO "%D3DX8LIB%" legacy_stdio_definitions.lib /NODEFAULTLIB:libci.lib advapi32.lib gdi32.lib user32.lib || exit /b 1
echo Built build\d3d8\d3d8_unit.exe
rem The shader cache shipped beside the DLL: data\d3d8\keys.bin (the shader keys met by the scenes and the d3dlog replays,
rem tests\d3d8_warm.cmd) compiled into build\d3d8\d3d8-shader-cache, without a GPU
if not exist build\d3d8\d3d8-shader-cache mkdir build\d3d8\d3d8-shader-cache
copy /y data\d3d8\keys.bin build\d3d8\d3d8-shader-cache\ >nul || exit /b 1
build\d3d8\d3d8_unit.exe --warm build\d3d8\d3d8-shader-cache || exit /b 1
rem HiFi XI's own hooks (src\host) over the DLL's Direct3D 12 stream, an FFXI-shaped frame on WARP: build\d3d8\d3d8_hooks.exe
cl /nologo /std:c++17 /O2 /MT /EHsc /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /I "%D3D8INC%" ^
  tests\d3d8_hooks.cpp src\host\hooks.cpp src\host\slots.cpp /Fo"build\d3d8\test\\" /Fe"build\d3d8\d3d8_hooks.exe" /link /NOLOGO d3d12.lib dxgi.lib user32.lib || exit /b 1
echo Built build\d3d8\d3d8_hooks.exe
exit /b 0
