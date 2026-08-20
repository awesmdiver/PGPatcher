# Fast incremental dev build -- rebuilds whatever changed in the EXISTING build/ directory (already
# CMake-configured, Ninja generator) and nothing else. NOT the same as buildRelease.ps1, which wipes
# build/, does a full clean configure+build, and packages a distribution zip -- that's for cutting a
# real release, not for "I changed one line, rebuild pgtools.exe and let me test it" iteration.
#
# Usage:
#   .\build.ps1                  # builds the default target (pgtools)
#   .\build.ps1 -Target PGPatcher   # builds the GUI exe instead
#   .\build.ps1 -Target ALL         # builds everything

param(
    [string]$Target = "pgtools"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptDir = $PSScriptRoot
$buildDir = Join-Path -Path $scriptDir -ChildPath "build"

if (-not (Test-Path $buildDir)) {
    throw "build/ doesn't exist yet -- run buildRelease.ps1 once first (or a manual cmake configure) to set it up. This script only does incremental rebuilds of an already-configured tree."
}

# Same MSVC x64 environment Start-Claude.ps1 loads for any Skyrim-tooling session -- needed here too
# since this script may be run from a terminal that wasn't launched through that launcher (e.g. a
# session rooted directly in this repo instead of the Skyrim game folder).
$vsBase = "C:\Program Files\Microsoft Visual Studio\18\Community"
$vcvars = "$vsBase\VC\Auxiliary\Build\vcvarsall.bat"
if (Test-Path $vcvars) {
    Write-Host "Loading MSVC x64 environment..." -ForegroundColor Cyan
    $envBlock = cmd /c "`"$vcvars`" x64 >nul 2>&1 && set" 2>&1
    foreach ($line in $envBlock) {
        if ($line -match '^([^=]+)=(.*)$') {
            [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
        }
    }
} else {
    Write-Host "WARNING: vcvarsall.bat not found at $vcvars -- assuming the environment is already set up (e.g. this shell was launched via Start-Claude.ps1)." -ForegroundColor Yellow
}

$ninjaDir = "$vsBase\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
if (Test-Path $ninjaDir) {
    $env:PATH = "$ninjaDir;$env:PATH"
}

Write-Host "Building target '$Target' in $buildDir ..." -ForegroundColor Cyan
if ($Target -eq "ALL") {
    cmake --build $buildDir
} else {
    cmake --build $buildDir --target $Target
}

if ($LASTEXITCODE -ne 0) {
    Write-Host "Build FAILED (exit $LASTEXITCODE)." -ForegroundColor Red
    exit $LASTEXITCODE
}

Write-Host "Build succeeded. Output: $buildDir\bin\" -ForegroundColor Green
