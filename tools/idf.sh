#!/usr/bin/env bash
# Run idf.py for a firmware project with ESP-IDF v6.1 activated (Linux / macOS).
#
# The POSIX counterpart of tools/idf.ps1: same options, same build folders, same sdkconfig
# files, so the two produce the same image. Keep them in step.
#
# Images are signed (docs/features/13). The signing key is secrets/ota_signing_key.pem (never
# committed); it is created on the first build when missing. Keep a copy somewhere safe: panels
# only accept updates signed with the key of the firmware they run, so a lost key means
# reflashing every panel over USB.
#
#   tools/idf.sh build                        # crowpanel_p4, P4 silicon v1.x (default)
#   tools/idf.sh --chip-rev v3 build          # P4 silicon v3.x build
#   tools/idf.sh -p /dev/ttyACM0 flash monitor
#   tools/idf.sh --version 0.6.1-test build   # test image (build_v1_0.6.1-test)
#   tools/idf.sh --project e32r40t build      # another board (build, build_dev, ...)
#   tools/idf.sh --project crowpanel_10 build  # the 10.1in, built on crowpanel_p4
#   tools/idf.sh --project radios/h2_rcp build  # the ESP32-H2's own firmware
#
# Build folders: crowpanel_p4 and the boards built on it (ALARM_BOARD_BASE crowpanel_p4 in their
# CMakeLists.txt) build one image per P4 silicon family (build_v1, build_v3 and
# sdkconfig.v1/.v3); every other board has one (build, sdkconfig). A version, profile or dark
# build adds its suffix to either: build_v1_dev, build_dev, build_0.6.1-test. A board built on
# another project reads each sdkconfig.defaults fragment from it first (cmake/alarm_firmware.cmake).
#
# ESP-IDF is looked for in this order: an already-activated environment, $IDF_PATH,
# $IDF_EIM_JSON or an EIM installation, then the usual manual install paths. Set IDF_PATH or
# IDF_EIM_JSON if yours is somewhere else.
set -euo pipefail

project=crowpanel_p4
port=
chip_rev=v1
chip_rev_given=0
version=
profile=legacy
dark=0
idf_args=()

die() { printf 'idf.sh: %s\n' "$*" >&2; exit 1; }
note() { printf '%s\n' "$*" >&2; }

while [ $# -gt 0 ]; do
    case "$1" in
        --project) project=${2:?--project needs a value}; shift 2 ;;
        --project=*) project=${1#*=}; shift ;;
        -p|--port) port=${2:?--port needs a value}; shift 2 ;;
        -p=*|--port=*) port=${1#*=}; shift ;;
        --chip-rev) chip_rev=${2:?--chip-rev needs a value}; chip_rev_given=1; shift 2 ;;
        --chip-rev=*) chip_rev=${1#*=}; chip_rev_given=1; shift ;;
        --version) version=${2:?--version needs a value}; shift 2 ;;
        --version=*) version=${1#*=}; shift ;;
        --profile) profile=${2:?--profile needs a value}; shift 2 ;;
        --profile=*) profile=${1#*=}; shift ;;
        --dark) dark=1; shift ;;
        -h|--help) sed -n '2,28p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        --) shift; idf_args+=("$@"); break ;;
        *) idf_args+=("$1"); shift ;;
    esac
done

case "$chip_rev" in
    v1|v3) ;;
    *) die "--chip-rev must be v1 or v3 (got '$chip_rev')" ;;
esac
if [ -n "$version" ] && ! printf '%s' "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+([-+][0-9A-Za-z.+-]+)?$'; then
    die "--version must look like 1.2.3 or 1.2.3-test (got '$version')"
fi

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
proj_dir="$root/firmware/$project"
[ -d "$proj_dir" ] || die "no firmware project at $proj_dir"

# ---- find and activate ESP-IDF -------------------------------------------------------------
# ESP-IDF brings its own Python, but the JSON below is read before it is activated.
find_python() {
    local py
    for py in "${PYTHON:-}" python3 python; do
        [ -n "$py" ] || continue
        if command -v "$py" >/dev/null 2>&1 && "$py" -c '' >/dev/null 2>&1; then
            printf '%s' "$py"
            return 0
        fi
    done
    return 1
}

# EIM writes eim_idf.json listing each installation and its activation script.
# Returns 2 (not 1) when it is the JSON that has no v6.1, so the caller can say which it is.
eim_activation_script() {
    local json=$1 py
    py=$(find_python) || return 1
    "$py" - "$json" <<'PY'
import json, sys
try:
    with open(sys.argv[1]) as f:
        data = json.load(f)
except Exception as exc:
    sys.stderr.write("cannot read it: %s\n" % exc)
    sys.exit(2)
installs = data.get("idfInstalled") or []
for i in installs:
    if str(i.get("name", "")).lstrip("v").startswith("6.1"):
        script = i.get("activationScript") or ""
        if script:
            print(script)
            sys.exit(0)
sys.stderr.write("it lists: %s\n" % (", ".join(str(i.get("name")) for i in installs) or "nothing"))
sys.exit(2)
PY
}

activate_idf() {
    if command -v idf.py >/dev/null 2>&1; then
        return 0                      # already activated (you ran export.sh)
    fi
    local script=
    if [ -n "${IDF_PATH:-}" ] && [ -f "$IDF_PATH/export.sh" ]; then
        script=$IDF_PATH/export.sh
    else
        local json candidates=()
        [ -n "${IDF_EIM_JSON:-}" ] && candidates+=("$IDF_EIM_JSON")
        candidates+=(
            "$HOME/.espressif/tools/eim_idf.json"
            "$HOME/.espressif/eim_idf.json"
            "$HOME/Library/Application Support/espressif/tools/eim_idf.json"
            "/opt/esp/tools/eim_idf.json"
        )
        local rc
        for json in "${candidates[@]}"; do
            [ -f "$json" ] || continue
            script=$(eim_activation_script "$json") && break
            rc=$?
            script=
            if [ "$rc" = 1 ]; then
                note "idf.sh: no usable Python to read $json (tried \$PYTHON, python3, python)"
            else
                note "idf.sh: $json has no ESP-IDF v6.1 installation"
            fi
        done
        if [ -z "$script" ]; then
            for candidate in \
                "$HOME/esp/v6.1/esp-idf/export.sh" \
                "$HOME/esp/esp-idf/export.sh" \
                "/opt/esp/idf/export.sh"
            do
                [ -f "$candidate" ] && { script=$candidate; break; }
            done
        fi
    fi
    [ -n "$script" ] || die "ESP-IDF not found. Install v6.1 (EIM or the manual install), or set
       IDF_PATH=/path/to/esp-idf   (then this script sources its export.sh)
       IDF_EIM_JSON=/path/to/eim_idf.json
     ...or activate it yourself first:  . \$IDF_PATH/export.sh"
    [ -f "$script" ] || die "ESP-IDF activation script not found: $script"
    # shellcheck disable=SC1090
    . "$script" >/dev/null
    command -v idf.py >/dev/null 2>&1 || die "sourcing $script did not put idf.py on PATH"
}

activate_idf

# The firmware pins ESP-IDF v6.1; other versions may configure or build differently.
idf_version=$(idf.py --version 2>/dev/null | head -n 1 || true)
case "$idf_version" in
    *v6.1*) ;;
    *) note "idf.sh: warning: this project is built with ESP-IDF v6.1, found: ${idf_version:-unknown}" ;;
esac

# ---- the OTA signing key (sdkconfig.defaults: CONFIG_SECURE_BOOT_SIGNING_KEY) ---------------
key_file="$root/secrets/ota_signing_key.pem"
if [ ! -f "$key_file" ]; then
    mkdir -p "$(dirname -- "$key_file")"
    note "Creating the firmware signing key $key_file (keep a backup of it)"
    if command -v espsecure >/dev/null 2>&1; then
        espsecure generate-signing-key --version 2 --scheme rsa3072 "$key_file"
    elif command -v espsecure.py >/dev/null 2>&1; then
        espsecure.py generate-signing-key --version 2 --scheme rsa3072 "$key_file"
    else
        die "espsecure not found, although ESP-IDF is active - cannot create the signing key"
    fi
    [ -s "$key_file" ] || die "creating the signing key failed"
fi

# ---- build ---------------------------------------------------------------------------------
# A board built on another firmware project (set(ALARM_BOARD_BASE <board>) in its CMakeLists.txt)
# takes its fragments from there too, and its silicon families.
base=$(sed -n -E 's/^[[:space:]]*set\(ALARM_BOARD_BASE[[:space:]]+([a-z0-9_]+)[[:space:]]*\)[[:space:]]*$/\1/p' \
    "$proj_dir/CMakeLists.txt" | head -n 1)
base_dir=
if [ -n "$base" ]; then
    base_dir="$root/firmware/$base"
    [ -d "$base_dir" ] || die "firmware/$project is built on firmware/$base, which is missing"
fi

# Each P4 silicon family needs its own image (ESP-IDF 6.x can't build one for both); the other
# boards have one.
defaults=sdkconfig.defaults
if [ "$project" = crowpanel_p4 ] || [ "$base" = crowpanel_p4 ]; then
    [ "$chip_rev" = v1 ] && defaults="$defaults;sdkconfig.defaults.p4rev1"
    build_dir="build_$chip_rev"
    sdkconfig="sdkconfig.$chip_rev"
else
    [ "$chip_rev_given" = 0 ] || die "--chip-rev is for crowpanel_p4 and the boards built on it only"
    build_dir=build
    sdkconfig=sdkconfig
fi

# A version other than the VERSION file (test images) builds in its own folder.
export ALARM_FW_VERSION=${version:-}
[ -n "$version" ] && build_dir="${build_dir}_${version}"

case "$profile" in legacy|dev|release) ;; *) die "--profile must be legacy, dev or release" ;; esac
if [ "$profile" != legacy ]; then
    defaults="$defaults;sdkconfig.defaults.$profile"
    build_dir="${build_dir}_$profile"
    sdkconfig="$sdkconfig.$profile"
fi
if [ "$dark" = 1 ]; then
    # Dark acceptance retains the signed profile; packaging rejects forced-dark releases.
    defaults="$defaults;sdkconfig.defaults.dark"
    build_dir="${build_dir}_dark"
    sdkconfig="$sdkconfig.dark"
fi
export ALARM_BUILD_PROFILE=$profile
IFS=';' read -r -a fragments <<< "$defaults"
for fragment in "${fragments[@]}"; do
    [ -f "$proj_dir/$fragment" ] || { [ -n "$base_dir" ] && [ -f "$base_dir/$fragment" ]; } ||
        die "firmware/$project has no $fragment"
done

set -- -B "$build_dir" -D "SDKCONFIG=$sdkconfig" -D "SDKCONFIG_DEFAULTS=$defaults"
[ -n "$port" ] && set -- "$@" -p "$port"
[ ${#idf_args[@]} -gt 0 ] && set -- "$@" "${idf_args[@]}"

cd "$proj_dir"
exec idf.py "$@"
