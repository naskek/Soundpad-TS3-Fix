$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$vendorDir = Join-Path $repoRoot "vendor"
$sdkDir = Join-Path $vendorDir "ts3client-pluginsdk"

New-Item -ItemType Directory -Force -Path $vendorDir | Out-Null

if (Test-Path (Join-Path $sdkDir "include\ts3_functions.h")) {
    Write-Host "[OK] TeamSpeak 3 Client Plugin SDK already present:"
    Write-Host "     $sdkDir"
    exit 0
}

if (Test-Path $sdkDir) {
    Remove-Item -Recurse -Force $sdkDir
}

Write-Host "[INFO] Cloning official TeamSpeak 3 Client Plugin SDK..."
git clone --depth 1 https://github.com/teamspeak/ts3client-pluginsdk.git $sdkDir

if ($LASTEXITCODE -ne 0) {
    throw "git clone failed."
}

Write-Host "[OK] SDK ready:"
Write-Host "     $sdkDir"
