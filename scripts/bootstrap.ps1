#requires -Version 5.1
<#
.SYNOPSIS
    Generate the Visual Studio solution for cw-mod.

    The mod source and all third-party libs are vendored directly in this repo
    (no submodules), so setup is just: generate the solution with premake.

    Premake itself is not in the repo (binaries are git-ignored). The first run downloads the
    pinned official release into tools\premake, where the build's own prebuild step expects it.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1
#>
$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot

Write-Host "==> cw-mod bootstrap" -ForegroundColor Cyan

# --- Premake 5.0.0-beta2, from the official release, checked against its SHA-256 ---
$premakeDir = Join-Path $repoRoot "tools\premake"
$premakeExe = Join-Path $premakeDir "premake5.exe"
if (-not (Test-Path $premakeExe)) {
    $url  = "https://github.com/premake/premake-core/releases/download/v5.0.0-beta2/premake-5.0.0-beta2-windows.zip"
    $hash = "87cfa10ed52fd1f4e835f738ac1033ff302035758671400fec078b700c622c54"
    $zip  = Join-Path $env:TEMP "premake-5.0.0-beta2-windows.zip"

    Write-Host "--> Downloading premake 5.0.0-beta2"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
    $got = (Get-FileHash $zip -Algorithm SHA256).Hash
    if ($got -ne $hash) { Remove-Item $zip -Force; throw "premake download has the wrong SHA-256 ($got)." }

    New-Item -ItemType Directory -Force $premakeDir | Out-Null
    Expand-Archive -Path $zip -DestinationPath $premakeDir -Force
    Remove-Item $zip -Force
    if (-not (Test-Path $premakeExe)) { throw "premake5.exe is missing from the downloaded archive." }
}

Write-Host "--> Generating Visual Studio solution (premake5 vs2022)"
Push-Location $repoRoot
try {
    & ".\tools\premake\premake5.exe" vs2022
    if ($LASTEXITCODE -ne 0) { throw "premake failed with exit code $LASTEXITCODE" }
} finally { Pop-Location }

Write-Host ""
Write-Host "Bootstrap complete. Next: scripts\build.ps1" -ForegroundColor Green
