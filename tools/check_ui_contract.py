#!/usr/bin/env python3
"""check_ui_contract.py - every screen layout offers what the UI code uses (docs/features/19 CYD-007).

components/ui/alarm_ui is the one copy of the UI's behaviour; each screen size is its own LVGL
Pro project under components/ui/alarm_ui/layouts/<W>x<H>/alarm_ui (ADR-036). The C code reaches a layout only through what
the generated code declares and the objects it looks up by name, so every layout has to offer:

  - the same subjects, with the same types (the UI code writes them);
  - the same components, each with the same API props in the same order and of the same types
    (the generated <name>_create() functions the UI code calls);
  - the same hand-written part (alarm_ui.c / alarm_ui.h: one copy of the callbacks), and every
    event_cb callback its XML names is one alarm_ui.c defines;
  - the constants, fonts and global styles the UI code or the PC programs name (SPACE_SM,
    font_label, style_segment_active...);
  - every object name the UI code or the PC programs look up by a string ("pad_hint", ...);
  - a display the size its folder says.

Everything else - composition, sizes, fonts sizes, which constants exist - is the layout's own.

A layout whose board is on hold (ON_HOLD below) is left out unless --all names it back in: its
board's work is blocked, so the UI code may move on without it for a while. Take the entry out
when that board's work resumes, and bring the layout up to date then.

    python tools/check_ui_contract.py              # check every layout not on hold (exit 1 on a problem)
    python tools/check_ui_contract.py --all        # ... and the ones on hold
    python tools/check_ui_contract.py --layout 1024x600   # only these (repeatable)
    python tools/check_ui_contract.py --list       # what the contract holds today
"""
import argparse
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Where the UI code and the PC programs that drive it live, relative to the repository.
CODE_GLOBS = [
    "components/ui/alarm_ui/src/*.c",
    "components/ui/alarm_ui/src/*.h",
    "components/ui/alarm_ui/include/*.h",
    "host/ui_preview/*.c",
    "host/ui_preview/*.h",
    "host/ui_sim/*.c",
]
HAND_WRITTEN = ["alarm_ui.c", "alarm_ui.h"]
# Layouts left out of the check while their board's work is blocked (see the top).
ON_HOLD = {
    "480x320": "the 4\" E32R40T is on hold (docs/features/19; decided 2026-09-24)",
}
LAYOUT_DIR = re.compile(r"^(\d+)x(\d+)$")
CALLBACK_DEF = re.compile(r"^void\s+(ui_on_[a-z0-9_]+)\s*\(\s*lv_event_t\s*\*", re.M)
COMMENTS = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
STRING = re.compile(r'"([a-z][a-z0-9_]*)"')
IDENT = re.compile(r"\b[A-Za-z_][A-Za-z0-9_]*\b")


class Layout:
    """What one LVGL Pro project declares."""

    def __init__(self, path):
        self.path = path
        self.name = path.parent.name  # components/ui/alarm_ui/layouts/<WxH>/alarm_ui -> <WxH>
        self.subjects = {}            # name -> type (int, string, color...)
        self.consts = set()           # as the generated code names them (SPACE_SM)
        self.fonts = set()
        self.styles = set()
        self.components = {}          # name -> [(prop, type), ...] in API order
        self.objects = set()          # names given to objects in screens and components
        self.callbacks = set()        # event_cb callbacks the XML names
        self.display = None           # (width, height) from project.xml
        self.errors = []
        self._read()

    def _parse(self, file):
        try:
            return ET.parse(file).getroot()
        except (ET.ParseError, OSError) as exc:
            self.errors.append("%s: %s" % (file.relative_to(self.path), exc))
            return None

    def _view(self, root):
        view = root.find("view")
        if view is None:
            return
        for el in view.iter():
            if el.get("name"):
                self.objects.add(el.get("name"))
            if el.tag == "event_cb" and el.get("callback"):
                self.callbacks.add(el.get("callback"))

    def _read(self):
        project = self._parse(self.path / "project.xml") if (self.path / "project.xml").is_file() else None
        if project is not None:
            disp = project.find("./targets/target/display")
            if disp is not None:
                self.display = (int(disp.get("width", "0")), int(disp.get("height", "0")))
        glob = self._parse(self.path / "globals.xml") if (self.path / "globals.xml").is_file() else None
        if glob is None:
            self.errors.append("no readable globals.xml")
        else:
            for el in glob.findall("./subjects/*"):
                self.subjects[el.get("name")] = el.tag
            for el in glob.findall("./consts/*"):
                self.consts.add(el.get("name", "").upper())
            for el in glob.findall("./fonts/*"):
                self.fonts.add(el.get("name"))
            for el in glob.findall("./styles/style"):
                self.styles.add(el.get("name"))
        for file in sorted((self.path / "components").rglob("*.xml")):
            root = self._parse(file)
            if root is None:
                continue
            self.components[file.stem] = [(p.get("name"), p.get("type"))
                                          for p in root.findall("./api/prop")]
            for el in root.findall("./styles/style"):
                self.styles.add(el.get("name"))
            self._view(root)
        for file in sorted((self.path / "screens").glob("*.xml")):
            root = self._parse(file)
            if root is not None:
                self._view(root)

    def defined_callbacks(self):
        src = self.path / "alarm_ui.c"
        return set(CALLBACK_DEF.findall(src.read_text(encoding="utf-8"))) if src.is_file() else set()


def layouts(root):
    ui = root / "components" / "ui" / "alarm_ui" / "layouts"
    if not ui.is_dir():
        return []
    return [Layout(d / "alarm_ui") for d in sorted(ui.iterdir())
            if LAYOUT_DIR.match(d.name) and (d / "alarm_ui").is_dir()]


def chosen(root, only=None, on_hold=None, include_on_hold=False):
    """(the layouts to check, [(name, why) left out]): every layout, or the ones in `only`, less
    the ones on hold unless include_on_hold (or `only` names them)."""
    on_hold = ON_HOLD if on_hold is None else on_hold
    found, skipped = [], []
    for lay in layouts(root):
        if only is not None and lay.name not in only:
            continue
        if lay.name in on_hold and not include_on_hold and only is None:
            skipped.append((lay.name, on_hold[lay.name]))
            continue
        found.append(lay)
    return found, skipped


def code_uses(root):
    """(identifiers, string literals) in the UI code and the PC programs, comments left out."""
    idents, strings = set(), set()
    for pattern in CODE_GLOBS:
        for file in sorted(root.glob(pattern)):
            text = COMMENTS.sub(" ", file.read_text(encoding="utf-8", errors="replace"))
            strings.update(STRING.findall(text))
            idents.update(IDENT.findall(STRING.sub('""', text)))
    return idents, strings


def contract(found, root):
    """What every layout has to offer: derived from the layouts and the code that uses them."""
    idents, strings = code_uses(root)
    all_globals = set()
    all_objects = set()
    for lay in found:
        all_globals |= lay.consts | lay.fonts | lay.styles
        all_objects |= lay.objects
    return {
        "globals": sorted(all_globals & idents),
        "objects": sorted(all_objects & strings),
    }


def check(root, out=sys.stdout, only=None, on_hold=None, include_on_hold=False):
    found, skipped = chosen(root, only, on_hold, include_on_hold)
    problems = []
    for name, why in skipped:
        print("SKIPPED  %s: %s (--all checks it)" % (name, why), file=out)
    if only is not None:
        problems.extend("no layout %s under ui/" % name
                        for name in sorted(set(only) - {lay.name for lay in found}))
    if not found:
        problems.append("no layouts under components/ui/alarm_ui/layouts/<W>x<H>/alarm_ui")
    for lay in found:
        problems.extend("%s: %s" % (lay.name, e) for e in lay.errors)
        m = LAYOUT_DIR.match(lay.name)
        if lay.display != (int(m.group(1)), int(m.group(2))):
            problems.append("%s: project.xml's display is %s, not the folder's size"
                            % (lay.name, "x".join(map(str, lay.display)) if lay.display else "missing"))
        for cb in sorted(lay.callbacks - lay.defined_callbacks()):
            problems.append("%s: callback %s is named in the XML but not defined in alarm_ui.c"
                            % (lay.name, cb))
    if found:
        ref = found[0]
        for lay in found[1:]:
            pair = "%s vs %s" % (lay.name, ref.name)
            for name in sorted(set(ref.subjects) | set(lay.subjects)):
                a, b = ref.subjects.get(name), lay.subjects.get(name)
                if a is None or b is None:
                    problems.append("%s: subject %s exists only in %s"
                                    % (pair, name, ref.name if b is None else lay.name))
                elif a != b:
                    problems.append("%s: subject %s is %s in one and %s in the other"
                                    % (pair, name, a, b))
            for name in sorted(set(ref.components) | set(lay.components)):
                a, b = ref.components.get(name), lay.components.get(name)
                if a is None or b is None:
                    problems.append("%s: component %s exists only in %s"
                                    % (pair, name, ref.name if b is None else lay.name))
                elif a != b:
                    problems.append("%s: component %s's API differs: %s / %s"
                                    % (pair, name, ", ".join("%s:%s" % p for p in a),
                                       ", ".join("%s:%s" % p for p in b)))
            for file in HAND_WRITTEN:
                fa, fb = ref.path / file, lay.path / file
                if not fa.is_file() or not fb.is_file():
                    problems.append("%s: %s is missing" % (pair, file))
                elif (fa.read_bytes().replace(b"\r\n", b"\n")
                      != fb.read_bytes().replace(b"\r\n", b"\n")):
                    problems.append("%s: %s differs (the hand-written part is one copy)"
                                    % (pair, file))
        # What the code needs is read from every layout, the ones left out included, so a
        # layout checked alone still has to offer the names the others give the code.
        need = contract(layouts(root), root)
        for lay in found:
            have = lay.consts | lay.fonts | lay.styles
            for name in need["globals"]:
                if name not in have:
                    problems.append("%s: the code uses %s, which this layout doesn't define"
                                    % (lay.name, name))
            for name in need["objects"]:
                if name not in lay.objects:
                    problems.append("%s: the code looks up an object named \"%s\", which this "
                                    "layout doesn't have" % (lay.name, name))
    for p in problems:
        print("PROBLEM  " + p, file=out)
    if problems:
        print("FAIL: %d problem(s) in %d layout(s)" % (len(problems), len(found)), file=out)
        return 1
    print("OK: %d layout(s) keep the UI contract (%s)"
          % (len(found), ", ".join(lay.name for lay in found)), file=out)
    return 0


def listing(root, out=sys.stdout, only=None, on_hold=None, include_on_hold=False):
    found, _ = chosen(root, only, on_hold, include_on_hold)
    if not found:
        print("no layouts", file=out)
        return 1
    need = contract(layouts(root), root)
    ref = found[0]
    print("Layouts: %s" % ", ".join(lay.name for lay in found), file=out)
    print("Subjects (%d): %s" % (len(ref.subjects), ", ".join(sorted(ref.subjects))), file=out)
    print("Components (%d): %s" % (len(ref.components), ", ".join(sorted(ref.components))),
          file=out)
    print("Globals the code names (%d): %s" % (len(need["globals"]), ", ".join(need["globals"])),
          file=out)
    print("Objects the code looks up (%d): %s" % (len(need["objects"]), ", ".join(need["objects"])),
          file=out)
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--root", type=Path, default=ROOT, help=argparse.SUPPRESS)
    parser.add_argument("--list", action="store_true", help="print what the contract holds")
    parser.add_argument("--all", action="store_true",
                        help="also check the layouts on hold (%s)" % ", ".join(sorted(ON_HOLD)))
    parser.add_argument("--layout", action="append", metavar="WxH",
                        help="check only this layout (repeatable)")
    args = parser.parse_args(argv)
    only = set(args.layout) if args.layout else None
    if args.list:
        return listing(args.root, only=only, include_on_hold=args.all)
    return check(args.root, only=only, include_on_hold=args.all)


if __name__ == "__main__":
    sys.exit(main())
