#!/usr/bin/env python3
"""check_boards.py - every board's manifest is complete and consistent (docs/features/33 step 9).

A board's manifest is its firmware project's sdkconfig.defaults (firmware/<board>/): it states
every feature option explicitly, so what the board carries can be read from one file, by its
firmware build and by its PC simulator profile alike (cmake/alarm_board.cmake reads the same
lines). A board built on another board's project (set(ALARM_BOARD_BASE <board>) in its
CMakeLists.txt, cmake/alarm_firmware.cmake) has the base's manifest followed by its own lines,
read in that order by the build and by both readers. This checks, for every firmware/<board>/ that sets
ALARM_BOARD (a radio or co-processor project such as firmware/c6_cp is not a board):

  - every option in MANIFEST is stated (set, or "# CONFIG_X is not set");
  - the UI layout it names exists (components/ui/alarm_ui/layouts/<WxH>/alarm_ui) when the board has a UI;
  - features that need PSRAM are only on boards whose manifest turns PSRAM on;
  - a board with storage links at least one SD driver; Devices need the Home Assistant link;
  - capacities are in range;
  - its main requires its own board component (components/boards/board_<board>) and no other;
  - its base project exists, and is built on no other (one level);
  - a family library its board component requires (components/boards/<name>_family) is the
    one its CMakeLists.txt names in ALARM_BOARD_FAMILY, and that one exists.

    python tools/check_boards.py            # check (exit 1 on a problem)
    python tools/check_boards.py --table    # what each board carries, as a Markdown table
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Every option a manifest must state, in the order the table prints them.
MANIFEST = [
    "IDF_TARGET",
    "ALARM_UI_LAYOUT",
    "ALARM_MAX_ZONES",
    "ALARM_MAX_USERS",
    "ALARM_FEATURE_UI",
    "ALARM_FEATURE_MQTT",
    "ALARM_FEATURE_NOTIFY",
    "ALARM_FEATURE_HA",
    "ALARM_PANEL_WEB",
    "ALARM_DEV_REMOTE",
    "ALARM_FEATURE_STORAGE",
    "ALARM_FEATURE_PHOTO_SAVER",
    "ALARM_FEATURE_RTC",
    "ALARM_HOME_CONTROL",
    "ALARM_FEATURE_AUTOMATIONS",
    "ALARM_LINK",
    "ALARM_ZSRC_RF",
    "ALARM_ZSRC_ZIGBEE",
    "ALARM_ZSRC_ZWAVE",
    "ALARM_ZWAVE_S0",
    "ALARM_ZWAVE_LR",
    "PANEL_STORAGE_SDMMC",
    "PANEL_STORAGE_SDSPI",
]
# Features whose memory only fits beside PSRAM (components/services/alarm_features/Kconfig;
# Z-Wave's network: components/services/zwave_service/Kconfig).
NEEDS_PSRAM = ["ALARM_FEATURE_HA", "ALARM_PANEL_WEB", "ALARM_DEV_REMOTE",
               "ALARM_FEATURE_PHOTO_SAVER", "ALARM_HOME_CONTROL", "ALARM_ZSRC_ZWAVE",
               "ALARM_LINK"]
# Options that only exist inside another one (Kconfig "if"): stated on, they need it on.
NEEDS = [("ALARM_HOME_CONTROL", "ALARM_FEATURE_HA"),
         ("ALARM_FEATURE_AUTOMATIONS", "ALARM_HOME_CONTROL"),
         ("ALARM_ZWAVE_S0", "ALARM_ZSRC_ZWAVE"),
         ("ALARM_ZWAVE_LR", "ALARM_ZSRC_ZWAVE")]

SET = re.compile(r"^CONFIG_([A-Z0-9_]+)=(.*)$")
BOARD = re.compile(r"^\s*set\(ALARM_BOARD\s+([a-z0-9_]+)\s*\)", re.M)
BASE = re.compile(r"^\s*set\(ALARM_BOARD_BASE\s+([a-z0-9_]+)\s*\)", re.M)
FAMILY = re.compile(r"^\s*set\(ALARM_BOARD_FAMILY\s+([a-z0-9_]+)\s*\)", re.M)
FAMILY_REQUIRED = re.compile(r"\b([a-z0-9_]+_family)\b")
UNSET = re.compile(r"^#\s*CONFIG_([A-Z0-9_]+) is not set\s*$")
LAYOUT = re.compile(r"^[0-9]+x[0-9]+$")


def parse_defaults(text):
    """{option: value} from sdkconfig lines; "n" for "# CONFIG_X is not set", quotes dropped.
    The last line for an option wins, as in Kconfig."""
    values = {}
    for raw in text.splitlines():
        line = raw.strip()
        m = SET.match(line)
        if m:
            value = m.group(2).strip()
            if len(value) >= 2 and value[0] == value[-1] == '"':
                value = value[1:-1]
            values[m.group(1)] = value
            continue
        m = UNSET.match(line)
        if m:
            values[m.group(1)] = "n"
    return values


def is_on(values, option):
    return values.get(option) == "y"


def project_setting(board_dir, pattern):
    """A set(ALARM_BOARD_BASE|FAMILY <name>) of firmware/<board>/CMakeLists.txt, or None."""
    cmake = board_dir / "CMakeLists.txt"
    if not cmake.is_file():
        return None
    m = pattern.search(cmake.read_text(encoding="utf-8"))
    return m.group(1) if m else None


def manifest_files(board_dir):
    """The sdkconfig.defaults files that make the board's manifest, base first."""
    base = project_setting(board_dir, BASE)
    files = [board_dir.parent / base / "sdkconfig.defaults"] if base else []
    return files + [board_dir / "sdkconfig.defaults"]


def read_manifest(board_dir):
    """The board's manifest: its base's lines, then its own (the last line wins)."""
    return parse_defaults("\n".join(f.read_text(encoding="utf-8")
                                    for f in manifest_files(board_dir) if f.is_file()))


def boards(root):
    """The panel boards: firmware/<board>/ projects that set(ALARM_BOARD <board>). A radio or
    co-processor project (firmware/c6_cp, firmware/radios/h2_rcp) is not a panel board."""
    fw = root / "firmware"
    if not fw.is_dir():
        return []
    return sorted(p for p in fw.iterdir()
                  if (p / "sdkconfig.defaults").is_file() and project_setting(p, BOARD))


def check_board(root, board_dir):
    """Problems with one board's manifest, as strings."""
    name = board_dir.name
    values = read_manifest(board_dir)
    problems = []

    def problem(text):
        problems.append("%s: %s" % (name, text))

    base = project_setting(board_dir, BASE)
    if base is not None:
        base_dir = board_dir.parent / base
        if not (base_dir / "sdkconfig.defaults").is_file():
            problem("its base firmware/%s does not exist" % base)
        elif project_setting(base_dir, BASE) is not None:
            problem("its base firmware/%s is built on another project (one level only)" % base)

    for option in MANIFEST:
        if option not in values:
            problem("CONFIG_%s is not stated in sdkconfig.defaults" % option)

    layout = values.get("ALARM_UI_LAYOUT", "")
    if is_on(values, "ALARM_FEATURE_UI"):
        if not LAYOUT.match(layout):
            problem("CONFIG_ALARM_UI_LAYOUT=%r is not <width>x<height>" % layout)
        elif not (root / "components" / "ui" / "alarm_ui" / "layouts" / layout / "alarm_ui"
                  / "file_list_gen.cmake").is_file():
            problem("the UI layout components/ui/alarm_ui/layouts/%s/alarm_ui does not exist"
                    % layout)

    if not is_on(values, "SPIRAM"):
        for option in NEEDS_PSRAM:
            if is_on(values, option):
                problem("CONFIG_%s needs PSRAM, which this board's manifest doesn't turn on"
                        % option)
    for option, needed in NEEDS:
        if is_on(values, option) and not is_on(values, needed):
            problem("CONFIG_%s needs CONFIG_%s" % (option, needed))
    if is_on(values, "ALARM_FEATURE_STORAGE") and not (
            is_on(values, "PANEL_STORAGE_SDMMC") or is_on(values, "PANEL_STORAGE_SDSPI")):
        problem("CONFIG_ALARM_FEATURE_STORAGE without an SD driver "
                "(CONFIG_PANEL_STORAGE_SDMMC or _SDSPI)")

    for option, low, high in (("ALARM_MAX_ZONES", 1, 999), ("ALARM_MAX_USERS", 2, 99)):
        if option in values:
            try:
                number = int(values[option])
            except ValueError:
                number = None
            if number is None or not low <= number <= high:
                problem("CONFIG_%s=%s is outside %d..%d" % (option, values[option], low, high))

    own = "board_" + name
    if not (root / "components" / "boards" / own).is_dir():
        problem("no board component components/boards/%s" % own)
    family = project_setting(board_dir, FAMILY)
    own_cmake = root / "components" / "boards" / own / "CMakeLists.txt"
    required = set()
    if own_cmake.is_file():
        code = "\n".join(line.split("#", 1)[0]
                         for line in own_cmake.read_text(encoding="utf-8").splitlines())
        required = set(FAMILY_REQUIRED.findall(code))
    if family is not None and not (root / "components" / "boards" / family).is_dir():
        problem("no family library components/boards/%s" % family)
    for other in sorted(required - {family}):
        problem("%s requires %s, which CMakeLists.txt doesn't name in ALARM_BOARD_FAMILY"
                % (own, other))
    if family is not None and family not in required:
        problem("ALARM_BOARD_FAMILY %s, which %s doesn't require" % (family, own))
    main_cmake = board_dir / "main" / "CMakeLists.txt"
    if main_cmake.is_file():
        text = main_cmake.read_text(encoding="utf-8")
        named = set(re.findall(r"\bboard_[a-z0-9_]+\b", text)) - {"board_api", "board_info"}
        if own not in named:
            problem("main/CMakeLists.txt doesn't require its board component %s" % own)
        for other in sorted(named - {own}):
            problem("main/CMakeLists.txt names another board's component %s" % other)
    else:
        problem("no main/CMakeLists.txt")
    return problems


def run(root, out=sys.stdout):
    found = boards(root)
    problems = []
    for board_dir in found:
        problems.extend(check_board(root, board_dir))
    for p in problems:
        print("PROBLEM  " + p, file=out)
    if problems:
        print("FAIL: %d problem(s) in %d board manifest(s)" % (len(problems), len(found)), file=out)
        return 1
    print("OK: %d board manifest(s) complete and consistent (%s)"
          % (len(found), ", ".join(b.name for b in found)), file=out)
    return 0


def table(root, out=sys.stdout):
    found = boards(root)
    manifests = [(b.name, read_manifest(b)) for b in found]
    print("| Option | " + " | ".join(n for n, _ in manifests) + " |", file=out)
    print("|---|" + "---|" * len(manifests), file=out)
    for option in MANIFEST:
        print("| `%s` | " % option + " | ".join(v.get(option, "?") for _, v in manifests) + " |",
              file=out)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--root", type=Path, default=ROOT, help=argparse.SUPPRESS)
    parser.add_argument("--table", action="store_true", help="print what each board carries")
    args = parser.parse_args(argv)
    if args.table:
        table(args.root)
        return 0
    return run(args.root)


if __name__ == "__main__":
    sys.exit(main())
