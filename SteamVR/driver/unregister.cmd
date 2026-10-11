@echo off
setlocal
if /I "%~1"=="--dry-run" goto dryrun
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0manage-driver.ps1" -Mode Remove
set "taskRemoveExit=%errorlevel%"
pause
exit /b %taskRemoveExit%
:dryrun
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0manage-driver.ps1" -Mode Remove -DryRun
exit /b %errorlevel%
