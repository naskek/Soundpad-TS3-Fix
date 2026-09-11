$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$bootstrap = Join-Path $PSScriptRoot "bootstrap.ps1"
$buildDir = Join-Path $repoRoot "build"

& $bootstrap

Write-Host "[INFO] Configuring x64 Release build..."
cmake -S $repoRoot -B $buildDir -A x64
if ($LASTEXITCODE -ne 0) {
    throw "CMake configure failed."
}

Write-Host "[INFO] Building..."
cmake --build $buildDir --config Release
if ($LASTEXITCODE -ne 0) {
    throw "Build failed."
}

$dll = Join-Path $buildDir "Release\soundpad_ts3_diag_win64.dll"
if (-not (Test-Path $dll)) {
    throw "Expected DLL not found: $dll"
}

Write-Host ""
Write-Host "[OK] Built:"
Write-Host "     $dll"
