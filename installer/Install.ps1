# RimWorldAMDFix installer. Per-user, no administrator rights needed.
#
# - Copies the Vulkan layer to %LOCALAPPDATA%\RimWorldAMDFix\<version>
# - Registers it as an implicit Vulkan layer (HKCU\Software\Khronos\Vulkan\ImplicitLayers)
# - Adds an entry to Windows "Installed apps" so it can be removed from Settings
# - Clears RimWorld's cached texture atlases so they are rebuilt with the fix
$ErrorActionPreference = 'Stop'

$PackageDir = $PSScriptRoot
$BaseDir = Join-Path $env:LOCALAPPDATA 'RimWorldAMDFix'
$LayerKey = 'HKCU:\Software\Khronos\Vulkan\ImplicitLayers'
$UninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\RimWorldAMDFix'
$AtlasCache = Join-Path $env:USERPROFILE 'AppData\LocalLow\Ludeon Studios\RimWorld by Ludeon Studios\TextureCache'
$ManifestName = 'VkLayer_RimWorldAMDFix.json'

function Fail($message) {
    Write-Host ''
    Write-Host "  Install failed: $message" -ForegroundColor Red
    exit 1
}

Write-Host ''
Write-Host '  RimWorldAMDFix installer' -ForegroundColor Cyan
Write-Host '  ------------------------'

if (-not [Environment]::Is64BitOperatingSystem) { Fail 'RimWorldAMDFix needs 64-bit Windows.' }
foreach ($f in @('RimWorldAMDFix.dll', $ManifestName, 'VERSION', 'Uninstall.ps1')) {
    if (-not (Test-Path (Join-Path $PackageDir $f))) { Fail "the package is incomplete ($f is missing). Extract the whole zip and try again." }
}
if (Get-Process -Name 'RimWorld', 'RimWorldWin64' -ErrorAction SilentlyContinue) { Fail 'RimWorld is running. Close it and run the installer again.' }

$Version = (Get-Content (Join-Path $PackageDir 'VERSION') -Raw).Trim()
$InstallDir = Join-Path $BaseDir $Version
$Manifest = Join-Path $InstallDir $ManifestName

# Copy the layer into a folder per version, so an upgrade never has to overwrite a DLL that is in use.
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
foreach ($f in @('RimWorldAMDFix.dll', $ManifestName)) {
    $from = Join-Path $PackageDir $f
    $to = Join-Path $InstallDir $f
    if ((Test-Path $to) -and ((Get-FileHash $from).Hash -eq (Get-FileHash $to).Hash)) { continue }
    try { Copy-Item $from $to -Force }
    catch { Fail "could not write $to. Close any running games or 3D programs and try again." }
}
Copy-Item (Join-Path $PackageDir 'Uninstall.ps1') (Join-Path $BaseDir 'Uninstall.ps1') -Force
Get-ChildItem $BaseDir -Recurse -File | Unblock-File
Write-Host "  Installed version $Version to $InstallDir"

# Register this version's manifest and drop registrations of older versions.
if (-not (Test-Path $LayerKey)) { New-Item -Path $LayerKey -Force | Out-Null }
$layerProps = Get-Item $LayerKey
foreach ($name in $layerProps.GetValueNames()) {
    if ($name -like "$BaseDir\*" -and $name -ne $Manifest) { Remove-ItemProperty -Path $LayerKey -Name $name }
}
New-ItemProperty -Path $LayerKey -Name $Manifest -PropertyType DWord -Value 0 -Force | Out-Null
Write-Host '  Registered the Vulkan layer'

# Remove folders of older versions; one still loaded by a running program is cleaned up next time.
Get-ChildItem $BaseDir -Directory | Where-Object { $_.FullName -ne $InstallDir } | ForEach-Object {
    try { Remove-Item $_.FullName -Recurse -Force } catch { }
}

# "Installed apps" entry, so the fix can be removed from Windows Settings.
$uninstallCmd = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "' + (Join-Path $BaseDir 'Uninstall.ps1') + '"'
if (-not (Test-Path $UninstallKey)) { New-Item -Path $UninstallKey -Force | Out-Null }
$sizeKb = [int]((Get-ChildItem $InstallDir -File | Measure-Object Length -Sum).Sum / 1KB)
$entries = @{
    DisplayName = 'RimWorld AMD Fix (Vulkan layer)'
    DisplayVersion = $Version
    Publisher = 'RimWorldAMDFix'
    URLInfoAbout = 'https://github.com/RevenantBob/RimWorldAMDFix'
    InstallLocation = $BaseDir
    UninstallString = $uninstallCmd
    QuietUninstallString = "$uninstallCmd -Quiet"
}
foreach ($k in $entries.Keys) { New-ItemProperty -Path $UninstallKey -Name $k -PropertyType String -Value $entries[$k] -Force | Out-Null }
foreach ($k in @('NoModify', 'NoRepair')) { New-ItemProperty -Path $UninstallKey -Name $k -PropertyType DWord -Value 1 -Force | Out-Null }
New-ItemProperty -Path $UninstallKey -Name 'EstimatedSize' -PropertyType DWord -Value $sizeKb -Force | Out-Null

# Atlases baked before the fix are cached and would be reused; delete them so the game rebuilds them.
$pages = @(Get-ChildItem -Path $AtlasCache -Recurse -File -Filter 'pages.*.zip*' -ErrorAction SilentlyContinue)
foreach ($p in $pages) { Remove-Item $p.FullName -Force }
if ($pages.Count) { Write-Host '  Cleared RimWorld''s cached texture atlases (they are rebuilt on the next start)' }

Write-Host ''
Write-Host '  Done! Start RimWorld as usual.' -ForegroundColor Green
Write-Host "  The fix writes a short log to $BaseDir\RimWorldAMDFix.log"
Write-Host '  To remove it, run Uninstall.bat or use Settings > Apps > Installed apps.'
