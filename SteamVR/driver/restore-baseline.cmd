@echo off
setlocal
if /I "%~1"=="--dry-run" goto dryrun
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0manage-driver.ps1" -Mode RestoreBaseline
set "taskRestoreExit=%errorlevel%"
pause
exit /b %taskRestoreExit%
:dryrun
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0manage-driver.ps1" -Mode RestoreBaseline -DryRun
exit /b %errorlevel%
