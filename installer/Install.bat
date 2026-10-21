@echo off
rem Installs RimWorldAMDFix for the current user. No administrator rights needed.

rem When this is started from a PowerShell 7 window, PSModulePath points at PowerShell 7's modules and
rem Windows PowerShell 5.1 then fails to load its own (Get-FileHash, Unblock-File). Clearing it makes
rem 5.1 use its defaults.
set "PSModulePath="

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0files\Install.ps1"

echo.
pause
