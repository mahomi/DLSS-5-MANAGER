@echo off
setlocal
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cl /nologo /O2 /EHsc /W4 /MD /utf-8 /std:c++17 verify_fast_multipass.cpp /Fe:verify_fast_multipass.exe /link d3d12.lib dxgi.lib d3dcompiler.lib shell32.lib uuid.lib ole32.lib
exit /b %errorlevel%
