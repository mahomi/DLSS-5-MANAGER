@echo off
setlocal
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cl /nologo /W4 /O2 /MT /EHsc /std:c++17 live_route_self_test.cpp /Fe:live_route_self_test.exe /link d3d11.lib dxgi.lib
if errorlevel 1 exit /b 1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-isolated-route-test.ps1" %*
set "taskLiveRouteExit=%errorlevel%"
exit /b %taskLiveRouteExit%
