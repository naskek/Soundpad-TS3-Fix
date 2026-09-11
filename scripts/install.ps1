$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$dll = Join-Path $repoRoot "build\Release\soundpad_ts3_diag_win64.dll"
$pluginDir = Join-Path $env:APPDATA "TS3Client\plugins"
$destination = Join-Path $pluginDir "soundpad_ts3_diag_win64.dll"

if (-not (Test-Path $dll)) {
    throw "Plugin DLL not found. Run scripts\build.ps1 first. Expected: $dll"
}

New-Item -ItemType Directory -Force -Path $pluginDir | Out-Null
Copy-Item -Force $dll $destination

Write-Host "[OK] Installed:"
Write-Host "     $destination"
Write-Host ""
Write-Host "Restart TeamSpeak 3, then check Tools -> Options -> Addons/Plugins."
