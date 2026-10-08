#requires -Version 5.1
<#
.SYNOPSIS
    Inject the built cw-mod DLL into Black Ops Cold War and launch it.

    Injection vector: the game loads 'discord_game_sdk.dll' from its own folder.
    We drop our built DLL there under that name, so the game loads our code.

.PARAMETER GamePath
    Path to your BOCW install folder (the one containing BlackOpsColdWar.exe).
    Remembered in scripts\.gamepath after the first run.

.PARAMETER Exe
    Executable to start (default BlackOpsColdWar.exe).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\launch.ps1 -GamePath "D:\Games\Call of Duty Black Ops Cold War"

.NOTES
    Target build: 1.34.0.15931218 (also supports 1.34.1.15931218).
    You must legally own the game. No game files are shipped by this project.
#>
param(
    [string]$GamePath,
    [string]$Exe = "BlackOpsColdWar.exe"
)
$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$dll = Join-Path $repoRoot "build\t9_vs2022\x64\client\discord_game_sdk.dll"
$rememberFile = Join-Path $PSScriptRoot ".gamepath"

if (-not (Test-Path $dll)) { throw "DLL not built. Run scripts\build.ps1 first." }

# Resolve game path: parameter -> remembered file -> error.
if (-not $GamePath -and (Test-Path $rememberFile)) { $GamePath = (Get-Content $rememberFile -Raw).Trim() }
if (-not $GamePath) { throw "Provide -GamePath '<your BOCW folder>' (contains $Exe). It will be remembered afterwards." }
if (-not (Test-Path (Join-Path $GamePath $Exe))) { throw "Couldn't find $Exe in '$GamePath'." }
Set-Content -Path $rememberFile -Value $GamePath -Encoding utf8

Write-Host "==> Injecting and launching cw-mod" -ForegroundColor Cyan
Copy-Item $dll (Join-Path $GamePath "discord_game_sdk.dll") -Force
Write-Host ("    DLL copied to {0}" -f $GamePath)
Write-Host ("    Starting {0}" -f $Exe)
Start-Process -FilePath (Join-Path $GamePath $Exe) -WorkingDirectory $GamePath
Write-Host "Launched. If the mod console/menu doesn't appear, see docs\build.md (Troubleshooting)." -ForegroundColor Green
