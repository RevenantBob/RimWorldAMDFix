# RimWorldAMDFix uninstaller. Removes everything the installer added:
# the layer registration, the "Installed apps" entry and %LOCALAPPDATA%\RimWorldAMDFix.
param([switch]$Quiet)
$ErrorActionPreference = 'Stop'

$BaseDir = Join-Path $env:LOCALAPPDATA 'RimWorldAMDFix'
$LayerKey = 'HKCU:\Software\Khronos\Vulkan\ImplicitLayers'
$UninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\RimWorldAMDFix'

function Say($text, $color) {
    if ($Quiet) { return }
    if ($color) { Write-Host $text -ForegroundColor $color } else { Write-Host $text }
}

Say ''
Say '  RimWorldAMDFix uninstaller' 'Cyan'
Say '  --------------------------'

$removed = $false
if (Test-Path $LayerKey) {
    $layerProps = Get-Item $LayerKey
    foreach ($name in $layerProps.GetValueNames()) {
        if ($name -like "$BaseDir\*") {
            Remove-ItemProperty -Path $LayerKey -Name $name
            $removed = $true
        }
    }
}
if ($removed) { Say '  Unregistered the Vulkan layer' }

if (Test-Path $UninstallKey) { Remove-Item $UninstallKey -Recurse -Force }

$leftover = $false
if (Test-Path $BaseDir) {
    Set-Location $env:TEMP  # this script may be running from inside the folder it deletes
    try { Remove-Item $BaseDir -Recurse -Force }
    catch { $leftover = $true }
}

if ($leftover) {
    Say '  The fix is turned off, but some files are still in use by a running program.' 'Yellow'
    Say "  Close it and delete this folder to finish: $BaseDir" 'Yellow'
} else {
    Say '  Removed RimWorldAMDFix.' 'Green'
}
