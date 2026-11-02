@echo off
rem b.cmd: rt.cso, then host32.exe (x86) and helper64.exe (x64), each in a shell of its own
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
cd /d "%~dp0"
"%ProgramFiles(x86)%\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe" -nologo -T cs_6_5 -E main rt.hlsl -Fo rt.cso || exit /b 1
cmd /c "%~dp0one.cmd" x86 host32 || exit /b 1
cmd /c "%~dp0one.cmd" x64 helper64 || exit /b 1
