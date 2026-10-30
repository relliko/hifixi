@echo off
rem HiFi XI's d3d8.dll against dgVoodoo 2.87 (docs\own-frame\d3d8.md): tests\d3d8_frame.cpp's scenes, and the d3dlog
rem replays, drawn through each and compared pixel by pixel. dgVoodoo's d3d8.dll and dgVoodoo.conf are copied from the
rem game's bootloader into a scratch folder (read only: nothing in the game's folder is written). Uses the GPU: run it
rem with the game closed.
rem   tests\d3d8_compare.cmd <scratch folder> [scenes|replay|all]
setlocal
cd /d "%~dp0.."
set "OUT=%~1"
if "%OUT%"=="" (
  echo tests\d3d8_compare.cmd ^<scratch folder^> [scenes^|replay^|all]
  exit /b 1
)
set "WHAT=%~2"
if "%WHAT%"=="" set "WHAT=all"
if not exist "%OUT%\dgv" mkdir "%OUT%\dgv"
if not exist "%OUT%\mine" mkdir "%OUT%\mine"
if not exist "%OUT%\diff" mkdir "%OUT%\diff"
copy /y "C:\Games\PhoenixXI\bootloader\d3d8.dll" "%OUT%\dgv\" >nul || exit /b 1
copy /y "C:\Games\PhoenixXI\bootloader\dgVoodoo.conf" "%OUT%\dgv\" >nul || exit /b 1
copy /y build\d3d8\d3d8_frame.exe "%OUT%\dgv\" >nul || exit /b 1
copy /y build\d3d8\d3d8_frame.exe "%OUT%\mine\" >nul || exit /b 1
copy /y build\d3d8\d3d8.dll "%OUT%\mine\" >nul || exit /b 1
set "D3D8_LOG=%OUT%\mine\d3d8.log"
set "D3D8_SHADER_CACHE=%OUT%\mine\d3d8-shader-cache"

if /i "%WHAT%"=="replay" goto replay
pushd "%OUT%\dgv"
.\d3d8_frame.exe "%OUT%\dgv\d3d8.dll" scenes "%OUT%\dgv\out" >"%OUT%\dgv\scenes.txt"
popd
pushd "%OUT%\mine"
.\d3d8_frame.exe "%OUT%\mine\d3d8.dll" scenes "%OUT%\mine\out" >"%OUT%\mine\scenes.txt" || (echo our scenes failed: %OUT%\mine\scenes.txt & exit /b 1)
popd
for %%s in (formats ops blend light fog texgen stencil streams shaders imgui readback) do (
  <nul set /p "=%%s: "
  build\d3d8\d3d8_frame.exe compare "%OUT%\dgv\out\%%s.bmp" "%OUT%\mine\out\%%s.bmp" "%OUT%\diff\%%s.bmp"
)
if /i "%WHAT%"=="scenes" exit /b 0

:replay
for %%c in (day_npcs night water spell menu) do (
  pushd "%OUT%\dgv"
  .\d3d8_frame.exe "%OUT%\dgv\d3d8.dll" replay "C:\Games\PhoenixXI\d3dlog\%%c.txt" "%OUT%\dgv\replay-%%c" >"%OUT%\dgv\replay-%%c.txt"
  popd
  pushd "%OUT%\mine"
  .\d3d8_frame.exe "%OUT%\mine\d3d8.dll" replay "C:\Games\PhoenixXI\d3dlog\%%c.txt" "%OUT%\mine\replay-%%c" >"%OUT%\mine\replay-%%c.txt"
  popd
  for %%p in (world screen) do (
    <nul set /p "=replay %%c %%p: "
    build\d3d8\d3d8_frame.exe compare "%OUT%\dgv\replay-%%c-%%p.bmp" "%OUT%\mine\replay-%%c-%%p.bmp" "%OUT%\diff\replay-%%c-%%p.bmp"
  )
)
exit /b 0
