<#
.SYNOPSIS
  Puts a host C toolchain (gcc, cmake, ninja) on PATH. Dot-source it: . "$PSScriptRoot/host_env.ps1"

.DESCRIPTION
  Uses gcc/cmake/ninja from PATH, or falls back to the toolchain bundled with JetBrains CLion
  (Windows).
#>

function Find-Tool([string]$name) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

if (-not (Find-Tool 'gcc') -or -not (Find-Tool 'cmake') -or -not (Find-Tool 'ninja')) {
    $clionRoots = @(
        "$env:LOCALAPPDATA\Programs\CLion",
        "$env:ProgramFiles\JetBrains\CLion*"
    ) + (Get-ChildItem "$env:LOCALAPPDATA\JetBrains\Toolbox\apps\CLion" -Directory -ErrorAction SilentlyContinue |
         ForEach-Object { Get-ChildItem $_.FullName -Directory -ErrorAction SilentlyContinue } |
         ForEach-Object { $_.FullName })
    foreach ($pattern in $clionRoots) {
        foreach ($clion in (Get-Item $pattern -ErrorAction SilentlyContinue)) {
            $dirs = @(
                (Join-Path $clion.FullName 'bin\mingw\bin'),
                (Join-Path $clion.FullName 'bin\cmake\win\x64\bin'),
                (Join-Path $clion.FullName 'bin\ninja\win\x64')
            )
            if ((Test-Path $dirs[0]) -and (Test-Path $dirs[1]) -and (Test-Path $dirs[2])) {
                $env:PATH = ($dirs -join ';') + ';' + $env:PATH
                Write-Host "Using CLion toolchain from $($clion.FullName)"
                break
            }
        }
        if (Find-Tool 'gcc') { break }
    }
}

foreach ($tool in 'gcc', 'cmake', 'ninja') {
    if (-not (Find-Tool $tool)) {
        throw "$tool not found. Install a C toolchain (e.g. CLion, MSYS2 MinGW, or LLVM) or add it to PATH."
    }
}
