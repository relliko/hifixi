@echo off
rem Builds build\hifixi.dll (32-bit, like the game), the render engine (build_engine.cmd: build\engine\worldfx_engine.lib)
rem and the offline tests with Visual Studio's compiler.
rem The Ashita v4 plugin SDK is in deps\ashita (a sparse clone of AshitaXI/Ashita-v4beta, plugins\sdk only); the Windows
rem SDK headers and libraries are the NuGet copy reshade-worldfx uses (no Windows SDK installed on this PC).
setlocal
cd /d "%~dp0"

if not exist deps\ashita\plugins\sdk\Ashita.h (
  echo Fetching the Ashita v4 plugin SDK...
  git clone --depth 1 --filter=blob:none --sparse https://github.com/AshitaXI/Ashita-v4beta.git deps\ashita || exit /b 1
  git -C deps\ashita sparse-checkout set plugins/sdk || exit /b 1
)

set "VSDIR="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo Visual Studio with the C++ tools not found.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul 2>&1 || exit /b 1

set "SDK=%~dp0..\reshade-worldfx\deps\winsdk"
if exist "%SDK%\microsoft.windows.sdk.cpp\c\Include" (
  for /d %%v in ("%SDK%\microsoft.windows.sdk.cpp\c\Include\10.*") do set "SDKINC=%%v"
)
if defined SDKINC (
  set "INCLUDE=%INCLUDE%;%SDKINC%\ucrt;%SDKINC%\um;%SDKINC%\shared"
  set "LIB=%LIB%;%SDK%\microsoft.windows.sdk.cpp.x86\c\ucrt\x86;%SDK%\microsoft.windows.sdk.cpp.x86\c\um\x86"
)

if not exist build mkdir build
if not exist build\obj mkdir build\obj
if not exist build\test mkdir build\test

rem The render engine (src\render), as the World FX add-on builds it
call "%~dp0build_engine.cmd" build\engine || exit /b 1

rem The game side (src\game): what the worldfx Ashita addon did (FEED); what hifps and drawcull did
rem (MODULES); the zone files made on the player's PC (BAKE, src\bake). The plugin and the menu's test take them all.
set "FEED=src\game\game.cpp src\game\feed.cpp src\game\glows.cpp src\game\gamewater.cpp src\game\gamerain.cpp"
set "MODULES=src\game\drawcull.cpp src\game\hifps\step.cpp src\game\hifps\sites.cpp src\game\hifps\exp.cpp src\game\gamedraw.cpp src\game\zoneslot.cpp"
set "BAKE=src\bake\dat.cpp src\bake\runner.cpp src\bake\collision.cpp src\bake\grass.cpp src\bake\water.cpp src\bake\wet.cpp src\bake\rain.cpp src\bake\shadow.cpp src\bake\lamps.cpp src\bake\sea.cpp src\bake\zonemesh.cpp"
set "GAME=%FEED% %MODULES% %BAKE%"
rem The native zone renderer (src\zone): each zone's data on the GPU (the plugin alone)
set "ZONE=src\zone\zone_gpu.cpp src\zone\native.cpp src\zone\frame.cpp src\zone\draw.cpp src\zone\inject.cpp src\zone\zoneshadow.cpp src\game\zonedraw.cpp"
rem The native character renderer (src\chars, src\game\charslot): the plugin alone
set "CHARS=src\game\charslot.cpp src\chars\marks.cpp src\game\charmodel.cpp src\chars\charframe.cpp src\chars\cache.cpp src\chars\os2.cpp src\chars\tex.cpp"
rem (its drawing on dgVoodoo's lists, with the engine's list_state: the plugin and the tests that draw)
set "CHARS_DRAW=src\chars\chardraw.cpp src\chars\charinject.cpp src\chars\chartest.cpp"

rem The plugin: the Ashita side, the backend (World FX's core on the hooks) and the hooks, with the engine
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG ^
  /LD src\plugin\main.cpp src\ui\menu.cpp src\settings\schema.cpp src\backend\backend.cpp src\backend\streamlog.cpp src\host\hooks.cpp src\host\slots.cpp ^
  %GAME% %ZONE% %CHARS% %CHARS_DRAW% build\engine\worldfx_engine.lib ^
  /Fo"build\obj\\" /Fe"build\hifixi.dll" /link /NOLOGO /DEF:src\plugin\exports.def user32.lib || exit /b 1
del /q build\hifixi.exp build\hifixi.lib 2>nul
echo Built build\hifixi.dll

rem The starter: build\starter\dxgi.dll, for the game's bootloader folder
if not exist build\starter mkdir build\starter
cl /nologo /O2 /MT /EHsc /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG ^
  /LD src\starter\starter.cpp /Fo"build\obj\\" /Fe"build\starter\dxgi.dll" /link /NOLOGO /DEF:src\starter\dxgi.def || exit /b 1
del /q build\starter\dxgi.exp build\starter\dxgi.lib 2>nul
echo Built build\starter\dxgi.dll

rem The hooks' offline test (WARP): build\hook_test.exe [--debug]
cl /nologo /std:c++17 /O2 /MT /EHsc /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG ^
  tests\hook_test.cpp src\host\hooks.cpp src\host\slots.cpp /Fo"build\test\\" /Fe"build\hook_test.exe" ^
  /link /NOLOGO || exit /b 1
echo Built build\hook_test.exe

rem The stream log (/hifi streamlog) on the hooks, on WARP: build\streamlog_test.exe
cl /nologo /std:c++20 /O2 /MT /EHsc /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG ^
  tests\streamlog_test.cpp src\backend\streamlog.cpp src\host\hooks.cpp src\host\slots.cpp /Fo"build\test\\" ^
  /Fe"build\streamlog_test.exe" /link /NOLOGO || exit /b 1
echo Built build\streamlog_test.exe

rem The render engine with the plugin's flags (C++20, /EHa): build\engine_check.exe
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG ^
  tests\engine_check.cpp build\engine\worldfx_engine.lib /Fo"build\test\\" /Fe"build\engine_check.exe" ^
  /link /NOLOGO || exit /b 1
echo Built build\engine_check.exe

rem The menu drawn with a fake GUI, its settings in a folder of its own: build\menu_test.exe
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG ^
  tests\menu_test.cpp src\ui\menu.cpp src\settings\schema.cpp src\backend\backend.cpp src\backend\streamlog.cpp src\host\hooks.cpp src\host\slots.cpp ^
  %GAME% %ZONE% %CHARS% %CHARS_DRAW% build\engine\worldfx_engine.lib /Fo"build\test\\" /Fe"build\menu_test.exe" /link /NOLOGO user32.lib || exit /b 1
echo Built build\menu_test.exe

rem The game side's feed on both FFXiMain dumps, World FX's functions stubbed: build\feed_test.exe
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG ^
  tests\feed_test.cpp %FEED% /Fo"build\test\\" /Fe"build\feed_test.exe" /link /NOLOGO user32.lib || exit /b 1
echo Built build\feed_test.exe

rem The game side's pieces on both dumps, each against the addon it took over (its routines and patches byte for byte):
rem build\glows_test.exe, gamewater_test, gamerain_test, drawcull_test; gamedraw_test (the off switches); zoneslot_test
rem (the zone pass's slot markers)
for %%t in (glows gamewater gamerain drawcull gamedraw zoneslot) do (
  cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG ^
    tests\%%t_test.cpp src\game\%%t.cpp src\game\game.cpp /Fo"build\test\\" /Fe"build\%%t_test.exe" /link /NOLOGO user32.lib || exit /b 1
  echo Built build\%%t_test.exe
)
rem The character passes' slot markers on both dumps, and their reading on the Direct3D 12 side: build\charslot_test.exe
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG ^
  tests\charslot_test.cpp %CHARS% src\game\game.cpp /Fo"build\test\\" /Fe"build\charslot_test.exe" /link /NOLOGO user32.lib d3d12.lib dxgi.lib || exit /b 1
echo Built build\charslot_test.exe
rem The characters' data: the Os2 and texture decoders against research\ffxi-re\tools\chars.py and chartex.py
rem (build\chars_data_test.exe build\test\chars_oracle.txt), and each model recorded at the game's model draw on both
rem dumps with the cache uploading to WARP (build\charmodel_test.exe)
python tests\chars_oracle.py build\test\chars_oracle.txt || exit /b 1
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG ^
  tests\chars_data_test.cpp src\chars\os2.cpp src\chars\tex.cpp /Fo"build\test\\" /Fe"build\chars_data_test.exe" /link /NOLOGO || exit /b 1
echo Built build\chars_data_test.exe
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG ^
  tests\charmodel_test.cpp %CHARS% src\game\game.cpp /Fo"build\test\\" /Fe"build\charmodel_test.exe" /link /NOLOGO user32.lib d3d12.lib dxgi.lib || exit /b 1
echo Built build\charmodel_test.exe
rem The character draw on a list recorded as dgVoodoo records (WARP, debug layer), the Mithra posed by chars.py:
rem build\charinject_test.exe build\test\chars_pose.bin [out.bmp]
python tests\chars_pose.py build\test\chars_pose.bin || exit /b 1
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /I src\render ^
  tests\charinject_test.cpp src\chars\chardraw.cpp src\chars\charinject.cpp src\chars\cache.cpp src\chars\charframe.cpp src\chars\os2.cpp src\chars\tex.cpp build\engine\worldfx_engine.lib ^
  /Fo"build\test\\" /Fe"build\charinject_test.exe" /link /NOLOGO user32.lib d3d12.lib dxgi.lib || exit /b 1
echo Built build\charinject_test.exe
rem The native zone renderer's zone data against tests\zonemesh_oracle.py: build\zonemesh_test.exe; and its offline
rem picture of a zone through the GPU upload (needs the game closed, or --warp): build\zone_view.exe; the plugin's
rem per-zone loading on WARP: build\zonenative_test.exe
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG ^
  tests\zonemesh_test.cpp src\bake\zonemesh.cpp src\bake\dat.cpp src\game\game.cpp /Fo"build\test\\" /Fe"build\zonemesh_test.exe" ^
  /link /NOLOGO user32.lib || exit /b 1
echo Built build\zonemesh_test.exe
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG ^
  tests\zone_view.cpp src\zone\draw.cpp src\zone\frame.cpp src\zone\zone_gpu.cpp src\bake\zonemesh.cpp src\bake\dat.cpp src\game\game.cpp /Fo"build\test\\" ^
  /Fe"build\zone_view.exe" /link /NOLOGO user32.lib d3d12.lib dxgi.lib || exit /b 1
echo Built build\zone_view.exe
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG ^
  tests\zonenative_test.cpp src\zone\native.cpp src\zone\zone_gpu.cpp src\bake\zonemesh.cpp src\bake\dat.cpp src\game\game.cpp ^
  /Fo"build\test\\" /Fe"build\zonenative_test.exe" /link /NOLOGO user32.lib d3d12.lib dxgi.lib || exit /b 1
echo Built build\zonenative_test.exe
rem The zone's objects handed to the native renderer (17CBA8's stub run on both dumps, zone 100 on WARP): build\zonedraw_test.exe
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG ^
  tests\zonedraw_test.cpp src\game\zonedraw.cpp src\zone\frame.cpp src\zone\zone_gpu.cpp src\bake\zonemesh.cpp src\bake\dat.cpp src\game\game.cpp ^
  /Fo"build\test\\" /Fe"build\zonedraw_test.exe" /link /NOLOGO user32.lib d3d12.lib dxgi.lib || exit /b 1
echo Built build\zonedraw_test.exe
rem The native renderer drawing at the zone's end marker on a list recorded as dgVoodoo records (WARP, debug layer):
rem build\zoneinject_test.exe [out.bmp]
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG /I src\render ^
  tests\zoneinject_test.cpp src\zone\inject.cpp src\zone\zoneshadow.cpp src\zone\draw.cpp src\zone\frame.cpp src\zone\zone_gpu.cpp src\bake\zonemesh.cpp src\bake\dat.cpp ^
  src\game\game.cpp build\engine\worldfx_engine.lib /Fo"build\test\\" /Fe"build\zoneinject_test.exe" /link /NOLOGO user32.lib d3d12.lib dxgi.lib || exit /b 1
echo Built build\zoneinject_test.exe
rem The visibility tests' answers (src\render\vistest.cpp): build\vistest_test.exe
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG ^
  tests\vistest_test.cpp src\render\vistest.cpp /Fo"build\test\\" /Fe"build\vistest_test.exe" /link /NOLOGO || exit /b 1
echo Built build\vistest_test.exe
rem hifps's frame step against hifps.lua run under lupa (tests\hifps_golden.py): build\hifps_step_test.exe
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG ^
  tests\hifps_step_test.cpp src\game\hifps\step.cpp src\game\hifps\sites.cpp src\game\game.cpp /Fo"build\test\\" ^
  /Fe"build\hifps_step_test.exe" /link /NOLOGO user32.lib || exit /b 1
echo Built build\hifps_step_test.exe
rem hifps's speed-ups against hifps.lua under lupa (tests\hifps_exp_lua.py), with the addon's own walls stub harness
rem (read from its folder, 32-bit, no CRT): build\hifps_exp_test.exe, build\test_hifps_exp\walls_harness.exe
if not exist build\test_hifps_exp mkdir build\test_hifps_exp
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG ^
  tests\hifps_exp_test.cpp src\game\hifps\exp.cpp src\game\game.cpp /Fo"build\test_hifps_exp\\" ^
  /Fe"build\hifps_exp_test.exe" /link /NOLOGO user32.lib || exit /b 1
echo Built build\hifps_exp_test.exe
set "HIFPS=C:\Games\PhoenixXI\addons\hifps\proxy"
cl /nologo /O1 /GS- /W3 /wd4164 /c "%HIFPS%\walls_harness.c" /Fo"build\test_hifps_exp\walls_harness.obj" || exit /b 1
link /nologo /subsystem:console /entry:main /nodefaultlib build\test_hifps_exp\walls_harness.obj kernel32.lib user32.lib ^
  /out:build\test_hifps_exp\walls_harness.exe || exit /b 1
echo Built build\test_hifps_exp\walls_harness.exe
rem The zone files made on their thread as zones are entered, into a folder of the test's own: build\runner_test.exe
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG ^
  tests\runner_test.cpp %BAKE% src\game\game.cpp /Fo"build\test\\" /Fe"build\runner_test.exe" /link /NOLOGO user32.lib || exit /b 1
echo Built build\runner_test.exe
rem The zone bakers against the Python bakers (tests\bake1_oracle.py, bake2_oracle.py): build\bake1_test.exe (grass,
rem water, wet ground, rain), build\bake2_test.exe (shadow maps, lamps)
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG ^
  tests\bake1_test.cpp src\bake\collision.cpp src\bake\grass.cpp src\bake\water.cpp src\bake\wet.cpp src\bake\rain.cpp ^
  src\bake\sea.cpp src\bake\dat.cpp src\game\game.cpp /Fo"build\test\\" /Fe"build\bake1_test.exe" /link /NOLOGO user32.lib || exit /b 1
echo Built build\bake1_test.exe
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG ^
  tests\bake2_test.cpp src\bake\shadow.cpp src\bake\lamps.cpp src\bake\dat.cpp src\game\game.cpp /Fo"build\test\\" ^
  /Fe"build\bake2_test.exe" /link /NOLOGO user32.lib || exit /b 1
echo Built build\bake2_test.exe
rem The sea sheets against tools\bake_sea.py (tests\sea_oracle.py): build\sea_test.exe
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG ^
  tests\sea_test.cpp src\bake\sea.cpp src\bake\collision.cpp src\bake\dat.cpp src\game\game.cpp /Fo"build\test\\" ^
  /Fe"build\sea_test.exe" ^
  /link /NOLOGO user32.lib || exit /b 1
echo Built build\sea_test.exe
rem The routines run as the game would, as reshade-worldfx's tests build them (their sprintf, and gamerain's ebp set in
rem inline assembly on purpose, not warned about): build\test\gamewater_cave.exe, build\test\gamerain_cave.exe
for %%t in ("..\reshade-worldfx\tests\gamewater_cave.c" "tests\gamerain_cave.c") do (
  cl /nologo /Od /W3 /D_CRT_SECURE_NO_WARNINGS /wd4731 "%%~t" /Fe"build\test\%%~nt.exe" ^
    /Fo"build\test\%%~nt.obj" || exit /b 1
  echo Built build\test\%%~nt.exe
)

rem "build.cmd e2e": World FX's end-to-end test (..\reshade-worldfx\tests\e2e.cpp, built with /DWORLDFX_NATIVE) on
rem HiFi XI instead of ReShade, in build\e2e-native: the starter as its dxgi.dll, hifixi.dll beside it, the stand-in
rem FFXiMain.dll and zone 100's blades. Run build\e2e-native\e2e.exe from there (WFX_E2E_HOUR=22 or 3 for night).
if /i not "%~1"=="e2e" exit /b 0
set "WFX=%~dp0..\reshade-worldfx"
if not exist build\e2e-native\worldfx-grass mkdir build\e2e-native\worldfx-grass
cl /nologo /O2 /MT /W4 /DWIN32_LEAN_AND_MEAN /LD "%WFX%\tests\fake_ffximain.cpp" /Fo"build\test\\" /Fe"build\e2e-native\FFXiMain.dll" /link /NOLOGO || exit /b 1
cl /nologo /std:c++17 /O2 /MT /EHsc /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG /DWORLDFX_NATIVE /I src\render ^
  "%WFX%\tests\e2e.cpp" build\engine\worldfx_engine.lib /Fo"build\test\\" /Fe"build\e2e-native\e2e.exe" /link /NOLOGO user32.lib || exit /b 1
del /q build\e2e-native\FFXiMain.exp build\e2e-native\FFXiMain.lib 2>nul
copy /y build\hifixi.dll build\e2e-native\ >nul || exit /b 1
copy /y build\starter\dxgi.dll build\e2e-native\ >nul || exit /b 1
copy /y "%WFX%\grass\100.bin" build\e2e-native\worldfx-grass\ >nul || exit /b 1
echo Built build\e2e-native
