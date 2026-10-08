#requires -Version 5.1
<#
.SYNOPSIS
    Build the cw-mod client DLL (Release x64).

    Auto-detects your installed MSVC PlatformToolset, so it works on both
    Visual Studio 2022 (v143) and newer (VS2026 ships v145) without you having
    to install the exact toolset the upstream solution was generated against.

.PARAMETER Configuration
    Release (default) or Debug.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\build.ps1
#>
param(
    [ValidateSet("Release","Debug")]
    [string]$Configuration = "Release"
)
$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$sln  = Join-Path $repoRoot "t9_vs2022.sln"

if (-not (Test-Path $sln)) { throw "Solution not found. Run scripts\bootstrap.ps1 first." }

# --- Locate Visual Studio + MSBuild via vswhere ---
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere not found - is Visual Studio installed?" }
$vsPath = & $vswhere -latest -requires Microsoft.Component.MSBuild -property installationPath
if (-not $vsPath) { throw "No Visual Studio with MSBuild found." }
$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
if (-not $msbuild) { throw "MSBuild.exe not found." }

# --- Detect an installed C++ PlatformToolset (prefer v143 to match upstream/CI) ---
$toolsetRoots = Get-ChildItem (Join-Path $vsPath "MSBuild\Microsoft\VC") -Directory -ErrorAction SilentlyContinue
$found = @()
foreach ($r in $toolsetRoots) {
    $p = Join-Path $r.FullName "Platforms\x64\PlatformToolsets"
    if (Test-Path $p) { $found += (Get-ChildItem $p -Directory).Name }
}
$found = $found | Sort-Object -Unique
if (-not $found) { throw "No C++ PlatformToolset found. Install the 'Desktop development with C++' workload." }
if ($found -contains "v143") { $toolset = "v143" }
else { $toolset = ($found | Sort-Object { [int]($_ -replace '\D','') } -Descending | Select-Object -First 1) }

Write-Host "==> Building cw-mod" -ForegroundColor Cyan
Write-Host ("    MSBuild : {0}" -f $msbuild)
Write-Host ("    Toolset : {0}  (available: {1})" -f $toolset, ($found -join ", "))
Write-Host ("    Config  : {0} x64" -f $Configuration)

& $msbuild /m /p:Configuration=$Configuration /p:Platform=x64 /p:PlatformToolset=$toolset $sln
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }

$dll = Join-Path $repoRoot "build\t9_vs2022\x64\client\discord_game_sdk.dll"
if (Test-Path $dll) {
    $f = Get-Item $dll
    Write-Host ""
    Write-Host ("BUILD OK -> {0}  ({1:N0} bytes)" -f $f.FullName, $f.Length) -ForegroundColor Green
    Write-Host "Next: inject with scripts\launch.ps1 -GamePath '<your BOCW folder>'"
} else {
    throw "Build reported success but DLL is missing at $dll"
}
