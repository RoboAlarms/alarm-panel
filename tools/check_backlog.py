#!/usr/bin/env python3
"""check_backlog.py - docs/BACKLOG.md stays the one list of open work.

The backlog lists every piece of work not yet done; the feature documents keep the details and
the step-by-step boxes. This checks that the two agree:

  - every row of docs/BACKLOG.md has a unique BL-NNN number and the cells its table has;
  - every relative link in the backlog leads to a file that exists;
  - every open box ("- [ ]") in docs/features carries a BL-NNN tag, on its own text or on a
    heading above it, and that row is in the backlog (a finished row is deleted only once its
    boxes are ticked);
  - no "BL-NEW" placeholder is left, and every BL-NNN a row mentions (a decision it waits on)
    is a row.

    python tools/check_backlog.py           # check (exit 1 on a problem)
    python tools/check_backlog.py --list    # the open boxes each row covers
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

ROW = re.compile(r"^\|\s*(BL-\d{3})\s*\|")
TAG = re.compile(r"\bBL-\d{3}\b")
BOX = re.compile(r"^(\s*)[-*] \[ \] ")
ITEM = re.compile(r"^\s*([-*]|\d+\.) ")
HEADING = re.compile(r"^(#{1,6}) +(.*)")
LINK = re.compile(r"\]\(([^)\s]+)\)")
SKIP = {"_TEMPLATE.md"}


def backlog_rows(text):
    """The rows' numbers, in order, and problems with the tables' shape."""
    rows, problems, cells = [], [], None
    for n, line in enumerate(text.splitlines(), 1):
        if not line.startswith("|"):
            cells = None
            continue
        count = line.count("|") - line.count("\\|")
        if cells is None:
            cells = count
        elif count != cells:
            problems.append(f"BACKLOG.md:{n}: {count - 1} cells in a table of {cells - 1}")
        m = ROW.match(line)
        if m:
            rows.append((m.group(1), n, line))
    return rows, problems


def open_boxes(path):
    """Each open box as (line number, its text with continuation lines, headings above it)."""
    lines = path.read_text(encoding="utf-8").splitlines()
    headings, boxes, fence = [], [], False
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.lstrip().startswith("```"):
            fence = not fence
        elif not fence:
            h = HEADING.match(line)
            if h:
                level = len(h.group(1))
                headings = [x for x in headings if x[0] < level] + [(level, h.group(2))]
            m = BOX.match(line)
            if m:
                indent = len(m.group(1))
                text = [line]
                j = i + 1
                while j < len(lines):
                    nxt = lines[j]
                    if (not nxt.strip() or HEADING.match(nxt) or nxt.startswith("|")
                            or (ITEM.match(nxt) and len(nxt) - len(nxt.lstrip()) <= indent)):
                        break
                    text.append(nxt)
                    j += 1
                boxes.append((i + 1, " ".join(t.strip() for t in text), [x[1] for x in headings]))
                i = j
                continue
        i += 1
    return boxes


def check(root, listing=None):
    problems = []
    backlog = root / "docs" / "BACKLOG.md"
    if not backlog.exists():
        return ["docs/BACKLOG.md is missing"]
    text = backlog.read_text(encoding="utf-8")
    rows, shape = backlog_rows(text)
    problems += shape
    ids = [r[0] for r in rows]
    for rid in sorted({x for x in ids if ids.count(x) > 1}):
        problems.append(f"BACKLOG.md: {rid} is used by more than one row")
    known = set(ids)
    for rid, n, line in rows:
        for ref in TAG.findall(line):
            if ref not in known:
                problems.append(f"BACKLOG.md:{n}: {rid} mentions {ref}, which is not a row")
    for n, line in enumerate(text.splitlines(), 1):
        if "BL-NEW" in line:
            problems.append(f"BACKLOG.md:{n}: a BL-NEW placeholder is left")
        for target in LINK.findall(line):
            if re.match(r"[a-z]+:", target) or target.startswith("#"):
                continue
            if not (backlog.parent / target.split("#")[0]).exists():
                problems.append(f"BACKLOG.md:{n}: the link {target} leads nowhere")

    features = root / "docs" / "features"
    for path in sorted(features.rglob("*.md")):
        if path.name in SKIP:
            continue
        rel = path.relative_to(root).as_posix()
        for n, box, headings in open_boxes(path):
            if "BL-NEW" in box or any("BL-NEW" in h for h in headings):
                problems.append(f"{rel}:{n}: a BL-NEW placeholder is left")
                continue
            tags = TAG.findall(box) or [t for h in headings for t in TAG.findall(h)]
            if not tags:
                problems.append(f"{rel}:{n}: open box with no backlog row (tag it BL-NNN): "
                                f"{box.strip()[:90]}")
                continue
            for tag in tags:
                if tag not in known:
                    problems.append(f"{rel}:{n}: {tag} is not in docs/BACKLOG.md")
                elif listing is not None:
                    listing.setdefault(tag, []).append(f"{rel}:{n}")
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--list", action="store_true", help="print the open boxes each row covers")
    args = ap.parse_args()
    listing = {} if args.list else None
    problems = check(ROOT, listing)
    if listing is not None:
        for tag in sorted(listing):
            print(f"{tag}: {', '.join(listing[tag])}")
    for p in problems:
        print(p)
    if problems:
        print(f"{len(problems)} problem(s): see docs/BACKLOG.md 'Keeping it current'")
        return 1
    print("OK: every open box in docs/features has its backlog row")
    return 0


if __name__ == "__main__":
    sys.exit(main())
