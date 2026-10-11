@echo off
setlocal
cd /d "%~dp0"
if not exist bin\win64 mkdir bin\win64
if not exist logs mkdir logs
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cl /nologo /W4 /O2 /MT /EHsc /std:c++17 /LD observer.cpp /Fe:bin\win64\driver_codex_live_flow.dll /link d3d11.lib dxgi.lib
if errorlevel 1 exit /b 1
cl /nologo /W4 /O2 /MT /EHsc /std:c++17 observer_self_test.cpp /Fe:observer_self_test.exe
if errorlevel 1 exit /b 1
observer_self_test.exe "%~dp0bin\win64\driver_codex_live_flow.dll" > logs\mock-passthrough-result.txt 2>&1
if errorlevel 1 exit /b 1
cl /nologo /W4 /O2 /MT /EHsc /std:c++17 live_self_test.cpp /Fe:live_self_test.exe
if errorlevel 1 exit /b 1
live_self_test.exe > logs\live-control-result.txt 2>&1
if errorlevel 1 exit /b 1
type logs\mock-passthrough-result.txt
type logs\live-control-result.txt
