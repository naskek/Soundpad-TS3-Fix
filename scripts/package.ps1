$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$dll = Join-Path $repoRoot "build\Release\soundpad_ts3_diag_win64.dll"
$dist = Join-Path $repoRoot "dist"
$staging = Join-Path $dist "package"
$pluginsDir = Join-Path $staging "plugins"
$packageIni = Join-Path $staging "package.ini"
$packageFile = Join-Path $dist "soundpad_ts3_fix.ts3_plugin"

if (-not (Test-Path $dll)) {
    throw "Plugin DLL not found. Run scripts\build.ps1 first. Expected: $dll"
}

if (Test-Path $staging) {
    Remove-Item -Recurse -Force $staging
}
New-Item -ItemType Directory -Force -Path $pluginsDir | Out-Null

Copy-Item -Force $dll (Join-Path $pluginsDir "soundpad_ts3_diag_win64.dll")

@"
Name = Soundpad TS3 Fix
Type = Plugin
Author = naskek / OpenAI
Version = 1.0.0
Platforms = win64
Description = Automatically bypasses supported TeamSpeak capture DSP while Soundpad is playing and restores the original settings.
"@ | Set-Content -Encoding UTF8 $packageIni

if (Test-Path $packageFile) {
    Remove-Item -Force $packageFile
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::Open($packageFile, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
        $zip,
        $packageIni,
        "package.ini"
    ) | Out-Null
    [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
        $zip,
        (Join-Path $pluginsDir "soundpad_ts3_diag_win64.dll"),
        "plugins/soundpad_ts3_diag_win64.dll"
    ) | Out-Null
}
finally {
    $zip.Dispose()
}

Remove-Item -Recurse -Force $staging

Write-Host "[OK] Packaged:"
Write-Host "     $packageFile"
