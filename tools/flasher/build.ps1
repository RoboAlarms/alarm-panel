# Build the RoboAlarms Flasher as one Windows program (docs/features/34).
#
#   pwsh tools/flasher/build.ps1                       # build/flasher/dist/RoboAlarms Flasher.exe
#
# No firmware goes inside: the Flasher downloads it from the firmware server (FLS-032), and a
# .roboflash file can still be opened in it by hand.
# A build environment of its own (build/flasher-venv, git-ignored) holds the Flasher's
# requirements and PyInstaller, so ESP-IDF's Python is never changed. tools/flasher/build.sh is
# the same for macOS and Linux: change one, change the other.
param(
    [string]$Python = ""
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$here = $PSScriptRoot
$out = Join-Path $root "build/flasher"
$venv = Join-Path $root "build/flasher-venv"

if (-not $Python) {
    $candidates = @("C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe", "py", "python")
    foreach ($c in $candidates) {
        if (Get-Command $c -ErrorAction SilentlyContinue) { $Python = $c; break }
    }
}
if (-not $Python) { throw "No Python found: pass -Python <path to python.exe> (3.10 or newer)" }

$py = Join-Path $venv "Scripts/python.exe"
if (-not (Test-Path $py)) {
    Write-Host "Creating the build environment in $venv"
    & $Python -m venv $venv
    if ($LASTEXITCODE -ne 0) { throw "venv failed" }
}
& $py -m pip install --disable-pip-version-check -q -r (Join-Path $here "requirements.txt") "pyinstaller>=6.10,<7"
if ($LASTEXITCODE -ne 0) { throw "pip install failed" }

# The tests first: a Flasher that fails them is not handed to anyone.
& $py -m unittest discover -s (Join-Path $root "tools/tests") -p "test_flasher_*.py"
if ($LASTEXITCODE -ne 0) { throw "the Flasher's tests failed" }

New-Item -ItemType Directory -Force $out | Out-Null
$icon = Join-Path $out "roboflash.ico"
& $py (Join-Path $here "make_icon.py") (Join-Path $here "roboflash/web/logo.png") $icon
if ($LASTEXITCODE -ne 0) { throw "icon failed" }

$sep = ";"
$piArgs = @(
    "-m", "PyInstaller", "--noconfirm", "--clean", "--onefile", "--windowed",
    "--name", "RoboAlarms Flasher",
    "--icon", $icon,
    "--distpath", (Join-Path $out "dist"),
    "--workpath", (Join-Path $out "work"),
    "--specpath", $out,
    "--paths", $here,
    "--add-data", ((Join-Path $here "roboflash/web") + $sep + "roboflash/web"),
    "--add-data", ((Join-Path $here "roboflash/boards.json") + $sep + "roboflash"),
    "--collect-all", "esptool",
    "--collect-all", "esp_pylib",
    "--hidden-import", "serial.tools.list_ports"
)
$piArgs += (Join-Path $here "run.py")
& $py @piArgs
if ($LASTEXITCODE -ne 0) { throw "PyInstaller failed" }
$exe = Join-Path $out "dist/RoboAlarms Flasher.exe"
Write-Host ""
Write-Host "Built: $exe ($([math]::Round((Get-Item $exe).Length / 1MB, 1)) MB)"
