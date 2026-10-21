@echo off
rem Installs RimWorldAMDFix for the current user. No administrator rights needed.

rem The PowerShell script lives in the "files" folder next to this file. It is missing when this file
rem is run from inside a zip without extracting it first: Windows then copies only this one file to a
rem temporary folder.
if not exist "%~dp0files\Install.ps1" (
    echo.
    echo   RimWorldAMDFix could not find its files.
    echo.
    echo   Please extract the whole zip first: right-click the zip, choose "Extract All...",
    echo   then open the extracted folder and run Install.bat from there.
    echo.
    pause
    exit /b 1
)

rem When this is started from a PowerShell 7 window, PSModulePath points at PowerShell 7's modules and
rem Windows PowerShell 5.1 then fails to load its own (Get-FileHash, Unblock-File). Clearing it makes
rem 5.1 use its defaults.
set "PSModulePath="

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0files\Install.ps1"

echo.
pause
