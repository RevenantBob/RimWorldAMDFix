# Builds RimWorldAMDFix and packages the release zip into dist\.
#
#   .\build.ps1                 # MSVC if Visual Studio is installed, otherwise zig
#   .\build.ps1 -Compiler zig
#
# The only dependency is the Vulkan headers (pinned below, downloaded once into build\deps).
param(
    [ValidateSet('auto', 'msvc', 'zig')] [string]$Compiler = 'auto'
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$Root = $PSScriptRoot
$Version = (Get-Content (Join-Path $Root 'VERSION') -Raw).Trim()
$HeadersTag = 'v1.4.341'
$BuildDir = Join-Path $Root 'build'
$DepsDir = Join-Path $BuildDir 'deps'
$GenDir = Join-Path $BuildDir 'gen'
$OutDir = Join-Path $BuildDir 'out'
$DistDir = Join-Path $Root 'dist'
$Dll = Join-Path $OutDir 'RimWorldAMDFix.dll'

New-Item -ItemType Directory -Force -Path $DepsDir, $GenDir, $OutDir, $DistDir | Out-Null

# Vulkan headers
$HeadersDir = Join-Path $DepsDir "Vulkan-Headers-$($HeadersTag.TrimStart('v'))"
if (-not (Test-Path (Join-Path $HeadersDir 'include\vulkan\vk_layer.h'))) {
    $zip = Join-Path $DepsDir "Vulkan-Headers-$HeadersTag.zip"
    Write-Host "Downloading Vulkan-Headers $HeadersTag"
    Invoke-WebRequest "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/$HeadersTag.zip" -OutFile $zip
    Expand-Archive $zip -DestinationPath $DepsDir -Force
}
$Include = Join-Path $HeadersDir 'include'

Set-Content -Path (Join-Path $GenDir 'version.h') -Value "#define RWAMDFIX_VERSION `"$Version`"" -Encoding Ascii
$Source = Join-Path $Root 'src\RimWorldAMDFix.c'

function Find-VcVars {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $null }
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { return $null }
    $bat = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    if (Test-Path $bat) { return $bat }
    return $null
}

$vcvars = Find-VcVars
if ($Compiler -eq 'auto') {
    if ($vcvars) { $Compiler = 'msvc' }
    elseif (Get-Command zig -ErrorAction SilentlyContinue) { $Compiler = 'zig' }
    else { throw 'No compiler found. Install Visual Studio (Desktop development with C++) or zig.' }
}

Write-Host "Building RimWorldAMDFix $Version with $Compiler"
if (Test-Path $Dll) { Remove-Item $Dll -Force }
if ($Compiler -eq 'msvc') {
    if (-not $vcvars) { throw 'Visual Studio C++ tools not found.' }
    $obj = Join-Path $BuildDir 'obj'
    New-Item -ItemType Directory -Force -Path $obj | Out-Null
    # /MT links the C runtime statically, so users need no Visual C++ redistributable.
    $cl = "cl /nologo /O2 /MT /W3 /LD /I`"$Include`" /I`"$GenDir`" /FIversion.h `"$Source`" /Fo`"$obj\\`" /Fe`"$Dll`" /link /NOLOGO"
    # vcvars64.bat calls vswhere.exe from PATH
    $env:PATH = (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer') + ';' + $env:PATH
    cmd /c "call `"$vcvars`" >nul && $cl"
    if ($LASTEXITCODE -ne 0) { throw 'MSVC build failed.' }
} else {
    & zig cc -O2 -shared -target x86_64-windows-gnu "-I$Include" "-I$GenDir" -include version.h $Source -o $Dll
    if ($LASTEXITCODE -ne 0) { throw 'zig build failed.' }
    Get-ChildItem $OutDir -Filter 'RimWorldAMDFix.*' | Where-Object { $_.Extension -in '.lib', '.pdb' } | Remove-Item -Force
}
if (-not (Test-Path $Dll)) { throw 'Build produced no DLL.' }

# Package layout:
#   RimWorldAMDFix-vX.Y.Z\Install.bat, Uninstall.bat, README.txt, LICENSE.txt
#   RimWorldAMDFix-vX.Y.Z\files\RimWorldAMDFix.dll, VkLayer_RimWorldAMDFix.json, Install.ps1, Uninstall.ps1, VERSION
$PkgName = "RimWorldAMDFix-v$Version"
$Pkg = Join-Path $DistDir $PkgName
$Files = Join-Path $Pkg 'files'
if (Test-Path $Pkg) { Remove-Item $Pkg -Recurse -Force }
New-Item -ItemType Directory -Force -Path $Files | Out-Null

function Copy-Crlf($from, $to) {
    # .bat files must have CRLF line endings; normalise everything text-based for Windows users.
    $text = [IO.File]::ReadAllText($from) -replace "`r`n", "`n" -replace "`n", "`r`n"
    [IO.File]::WriteAllText($to, $text, (New-Object Text.ASCIIEncoding))
}

Copy-Item $Dll $Files
$manifest = [IO.File]::ReadAllText((Join-Path $Root 'src\VkLayer_RimWorldAMDFix.json')).Replace('@VERSION@', $Version)
[IO.File]::WriteAllText((Join-Path $Files 'VkLayer_RimWorldAMDFix.json'), $manifest, (New-Object Text.ASCIIEncoding))
Copy-Crlf (Join-Path $Root 'installer\Install.ps1') (Join-Path $Files 'Install.ps1')
Copy-Crlf (Join-Path $Root 'installer\Uninstall.ps1') (Join-Path $Files 'Uninstall.ps1')
[IO.File]::WriteAllText((Join-Path $Files 'VERSION'), "$Version`r`n", (New-Object Text.ASCIIEncoding))
Copy-Crlf (Join-Path $Root 'installer\Install.bat') (Join-Path $Pkg 'Install.bat')
Copy-Crlf (Join-Path $Root 'installer\Uninstall.bat') (Join-Path $Pkg 'Uninstall.bat')
Copy-Crlf (Join-Path $Root 'installer\README.txt') (Join-Path $Pkg 'README.txt')
Copy-Crlf (Join-Path $Root 'LICENSE') (Join-Path $Pkg 'LICENSE.txt')

# Release description used by the GitHub workflow: the template with this version filled in.
$notes = [IO.File]::ReadAllText((Join-Path $Root '.github\release-notes.md')).Replace('@VERSION@', $Version)
[IO.File]::WriteAllText((Join-Path $DistDir 'release-notes.md'), $notes, (New-Object Text.UTF8Encoding $false))

$Zip = Join-Path $DistDir "$PkgName.zip"
if (Test-Path $Zip) { Remove-Item $Zip -Force }
Compress-Archive -Path $Pkg -DestinationPath $Zip
Write-Host "Package: $Zip"
