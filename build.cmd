@echo off
rem Builds build\hifixi.dll (32-bit, like the game), the render engine (build_engine.cmd: build\engine\worldfx_engine.lib)
rem and the offline tests with Visual Studio's compiler ("build.cmd quick": no tests but hook_test; "build.cmd e2e":
rem everything and the end-to-end test's folder).
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
rem Frame generation (src\framegen: AMD's FidelityFX SDK, its shaders compiled with the Windows SDK's DXC)
call "%~dp0build_framegen.cmd" build\framegen || exit /b 1

rem The game side (src\game): what the worldfx Ashita addon did (FEED); what hifps and drawcull did
rem (MODULES); the zone files made on the player's PC (BAKE, src\bake). The plugin and the menu's test take them all.
set "FEED=src\game\game.cpp src\game\feed.cpp src\game\glows.cpp src\game\gamewater.cpp src\game\gamerain.cpp"
set "MODULES=src\game\jitter.cpp src\game\drawcull.cpp src\game\hifps\step.cpp src\game\hifps\sites.cpp src\game\hifps\exp.cpp src\game\gamedraw.cpp src\game\zoneslot.cpp"
set "BAKE=src\bake\dat.cpp src\bake\runner.cpp src\bake\collision.cpp src\bake\grass.cpp src\bake\water.cpp src\bake\wet.cpp src\bake\rain.cpp src\bake\lamps.cpp src\bake\sea.cpp src\bake\zonemesh.cpp"
set "GAME=%FEED% %MODULES% %BAKE%"
rem The native zone renderer (src\zone): each zone's data on the GPU (the plugin alone)
set "ZONE=src\zone\zone_gpu.cpp src\zone\native.cpp src\zone\frame.cpp src\zone\draw.cpp src\zone\inject.cpp src\zone\zoneshadow.cpp src\game\zonedraw.cpp src\zone\actordata.cpp src\game\modelactor.cpp"
rem The native character renderer (src\chars, src\game\charslot): the plugin alone
set "CHARS=src\game\charslot.cpp src\chars\marks.cpp src\game\charmodel.cpp src\chars\charframe.cpp src\chars\cache.cpp src\chars\os2.cpp src\chars\tex.cpp"
rem (its drawing on dgVoodoo's lists, with the engine's list_state: the plugin and the tests that draw)
set "CHARS_DRAW=src\chars\chardraw.cpp src\chars\charinject.cpp src\chars\chartest.cpp"
rem The world upscaled to the screen (src\upscale: AMD FSR 1's headers in src\upscale\fsr1, FSR 2 in src\upscale\fsr2, both
rem MIT), the shaders built in; FSR 2's own C++ with its own warning level
if not exist build\gen mkdir build\gen
python tools\embed.py src\upscale\upscale.hlsl build\gen\upscale_hlsl.inc || exit /b 1
python tools\embed.py src\upscale\fsr1\ffx_a.h build\gen\ffx_a_h.inc || exit /b 1
python tools\embed.py src\upscale\fsr1\ffx_fsr1.h build\gen\ffx_fsr1_h.inc || exit /b 1
python tools\embed_dir.py src\upscale\fsr2\shaders build\gen\fsr2_shaders.inc .h .hlsl || exit /b 1
if not exist build\obj\fsr2 mkdir build\obj\fsr2
cl /nologo /c /std:c++17 /O2 /MT /EHsc /W3 /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG /D_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING ^
  src\upscale\fsr2\ffx_fsr2.cpp src\upscale\fsr2\ffx_assert.cpp src\upscale\fsr2\dx12\ffx_fsr2_dx12.cpp /Fo"build\obj\fsr2\\" || exit /b 1
set "UPSCALE=src\upscale\upscale.cpp src\upscale\temporal.cpp src\upscale\masks.cpp src\upscale\fsr2_shaders.cpp build\obj\fsr2\ffx_fsr2.obj build\obj\fsr2\ffx_assert.obj build\obj\fsr2\ffx_fsr2_dx12.obj"

rem The own-frame passes (PLAN-own-frame.md), each registered through src\host\passes.cpp. One line per part, so
rem the parts built side by side merge cleanly.
set "PASSES=src\host\passes.cpp"
rem (fx: every effect element of the priority-12 flush drawn natively: src\fx, its game side src\game\fxdraw)
set "PASSES_FX=src\fx\fxframe.cpp src\fx\fxtextures.cpp src\fx\fxrender.cpp src\fx\fxinject.cpp src\fx\fxmodule.cpp src\game\fxdraw.cpp"
set "PASSES_WORLD=src\world\world_frames.cpp src\world\world_dome.cpp src\world\world_render.cpp src\world\world_font.cpp src\world\world_module.cpp src\game\worldmark.cpp src\game\skydraw.cpp src\game\namedraw.cpp src\game\griddraw.cpp"
set "PASSES_ACTORS="
set "PASSES_2D=src\ui2d\frame2d.cpp src\ui2d\fonthigh.cpp src\ui2d\texfmt.cpp src\ui2d\render2d.cpp src\ui2d\ui2d.cpp src\game\uidraw.cpp"
set "PASSES=%PASSES% %PASSES_FX% %PASSES_WORLD% %PASSES_ACTORS% %PASSES_2D%"

rem The plugin: the Ashita side, the backend (World FX's core on the hooks) and the hooks, with the engine
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG /I build\gen ^
  /LD src\plugin\main.cpp src\ui\menu.cpp src\settings\schema.cpp src\backend\backend.cpp src\backend\streamlog.cpp src\host\hooks.cpp src\host\slots.cpp ^
  %PASSES% %GAME% %ZONE% %CHARS% %CHARS_DRAW% %UPSCALE% build\engine\worldfx_engine.lib build\framegen\ffx_fg.lib ^
  /Fo"build\obj\\" /Fe"build\hifixi.dll" /link /NOLOGO /DEF:src\plugin\exports.def user32.lib dxgi.lib || exit /b 1
del /q build\hifixi.exp build\hifixi.lib 2>nul
echo Built build\hifixi.dll

rem The starter: build\starter\dxgi.dll, for the game's bootloader folder
if not exist build\starter mkdir build\starter
cl /nologo /O2 /MT /EHsc /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG ^
  /LD src\starter\starter.cpp /Fo"build\obj\\" /Fe"build\starter\dxgi.dll" /link /NOLOGO /DEF:src\starter\dxgi.def || exit /b 1
del /q build\starter\dxgi.exp build\starter\dxgi.lib 2>nul
echo Built build\starter\dxgi.dll
call "%~dp0build_d3d8.cmd" %1 || exit /b 1

rem The hooks' offline test (WARP): build\hook_test.exe [--debug]
cl /nologo /std:c++17 /O2 /MT /EHsc /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG ^
  tests\hook_test.cpp src\host\hooks.cpp src\host\slots.cpp /Fo"build\test\\" /Fe"build\hook_test.exe" ^
  /link /NOLOGO || exit /b 1
echo Built build\hook_test.exe

rem "build.cmd quick": the plugin, the starter and hook_test only (a small fix). The other tests stay as the last full
rem build left them: run the full build when what they test changed, and before a public release.
if /i "%~1"=="quick" exit /b 0

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

rem The zone files the render tests read, made from this PC's game files with HiFi XI's own bakers when missing (not in
rem the repo: they're made from the game's data): build\testdata (build\test_data.exe)
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG ^
  tests\test_data.cpp src\bake\collision.cpp src\bake\grass.cpp src\bake\water.cpp src\bake\rain.cpp src\bake\sea.cpp src\bake\dat.cpp src\game\game.cpp ^
  /Fo"build\test\\" /Fe"build\test_data.exe" /link /NOLOGO user32.lib || exit /b 1
build\test_data.exe || exit /b 1

rem The renderer's offline test (WARP, debug layer; from this folder): build\grass_test.exe [blade file]
cl /nologo /std:c++17 /O2 /MT /EHsc /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG /I src\render ^
  tests\grass_test.cpp build\engine\worldfx_engine.lib /Fo"build\test\\" /Fe"build\grass_test.exe" /link /NOLOGO || exit /b 1
echo Built build\grass_test.exe

rem The shading pass timed on the real GPU (see its header): build\shade_bench.exe <width> <height> [shader file] ...
cl /nologo /std:c++17 /O2 /MT /EHsc /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG /I src\render ^
  tests\shade_bench.cpp build\engine\worldfx_engine.lib /Fo"build\test\\" /Fe"build\shade_bench.exe" /link /NOLOGO || exit /b 1
echo Built build\shade_bench.exe

rem The world part of owning the frame (src\world, src\game\skydraw, namedraw, griddraw): their places on both dumps, their
rem recording, the dome and the names' font against tests\world_oracle.py, the passes drawn on WARP with the debug layer:
rem build\world_test.exe build\test\world_oracle.txt
python tests\world_oracle.py build\test\world_oracle.txt || exit /b 1
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG /I src\render ^
  tests\world_test.cpp src\world\world_frames.cpp src\world\world_dome.cpp src\world\world_render.cpp src\world\world_font.cpp src\game\worldmark.cpp ^
  src\game\skydraw.cpp src\game\namedraw.cpp src\game\griddraw.cpp src\game\jitter.cpp src\chars\tex.cpp src\bake\dat.cpp src\game\game.cpp ^
  build\engine\worldfx_engine.lib /Fo"build\test\\" /Fe"build\world_test.exe" /link /NOLOGO /DYNAMICBASE:NO user32.lib d3d12.lib dxgi.lib || exit /b 1
echo Built build\world_test.exe
rem The world part's modules as the plugin and the backend drive them (the gates, the handlers into the world on WARP):
rem build\worldpass_test.exe (based high: with the plugin's statics its image would reach up to where the system maps its
rem early files, pushing them over the dump's base)
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG /I build\gen /I src\render ^
  tests\worldpass_test.cpp src\host\passes.cpp %PASSES_WORLD% src\chars\tex.cpp %UPSCALE% %GAME% %ZONE% build\engine\worldfx_engine.lib build\framegen\ffx_fg.lib ^
  /Fo"build\test\\" /Fe"build\worldpass_test.exe" /link /NOLOGO /DYNAMICBASE:NO /BASE:0x10000000 user32.lib d3d12.lib dxgi.lib || exit /b 1
echo Built build\worldpass_test.exe
rem The actors' part of owning the frame (docs\own-frame\actors.md): the cloth (type-1) meshes stepped by the game's own
rem 023290 through the model stub on both dumps, against tests\cloth_oracle.py, the cache uploading to WARP:
rem build\cloth_test.exe build\test\cloth_oracle.bin
python tests\cloth_oracle.py build\test\cloth_oracle.bin || exit /b 1
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG ^
  tests\cloth_test.cpp %CHARS% src\game\game.cpp /Fo"build\test\\" /Fe"build\cloth_test.exe" /link /NOLOGO user32.lib d3d12.lib dxgi.lib || exit /b 1
echo Built build\cloth_test.exe
rem ... and drawn (WARP, debug layer): the banner's cloth as the oracle steps it, the overlay and cloak passes, the scene's
rem copy at the see-through marker: build\actors_draw_test.exe build\test\cloth_oracle.bin
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /I src\render ^
  tests\actors_draw_test.cpp src\chars\chardraw.cpp src\chars\charinject.cpp src\chars\cache.cpp src\chars\charframe.cpp src\chars\os2.cpp src\chars\tex.cpp build\engine\worldfx_engine.lib ^
  /Fo"build\test\\" /Fe"build\actors_draw_test.exe" /link /NOLOGO user32.lib d3d12.lib dxgi.lib || exit /b 1
echo Built build\actors_draw_test.exe

rem The effects (fx: src\fx, src\game\fxdraw). The geometry decoders against effects_geo.py (tests\fx_oracle.py geo):
rem build\fx_geo_test.exe build\test\fx_geo.txt
python tests\fx_oracle.py geo build\test\fx_geo.txt ROM/11/17.DAT ROM/10/9.DAT ROM/76/30.DAT ROM/13/39.DAT ROM/0/0.DAT ROM/1/31.DAT ROM/0/120.DAT ROM3/9/111.DAT || exit /b 1
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG ^
  tests\fx_geo_test.cpp src\fx\fxgeo.cpp /Fo"build\test\\" /Fe"build\fx_geo_test.exe" /link /NOLOGO || exit /b 1
echo Built build\fx_geo_test.exe
rem The game side on both dumps, the game's own walk and element draw run through our stubs (WARP for a texture's copy):
rem build\fx_test.exe
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG ^
  tests\fx_test.cpp src\game\fxdraw.cpp src\fx\fxframe.cpp src\fx\fxgeo.cpp src\fx\fxtextures.cpp src\chars\tex.cpp src\bake\dat.cpp src\game\game.cpp ^
  src\game\gamewater.cpp src\zone\zone_gpu.cpp src\bake\zonemesh.cpp ^
  /Fo"build\test\\" /Fe"build\fx_test.exe" /link /NOLOGO /DYNAMICBASE:NO user32.lib d3d12.lib dxgi.lib || exit /b 1
echo Built build\fx_test.exe
rem The renderer at its marker on WARP with the debug layer against effects_play.py (tests\fx_oracle.py play: Fire, Cure,
rem Fast Blade, made when missing, a minute or so; Southern San d'Oria's fountain: MMB meshes):
rem build\fx_render_test.exe build\test\fx_fire.bin ...
if not exist build\test\fx_fire.bin python tests\fx_oracle.py play build\test\fx_fire.bin 150 --action spell 144 --size 480x270 || exit /b 1
if not exist build\test\fx_cure.bin python tests\fx_oracle.py play build\test\fx_cure.bin 60 --action spell 1 --size 480x270 || exit /b 1
if not exist build\test\fx_fastblade.bin python tests\fx_oracle.py play build\test\fx_fastblade.bin 38 --action ws 32 --size 480x270 || exit /b 1
if not exist build\test\fx_fountain.bin python tests\fx_oracle.py play build\test\fx_fountain.bin 120 --dat ROM/1/31 --gen fun1 fun2 fun3 awgd --size 480x270 || exit /b 1
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG /I src\render ^
  tests\fx_render_test.cpp src\fx\fxframe.cpp src\fx\fxgeo.cpp src\fx\fxinject.cpp src\fx\fxrender.cpp src\fx\fxtextures.cpp src\chars\tex.cpp ^
  src\zone\zone_gpu.cpp src\bake\zonemesh.cpp src\bake\dat.cpp src\game\game.cpp ^
  build\engine\worldfx_engine.lib /Fo"build\test\\" /Fe"build\fx_render_test.exe" /link /NOLOGO /DYNAMICBASE:NO user32.lib d3d12.lib dxgi.lib || exit /b 1
echo Built build\fx_render_test.exe
rem The screen's 2D (src\game\uidraw, src\ui2d; docs\own-frame\2d.md): the loop tail's calls, the recorder on a stand-in
rem device and the gate on both dumps, the text fonts against tests\ui2d_fonts.py (build\uidraw_test.exe
rem build\test\ui2d_fonts.txt); the recorder on dgVoodoo's own d3d8.dll (the game's, with
rem its dgVoodoo.conf drawn by WARP: tests\ui2d_conf.py) against the 2D renderer on Direct3D 12 WARP at the pass markers,
rem the d3dlog captures' interface replayed (build\dgv\ui2d_dgv.exe, run from build\dgv)
set "UI2D=src\ui2d\frame2d.cpp src\ui2d\fonthigh.cpp src\ui2d\texfmt.cpp src\ui2d\render2d.cpp src\game\uidraw.cpp src\bake\dat.cpp src\game\game.cpp"
python tests\ui2d_fonts.py build\test\ui2d_fonts.txt || exit /b 1
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG /I src\render ^
  tests\uidraw_test.cpp %UI2D% build\engine\worldfx_engine.lib /Fo"build\test\\" /Fe"build\uidraw_test.exe" ^
  /link /NOLOGO /DYNAMICBASE:NO user32.lib d3d12.lib dxgi.lib || exit /b 1
echo Built build\uidraw_test.exe
if not exist build\dgv mkdir build\dgv
rem (HiFi XI's own d3d8.dll, from build_d3d8.cmd: docs\own-frame\no-dgvoodoo.md; no dgVoodoo.conf)
if exist build\d3d8\d3d8.dll copy /y build\d3d8\d3d8.dll build\dgv\ >nul
if exist build\dgv\dgVoodoo.conf del build\dgv\dgVoodoo.conf
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /I src\render ^
  /I deps\ashita\plugins\sdk\d3d8\includes tests\ui2d_dgv.cpp %UI2D% src\ui2d\ui2d.cpp src\host\passes.cpp build\engine\worldfx_engine.lib ^
  /Fo"build\test\\" /Fe"build\dgv\ui2d_dgv.exe" /link /NOLOGO user32.lib d3d12.lib dxgi.lib d3dcompiler.lib || exit /b 1
echo Built build\dgv\ui2d_dgv.exe

rem The menu drawn with a fake GUI, its settings in a folder of its own: build\menu_test.exe
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG /I build\gen ^
  tests\menu_test.cpp src\ui\menu.cpp src\settings\schema.cpp src\backend\backend.cpp src\backend\streamlog.cpp src\host\hooks.cpp src\host\slots.cpp ^
  %PASSES% %GAME% %ZONE% %CHARS% %CHARS_DRAW% %UPSCALE% build\engine\worldfx_engine.lib build\framegen\ffx_fg.lib /Fo"build\test\\" /Fe"build\menu_test.exe" /link /NOLOGO user32.lib dxgi.lib || exit /b 1
echo Built build\menu_test.exe

rem The game side's feed on both FFXiMain dumps, World FX's functions stubbed: build\feed_test.exe
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG ^
  tests\feed_test.cpp %FEED% /Fo"build\test\\" /Fe"build\feed_test.exe" /link /NOLOGO user32.lib || exit /b 1
echo Built build\feed_test.exe

rem The game side's pieces on both dumps, each against the addon it took over (its routines and patches byte for byte):
rem build\glows_test.exe, gamewater_test, gamerain_test, drawcull_test; gamedraw_test (the off switches); zoneslot_test
rem (the zone pass's slot markers)
for %%t in (glows gamewater gamerain drawcull gamedraw zoneslot jitter) do (
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
rem The en-spell effects offline, on the GPU (tests\enspell_view.cpp; poses from tests\enspell_poses.py, a GIF from
rem tools\enspell_gif.py): build\enspell_view.exe <poses.bin> <out folder> <frames> <dat> [dat...]
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /I src\render ^
  tests\enspell_view.cpp src\chars\chardraw.cpp src\chars\charinject.cpp src\chars\cache.cpp src\chars\charframe.cpp src\chars\os2.cpp src\chars\tex.cpp build\engine\worldfx_engine.lib ^
  /Fo"build\test\\" /Fe"build\enspell_view.exe" /link /NOLOGO user32.lib d3d12.lib dxgi.lib || exit /b 1
echo Built build\enspell_view.exe
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
  tests\zonedraw_test.cpp src\game\zonedraw.cpp src\game\jitter.cpp src\zone\frame.cpp src\zone\zone_gpu.cpp src\bake\zonemesh.cpp src\bake\dat.cpp src\game\game.cpp ^
  /Fo"build\test\\" /Fe"build\zonedraw_test.exe" /link /NOLOGO user32.lib d3d12.lib dxgi.lib || exit /b 1
echo Built build\zonedraw_test.exe
rem The model actors handed to the zone renderer (0C4CC8's call on both dumps, a model DAT on WARP): build\modelactor_test.exe
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG ^
  tests\modelactor_test.cpp src\game\modelactor.cpp src\zone\actordata.cpp src\zone\draw.cpp src\zone\frame.cpp src\zone\zone_gpu.cpp src\bake\zonemesh.cpp ^
  src\bake\dat.cpp src\game\game.cpp /Fo"build\test\\" /Fe"build\modelactor_test.exe" /link /NOLOGO /DYNAMICBASE:NO user32.lib d3d12.lib dxgi.lib || exit /b 1
echo Built build\modelactor_test.exe
rem The native renderer drawing at the zone's end marker on a list recorded as dgVoodoo records (WARP, debug layer):
rem build\zoneinject_test.exe [out.bmp]
rem (no address-space randomisation: the FFXiMain dump goes at its own base, which a system mapping took at times)
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG /I src\render ^
  tests\zoneinject_test.cpp src\zone\inject.cpp src\zone\zoneshadow.cpp src\zone\draw.cpp src\zone\frame.cpp src\zone\zone_gpu.cpp src\bake\zonemesh.cpp src\bake\dat.cpp ^
  src\game\game.cpp build\engine\worldfx_engine.lib /Fo"build\test\\" /Fe"build\zoneinject_test.exe" /link /NOLOGO /DYNAMICBASE:NO user32.lib d3d12.lib dxgi.lib || exit /b 1
echo Built build\zoneinject_test.exe
rem The world upscaled to the screen on a list recorded as dgVoodoo records its own (src\upscale): build\upscale_test.exe
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG /I build\gen ^
  tests\upscale_test.cpp %UPSCALE% %GAME% %ZONE% build\engine\worldfx_engine.lib build\framegen\ffx_fg.lib /Fo"build\test\\" /Fe"build\upscale_test.exe" /link /NOLOGO user32.lib d3d12.lib dxgi.lib || exit /b 1
echo Built build\upscale_test.exe
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
rem water, wet ground, rain), build\bake2_test.exe (lamps)
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG ^
  tests\bake1_test.cpp src\bake\collision.cpp src\bake\grass.cpp src\bake\water.cpp src\bake\wet.cpp src\bake\rain.cpp ^
  src\bake\sea.cpp src\bake\dat.cpp src\game\game.cpp /Fo"build\test\\" /Fe"build\bake1_test.exe" /link /NOLOGO user32.lib || exit /b 1
echo Built build\bake1_test.exe
cl /nologo /std:c++20 /O2 /MT /EHa /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG ^
  tests\bake2_test.cpp src\bake\lamps.cpp src\bake\dat.cpp src\game\game.cpp /Fo"build\test\\" ^
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

rem "build.cmd e2e": World FX's end-to-end test on HiFi XI (tests\e2e.cpp) in build\e2e-native: the starter as its
rem dxgi.dll, hifixi.dll beside it, the stand-in FFXiMain.dll and zone 100's blades. Run build\e2e-native\e2e.exe from
rem there (WFX_E2E_HOUR=22 or 3 for night).
if /i not "%~1"=="e2e" exit /b 0
if not exist build\e2e-native\worldfx-grass mkdir build\e2e-native\worldfx-grass
cl /nologo /O2 /MT /W4 /DWIN32_LEAN_AND_MEAN /LD tests\fake_ffximain.cpp /Fo"build\test\\" /Fe"build\e2e-native\FFXiMain.dll" /link /NOLOGO || exit /b 1
cl /nologo /std:c++17 /O2 /MT /EHsc /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG /I src\render ^
  tests\e2e.cpp build\engine\worldfx_engine.lib /Fo"build\test\\" /Fe"build\e2e-native\e2e.exe" /link /NOLOGO user32.lib || exit /b 1
del /q build\e2e-native\FFXiMain.exp build\e2e-native\FFXiMain.lib 2>nul
copy /y build\hifixi.dll build\e2e-native\ >nul || exit /b 1
copy /y build\starter\dxgi.dll build\e2e-native\ >nul || exit /b 1
copy /y build\testdata\grass\100.bin build\e2e-native\worldfx-grass\ >nul || exit /b 1
echo Built build\e2e-native
