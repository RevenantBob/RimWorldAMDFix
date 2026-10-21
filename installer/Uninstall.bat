@echo off
rem Removes RimWorldAMDFix. You can also remove it from Settings > Apps > Installed apps.

rem See Install.bat: the script is missing when this file is run from inside an unextracted zip.
if not exist "%~dp0files\Uninstall.ps1" (
    echo.
    echo   RimWorldAMDFix could not find its files.
    echo.
    echo   Please extract the whole zip first, or remove the fix from
    echo   Settings ^> Apps ^> Installed apps ^> RimWorld AMD Fix ^(Vulkan layer^).
    echo.
    pause
    exit /b 1
)

rem See Install.bat: make Windows PowerShell 5.1 use its own module path.
set "PSModulePath="

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0files\Uninstall.ps1"

echo.
pause
