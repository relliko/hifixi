@echo off
rem The shader cache shipped with HiFi XI's d3d8.dll (docs\own-frame\d3d8.md, "First-session shader compiles"):
rem build\d3d8\d3d8-shader-cache, made fresh by drawing tests\d3d8_frame.cpp's scenes and the five d3dlog replays through
rem the DLL: keys.bin (every shader key met) and the compiled blobs. Beside the DLL in the game, the first session finds
rem them; a later DLL whose HLSL differs compiles the keys on worker threads at the device's making instead of in play.
rem Uses the GPU: run it with the game closed.
setlocal
cd /d "%~dp0.."
set "CACHE=%~dp0..\build\d3d8\d3d8-shader-cache"
if exist "%CACHE%" rmdir /s /q "%CACHE%"
set "D3D8_SHADER_CACHE=%CACHE%"
set "D3D8_COMPILEAHEAD=0"
set "D3D8_LOG=%~dp0..\build\d3d8\warm.log"
build\d3d8\d3d8_frame.exe "%~dp0..\build\d3d8\d3d8.dll" scenes "%~dp0..\build\d3d8\warm-scenes" >nul || exit /b 1
for %%c in (day_npcs night water spell menu) do (
  build\d3d8\d3d8_frame.exe "%~dp0..\build\d3d8\d3d8.dll" replay "C:\Games\PhoenixXI\d3dlog\%%c.txt" "%~dp0..\build\d3d8\warm-%%c" 3 >nul || exit /b 1
)
set /a N=0
for %%f in ("%CACHE%\*.bin") do set /a N+=1
echo build\d3d8\d3d8-shader-cache: %N% files (keys.bin and the blobs)
rem (the keys kept in the repository, for build_d3d8.cmd to compile the shipped cache from: commit the change)
copy /y "%CACHE%\keys.bin" data\d3d8\keys.bin >nul || exit /b 1
echo data\d3d8\keys.bin updated
exit /b 0
