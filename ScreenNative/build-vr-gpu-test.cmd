@echo off
setlocal
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cl /nologo /O2 /EHsc /W3 /MD /utf-8 /std:c++17 vr_gpu_self_test.cpp /Fe:vr_gpu_self_test.exe /link d3d11.lib dxgi.lib user32.lib
exit /b %errorlevel%
