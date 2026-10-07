# Builds the Windows-side deployer.
#
#   .\build\build_win.ps1
#
# Needs: Visual Studio 2022 (or Build Tools) with the C++ workload, cmake.
param(
    [string]$BuildType = "Release"
)

$ErrorActionPreference = "Stop"

$repo   = Split-Path -Parent $PSScriptRoot
$srcDir = Join-Path $repo "win\nhdeploy"
$b      = Join-Path $repo "out\build\nhdeploy-win"
$outDir = Join-Path $repo "out\win"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

& cmake -S $srcDir -B $b -A x64
if ($LASTEXITCODE -ne 0) { throw "configure failed" }

& cmake --build $b --config $BuildType --parallel
if ($LASTEXITCODE -ne 0) { throw "build failed" }

Copy-Item (Join-Path $b "$BuildType\nhdeploy.exe") $outDir -Force
Write-Host "`n[build] done -> $outDir\nhdeploy.exe"
