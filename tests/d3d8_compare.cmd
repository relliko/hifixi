@echo off
rem HiFi XI's d3d8.dll against dgVoodoo 2.87 (docs\own-frame\d3d8.md): tests\d3d8_frame.cpp's scenes, and the d3dlog
rem replays, drawn through each and compared pixel by pixel. dgVoodoo's d3d8.dll and dgVoodoo.conf are copied from the
rem game's bootloader into a scratch folder (read only: nothing in the game's folder is written). Uses the GPU: run it
rem with the game closed.
rem   tests\d3d8_compare.cmd <scratch folder> [scenes|replay|look|all]
rem "look": research\ffxi-re\tools\dgvprobe's native probe (built: its build.cmd native) with the real hifixi.dll and its
rem starter over each d3d8.dll: the slot markers and World FX's world, then LOOK_GAME, three real zones (Southern
rem San d'Oria, West Ronfaure, Bibiki Bay) from the game's DATs drawn through Direct3D 8 (fixed function and the game's
rem own bump shaders), world target A read back after the zone and after the characters.
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
if /i "%WHAT%"=="look" goto look
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
if /i "%WHAT%"=="replay" exit /b 0

:look
set "PROBE=%~dp0..\..\research\ffxi-re\tools\dgvprobe\build\native\probe_native.exe"
if not exist "%PROBE%" (
  echo look: no %PROBE% ^(research\ffxi-re\tools\dgvprobe: build.cmd native^)
  exit /b 0
)
for %%d in (dgv mine) do (
  if not exist "%OUT%\native-%%d" mkdir "%OUT%\native-%%d"
  copy /y "%PROBE%" "%OUT%\native-%%d\" >nul
  copy /y build\hifixi.dll "%OUT%\native-%%d\" >nul
  copy /y build\starter\dxgi.dll "%OUT%\native-%%d\" >nul
)
copy /y "%OUT%\dgv\d3d8.dll" "%OUT%\native-dgv\" >nul
copy /y "%OUT%\dgv\dgVoodoo.conf" "%OUT%\native-dgv\" >nul
copy /y build\d3d8\d3d8.dll "%OUT%\native-mine\" >nul
for %%d in (dgv mine) do (
  pushd "%OUT%\native-%%d"
  <nul set /p "=%%d: "
  .\probe_native.exe slot 40 | findstr /c:"World FX's world"
  for %%z in (230 100 4) do (
    set "LOOK_GAME=%%z"
    set "LOOK_OUT=look-%%z.bmp"
    .\probe_native.exe slot 40 >"look-%%z.txt"
  )
  set "LOOK_GAME="
  set "LOOK_OUT="
  popd
)
for %%z in (230 100 4) do (
  for %%f in (look-%%z look-%%z-after) do (
    <nul set /p "=%%f: "
    build\d3d8\d3d8_frame.exe compare "%OUT%\native-dgv\%%f.bmp" "%OUT%\native-mine\%%f.bmp" "%OUT%\diff\%%f.bmp"
  )
)
exit /b 0
