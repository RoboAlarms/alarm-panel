#!/usr/bin/env bash
# Build the RoboAlarms Flasher as one program for macOS or Linux (docs/features/34). The same steps
# as build.ps1 (Windows): change one, change the other.
#
#   tools/flasher/build.sh                       # build/flasher/dist/RoboAlarms Flasher[.app]
#
# No firmware goes inside: the Flasher downloads it from the firmware server (FLS-032).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="$root/build/flasher"
venv="$root/build/flasher-venv"
python="${PYTHON:-python3}"
while [ $# -gt 0 ]; do
    case "$1" in
        --python) python="$2"; shift 2 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done

[ -x "$venv/bin/python" ] || "$python" -m venv "$venv"
py="$venv/bin/python"
"$py" -m pip install --disable-pip-version-check -q -r "$here/requirements.txt" "pyinstaller>=6.10,<7"
"$py" -m unittest discover -s "$root/tools/tests" -p "test_flasher_*.py"

mkdir -p "$out"
args=(-m PyInstaller --noconfirm --clean --onefile --windowed
      --name "RoboAlarms Flasher"
      --distpath "$out/dist" --workpath "$out/work" --specpath "$out"
      --paths "$here"
      --add-data "$here/roboflash/web:roboflash/web"
      --add-data "$here/roboflash/boards.json:roboflash"
      --collect-all esptool --collect-all esp_pylib
      --hidden-import serial.tools.list_ports)
args+=("$here/run.py")
"$py" "${args[@]}"
echo "Built: $out/dist"
