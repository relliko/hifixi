@echo off
rem one.cmd <arch> <name>: probe.cpp -> <name>.exe for that architecture (VSDIR from b.cmd)
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" %1 >nul 2>&1 || exit /b 1
cd /d "%~dp0"
cl /nologo /std:c++17 /O2 /EHsc /W3 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS probe.cpp /Fe:%2.exe /Fo:%2.obj || exit /b 1
