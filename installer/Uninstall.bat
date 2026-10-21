@echo off
rem Removes RimWorldAMDFix. You can also remove it from Settings > Apps > Installed apps.

rem See Install.bat: make Windows PowerShell 5.1 use its own module path.
set "PSModulePath="

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0files\Uninstall.ps1"

echo.
pause
