<#
.SYNOPSIS
  Build and run the alarm_core host tests.

.DESCRIPTION
  Uses gcc/cmake/ninja from PATH, or falls back to the toolchain bundled with
  JetBrains CLion (Windows). Configures host/ into build/host, builds, runs ctest.

.EXAMPLE
  pwsh tools/host_test.ps1
  pwsh tools/host_test.ps1 -Clean
#>
param(
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build/host'
. (Join-Path $PSScriptRoot 'host_env.ps1')

if ($Clean -and (Test-Path $build)) {
    Remove-Item -Recurse -Force $build
}

cmake -S (Join-Path $root 'host') -B $build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=gcc
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build $build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
ctest --test-dir $build --output-on-failure
exit $LASTEXITCODE
