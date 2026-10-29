@echo off
rem Builds HiFi XI's frame generation (src\framegen: AMD's FidelityFX SDK v1.1.4 frame interpolation, optical flow, DX12
rem backend and frame interpolation swap chain, MIT, in src\framegen\ffx) into <folder>\ffx_fg.lib:
rem   call build_framegen.cmd <folder>
rem Its shaders are compiled with the Windows SDK's DXC (dxcompiler.dll, Microsoft's, signed; dxil.dll beside it signs the
rem output) by tools\ffx_sc into <folder>\fg_shaders.inc, built in. Needs the Windows SDK 10.0.26100 installed (its DXC
rem and headers: dxcapi.h, d3d12shader.h, dxgi1_6.h). From a build.cmd that has set up Visual Studio's x86 compiler.
setlocal
if "%~1"=="" (
  echo build_framegen.cmd: no output folder given
  exit /b 1
)
set "OUT=%~f1"
set "HERE=%~dp0"
set "KIT=C:\Program Files (x86)\Windows Kits\10"
set "KITVER=10.0.26100.0"
if not exist "%KIT%\bin\%KITVER%\x86\dxcompiler.dll" (
  echo build_framegen.cmd: the Windows SDK %KITVER% isn't installed ^(its DXC is needed for frame generation's shaders^)
  exit /b 1
)
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
set "INCLUDE=%KIT%\Include\%KITVER%\ucrt;%KIT%\Include\%KITVER%\um;%KIT%\Include\%KITVER%\shared;%INCLUDE%"
set "LIB=%KIT%\Lib\%KITVER%\ucrt\x86;%KIT%\Lib\%KITVER%\um\x86;%LIB%"
pushd "%HERE%"
rem The shader tool, then every pass and permutation compiled and reflected
cl /nologo /std:c++17 /O2 /MT /EHsc /W4 /utf-8 /DNOMINMAX tools\ffx_sc\ffx_sc.cpp /Fo"%OUT%\obj\\" /Fe"%OUT%\ffx_sc.exe" /link /NOLOGO || goto failed
"%OUT%\ffx_sc.exe" "%KIT%\bin\%KITVER%\x86\dxcompiler.dll" src\framegen\ffx "%OUT%\fg_shaders.inc" || goto failed
rem AMD's code and HiFi XI's own, with fg_names.h force-included (the names FSR 2's copy also has, apart)
set "FFX=src\framegen\ffx"
cl /nologo /c /std:c++17 /O2 /MT /EHsc /W3 /utf-8 /DNOMINMAX /DNDEBUG /DUNICODE /D_UNICODE /DFFX_FI /DFFX_OF ^
  /D_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING /I src\framegen /FI fg_names.h ^
  /I %FFX%\include /I %FFX%\src\shared /I %FFX%\src\backends\shared /I %FFX%\src\components /I src\framegen\shim /I "%OUT%" ^
  %FFX%\src\components\frameinterpolation\ffx_frameinterpolation.cpp %FFX%\src\components\opticalflow\ffx_opticalflow.cpp ^
  %FFX%\src\backends\dx12\ffx_dx12.cpp %FFX%\src\backends\dx12\FrameInterpolationSwapchain\FrameInterpolationSwapchainDX12.cpp ^
  %FFX%\src\backends\dx12\FrameInterpolationSwapchain\FrameInterpolationSwapchainDX12_DebugPacing.cpp ^
  %FFX%\src\backends\dx12\FrameInterpolationSwapchain\FrameInterpolationSwapchainDX12_Helpers.cpp ^
  %FFX%\src\backends\dx12\FrameInterpolationSwapchain\FrameInterpolationSwapchainDX12_UiComposition.cpp ^
  %FFX%\src\shared\ffx_assert.cpp %FFX%\src\shared\ffx_breadcrumbs_list.cpp %FFX%\src\shared\ffx_message.cpp %FFX%\src\shared\ffx_object_management.cpp ^
  src\framegen\ffx_shader_blobs.cpp src\framegen\framegen.cpp /Fo"%OUT%\obj\\" || goto failed
lib /nologo /OUT:"%OUT%\ffx_fg.lib" "%OUT%\obj\*.obj" || goto failed
rem Its swap chain in dgVoodoo's place, offline on WARP: build\framegen_test.exe
cl /nologo /std:c++17 /O2 /MT /EHsc /W4 /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG tests\framegen_test.cpp "%OUT%\ffx_fg.lib" ^
  /Fo"%OUT%\obj\\" /Fe"build\framegen_test.exe" /link /NOLOGO user32.lib d3d12.lib dxgi.lib || goto failed
popd
echo Built %OUT%\ffx_fg.lib, build\framegen_test.exe
exit /b 0
:failed
popd
exit /b 1
