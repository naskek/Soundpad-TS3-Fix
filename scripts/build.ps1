$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$bootstrap = Join-Path $PSScriptRoot "bootstrap.ps1"
$buildDir = Join-Path $repoRoot "build"

& $bootstrap

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw @"
Microsoft Visual Studio 2022 Build Tools with the C++ workload are required.

Install them with:

winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"

Then open a new PowerShell window and run this script again.
"@
}

$vsInstall = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) {
    throw @"
Visual Studio Build Tools were found, but the MSVC C++ x64 toolchain is missing.

Run:

winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"

Then open a new PowerShell window and run this script again.
"@
}

Write-Host "[OK] MSVC toolchain found:"
Write-Host "     $vsInstall"

if (Test-Path $buildDir) {
    Remove-Item -Recurse -Force $buildDir
}

Write-Host "[INFO] Configuring x64 Release build with Visual Studio 2022..."
cmake -S $repoRoot -B $buildDir -G "Visual Studio 17 2022" -A x64
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
