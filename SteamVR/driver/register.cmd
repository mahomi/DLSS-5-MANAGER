@echo off
setlocal
if /I "%~1"=="--dry-run" goto dryrun
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0manage-driver.ps1" -Mode Install
set "taskRegisterExit=%errorlevel%"
pause
exit /b %taskRegisterExit%
:dryrun
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0manage-driver.ps1" -Mode Install -DryRun
exit /b %errorlevel%
