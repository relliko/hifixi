@echo off
rem Builds HiFi XI's render engine (src\render: the grass and world shading renderer, the no-wait visibility tests, the
rem GPU crash reports, with the shaders src\render\grass.hlsl built in, and World FX's core, worldfx_core: what's drawn
rem when, for whichever host feeds it Direct3D 12's events) into <folder>\worldfx_engine.lib, for the
rem plugin, the World FX ReShade add-on in ..\reshade-worldfx (still what ships until the plugin draws) and their tests:
rem   call build_engine.cmd <folder>
rem From a build.cmd that has set up Visual Studio's x86 compiler and the Windows SDK's paths. The flags are the ones
rem the add-on was built with when the engine was part of it (moved here unchanged 2026-10-21).
setlocal
if "%~1"=="" (
  echo build_engine.cmd: no output folder given
  exit /b 1
)
set "OUT=%~f1"
set "HERE=%~dp0"
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
rem The shaders, built in as a string
pushd "%HERE%"
python tools\embed.py src\render\grass.hlsl "%OUT%\grass_hlsl.inc"
set "EMBED=%ERRORLEVEL%"
popd
if not "%EMBED%"=="0" exit /b 1
cl /nologo /c /std:c++17 /O2 /MT /EHsc /W4 /permissive- /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG /I "%OUT%" ^
  "%HERE%src\render\grass_render.cpp" "%HERE%src\render\vistest.cpp" "%HERE%src\render\gpu_crash.cpp" "%HERE%src\render\worldfx_core.cpp" ^
  /Fo"%OUT%\obj\\" || exit /b 1
lib /nologo /OUT:"%OUT%\worldfx_engine.lib" "%OUT%\obj\grass_render.obj" "%OUT%\obj\vistest.obj" "%OUT%\obj\gpu_crash.obj" ^
  "%OUT%\obj\worldfx_core.obj" || exit /b 1
echo Built %OUT%\worldfx_engine.lib
