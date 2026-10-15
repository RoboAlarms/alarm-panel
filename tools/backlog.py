#!/usr/bin/env python3
"""backlog.py - small views of docs/BACKLOG.md for people and coding agents.

Cheaper than reading the backlog (~35 KB) into context (AGENTS.md "Starting a session"):

    backlog.py handoff               your status page (HANDOFF.local.md), warned when it grows too long
    backlog.py list [--pc] [--all]   one line per row: id, section, what it needs, title, details
    backlog.py show BL-001 [...]     a row in full, the open boxes it covers (file:line), its links
    backlog.py next-id               the next free BL number (numbers are never reused)
    backlog.py worktrees [--quick]   agent worktrees: idle time, and which of their files main has
    backlog.py worktrees --clean [NAME ...] [--older-than 12] [--dry-run]
                                     keep each one's changes in local-archive/worktree-patches,
                                     then remove it and its branch (idle ones, or those named)
    backlog.py mark BL-066           remember the dirty files before a unit runs in the main tree
    backlog.py changes BL-066 [--diff]   that unit's own edits since its mark (a dirty tree too)

Needs flags in `list`: PC (the PC is enough), PC+ (a PC part first, then a panel, a service or
a reviewer), blocked:BL-NNN (waits on an open row), panel, part, service, maintainers (a decision,
their panels or their listening check first), other; "-" for the sections without a Needs column
(decisions, later, on hold).

On Windows, if plain `python` is only the Store stub, run it with the ESP-IDF venv's python.
The parsing of rows and open boxes is tools/check_backlog.py's own, imported, so the two agree.
"""
import argparse
import re
import subprocess
import sys
import textwrap
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import check_backlog as cb  # noqa: E402

BACKLOG = ROOT / "docs" / "BACKLOG.md"
HANDOFF = ROOT / "HANDOFF.local.md"
ARCHIVE = ROOT / "local-archive"      # old handoff pages, git-ignored (AGENTS.md "Recording work")
HANDOFF_MAX_LINES = 100
SECTION = re.compile(r"^## (\d+)\. (.*)")
SPLIT = re.compile(r"(?<!\\)\|")
LINK = re.compile(r"\[([^\]]+)\]\(([^)\s]+)\)")
BOLD = re.compile(r"\*\*(.+?)\*\*")


def plain(text):
    """Markdown cell text without links' targets and emphasis."""
    return LINK.sub(r"\1", text).replace("**", "").replace("`", "")


def cells(line):
    return [c.strip() for c in SPLIT.split(line.strip().strip("|"))]


def rows():
    """Every row: id, (section number, title), {column: cell}, line number."""
    out, sec, header = [], (0, ""), None
    for n, line in enumerate(BACKLOG.read_text(encoding="utf-8").splitlines(), 1):
        m = SECTION.match(line)
        if m:
            sec, header = (int(m.group(1)), m.group(2).strip()), None
            continue
        if not line.startswith("|"):
            continue
        m = cb.ROW.match(line)
        if not m:
            if header is None and not set(line.replace("|", "").strip()) <= set("-: "):
                header = cells(line)
            continue
        out.append({"id": m.group(1), "section": sec, "cells": dict(zip(header or [], cells(line))),
                    "line": n})
    return out


def needs_flag(row, open_ids):
    needs = row["cells"].get("Needs")
    if needs is None:
        return "-"
    blockers = [t for t in cb.TAG.findall(needs) if t in open_ids and t != row["id"]]
    if blockers:
        return "blocked:" + ",".join(blockers)
    text = plain(needs).strip()
    if text.startswith("PC"):
        return "PC" if re.fullmatch(r"PC\s*(\([^)]*\))?\s*", text) else "PC+"
    low = text.lower()
    if low.startswith(("panel", "the second panel")):
        return "panel"
    if low.startswith("part"):
        return "part"
    if low.startswith("service"):
        return "service"
    if low.startswith("maintainer"):
        return "maintainers"
    return "other"


def title(row):
    work = row["cells"].get("Work") or row["cells"].get("Question") or ""
    bold = BOLD.search(work)
    return plain(bold.group(1) if bold else work).strip().rstrip(":")


def details(row):
    return ", ".join(m.group(1) for m in LINK.finditer(row["cells"].get("Details", "")))


def cmd_list(args):
    all_rows = rows()
    open_ids = {r["id"] for r in all_rows}
    for r in all_rows:
        if r["section"][0] >= 8 and not args.all:
            continue
        flag = needs_flag(r, open_ids)
        if args.pc and flag not in ("PC", "PC+"):
            continue
        head = f"{r['id']} s{r['section'][0]} {flag:<8}"
        t = title(r)
        if len(t) > 72:
            t = t[:71] + "…"
        d = details(r)
        print(f"{head} {t}" + (f"  [{d[:40]}]" if d else ""))
    return 0


def boxes_for(ids):
    found = {i: [] for i in ids}
    for path in sorted((ROOT / "docs" / "features").rglob("*.md")):
        if path.name in cb.SKIP:
            continue
        rel = path.relative_to(ROOT).as_posix()
        for n, box, headings in cb.open_boxes(path):
            tags = cb.TAG.findall(box) or [t for h in headings for t in cb.TAG.findall(h)]
            for t in tags:
                if t in found:
                    where = " > ".join(h for h in headings[-2:])
                    found[t].append((rel, n, where, box.strip()))
    return found


def cmd_show(args):
    wanted = [i.upper() for i in args.ids]
    by_id = {r["id"]: r for r in rows()}
    open_ids = set(by_id)
    missing = [i for i in wanted if i not in by_id]
    for i in missing:
        print(f"{i}: not in docs/BACKLOG.md (finished rows are deleted; check `git log -S {i}`)")
    wrap = textwrap.TextWrapper(width=100, subsequent_indent="         ")
    found = boxes_for([i for i in wanted if i in by_id])
    for i in wanted:
        r = by_id.get(i)
        if r is None:
            continue
        print(f"{i} (section {r['section'][0]}: {r['section'][1]}; docs/BACKLOG.md:{r['line']})")
        for col, val in r["cells"].items():
            if col == "ID":
                continue
            if col == "Details":
                links = [f"{m.group(1)} -> docs/{m.group(2)}" for m in LINK.finditer(val)]
                print(wrap.fill(f"{col + ':':<8} " + "; ".join(links)) if links else f"{col}: {val}")
            else:
                print(wrap.fill(f"{col + ':':<8} {plain(val)}"))
        print(f"Flag:    {needs_flag(r, open_ids)}")
        boxes = found.get(i, [])
        print(f"Open boxes ({len(boxes)}):" if boxes else "Open boxes: none tagged with it")
        boxwrap = textwrap.TextWrapper(width=100, initial_indent="    ", subsequent_indent="      ")
        for rel, n, where, box in boxes:
            text = box if len(box) <= 700 else box[:697] + "..."
            print(f"  {rel}:{n}" + (f"  [{where}]" if where else ""))
            print(boxwrap.fill(text))
        print()
    return 1 if missing else 0


def cmd_next_id(_args):
    seen = set()
    sources = [BACKLOG, HANDOFF] + sorted((ROOT / "docs" / "features").rglob("*.md")) \
        + sorted((ROOT / "docs" / "bugs").glob("*.md")) + sorted(ARCHIVE.glob("*.md"))
    for p in sources:
        if p.exists():
            seen |= set(cb.TAG.findall(p.read_text(encoding="utf-8", errors="replace")))
    try:
        log = subprocess.run(["git", "-C", str(ROOT), "log", "-p", "--format=", "--all", "--",
                              "docs/BACKLOG.md"], capture_output=True, text=True,
                             encoding="utf-8", errors="replace", check=True).stdout
        seen |= set(cb.TAG.findall(log))
    except (OSError, subprocess.CalledProcessError):
        print("(git history not read: numbers from the working tree only)", file=sys.stderr)
    n = max((int(t[3:]) for t in seen), default=0) + 1
    print(f"BL-{n:03d}")
    return 0


def cmd_handoff(args):
    if not HANDOFF.exists():
        print("HANDOFF.local.md is missing (a fresh clone): docs/features/README.md "
              "'Current status' is the public summary")
        return 1
    lines = HANDOFF.read_text(encoding="utf-8").splitlines()
    print("\n".join(lines).rstrip())
    running = [(i, l) for i, l in enumerate(lines) if "IN PROGRESS" in l]
    for i, l in running:
        print(f"\n!! Unfinished work at HANDOFF.local.md:{i + 1}: {l.strip()[:100]} - resume or close it")
    if len(lines) > HANDOFF_MAX_LINES:
        print(f"\n!! HANDOFF.local.md has {len(lines)} lines (at most {HANDOFF_MAX_LINES}): rewrite it "
              "to what is true now and move the rest to its home (AGENTS.md \"Recording work\"); "
              "old pages go to local-archive/")
        return 2
    return 0


def git(*args, cwd=None, check=False):
    return subprocess.run(["git", "-C", str(cwd or ROOT), *args], capture_output=True, text=True,
                          encoding="utf-8", errors="replace", check=check)


def file_verdicts(path):
    """For each file a worktree changed: is that change already in the main tree?

    The worktree's diff against its own HEAD is split per file and tried against the main tree
    with `git apply --check` (which changes nothing), with one line of context so that main's
    later edits nearby don't hide it: the reverse applies -> main has it; the forward applies ->
    main doesn't; neither -> main changed that code too, someone has to read it."""
    import tempfile
    patch = git("diff", "HEAD", "--binary", cwd=path).stdout
    chunks = re.split(r"(?m)^(?=diff --git )", patch)
    result = {"in main": [], "NOT in main": [], "unclear": []}
    for chunk in chunks:
        m = re.match(r"diff --git a/(\S+)", chunk)
        if not m:
            continue
        with tempfile.NamedTemporaryFile("w", suffix=".patch", delete=False, encoding="utf-8",
                                         newline="\n") as f:
            f.write(chunk)
        try:
            if git("apply", "--check", "-R", "-C1", f.name).returncode == 0:
                result["in main"].append(m.group(1))
            elif git("apply", "--check", "-C1", f.name).returncode == 0:
                result["NOT in main"].append(m.group(1))
            else:
                result["unclear"].append(m.group(1))
        finally:
            Path(f.name).unlink(missing_ok=True)
    return result


WORKTREES = ROOT / ".claude" / "worktrees"     # where Claude Code puts agent worktrees
PATCHES = ARCHIVE / "worktree-patches"          # what --clean keeps of each one


def agent_worktrees():
    """(path, base, branch, locked) for each registered worktree under .claude/worktrees."""
    trees = []
    for block in git("worktree", "list", "--porcelain").stdout.strip().split("\n\n"):
        lines = block.splitlines()
        fields = dict(line.split(" ", 1) for line in lines if " " in line)
        path = fields.get("worktree", "")
        if "/.claude/worktrees/" in path.replace("\\", "/"):
            locked = any(line == "locked" or line.startswith("locked ") for line in lines)
            trees.append((path, fields.get("HEAD", "")[:7],
                          fields.get("branch", "").replace("refs/heads/", ""), locked))
    return trees


def changed_files(path):
    """The files a worktree changed or added (not ignored ones), as paths."""
    out = []
    for line in git("status", "--porcelain", "--untracked-files=all", cwd=path).stdout.splitlines():
        name = line[3:].split(" -> ")[-1].strip().strip('"')
        out.append(Path(path) / name)
    return out


def last_activity(path):
    """When an agent last worked in the worktree: the newest of its own git files (HEAD, index,
    written by git as it works) and the files it changed."""
    gitdir = Path(git("rev-parse", "--absolute-git-dir", cwd=path).stdout.strip() or path)
    stamps = [p.stat().st_mtime for p in (gitdir / "HEAD", gitdir / "index", gitdir / "gitdir")
              if p.exists()]
    stamps += [f.stat().st_mtime for f in changed_files(path) if f.exists()]
    return max(stamps, default=Path(path).stat().st_mtime)


def save_worktree(path, branch, stamp):
    """Keep what only this worktree has before it goes: its uncommitted changes (new files
    included) as one patch, and any commits its branch has that HEAD hasn't as format-patch
    files. Returns the paths written."""
    name = Path(path).name
    PATCHES.mkdir(parents=True, exist_ok=True)
    kept = []
    if changed_files(path):
        patch = PATCHES / f"{stamp}-{name}.patch"
        git("add", "-A", cwd=path, check=True)        # new files into the diff; the tree goes anyway
        git("diff", "--cached", "--binary", f"--output={patch}", "HEAD", cwd=path, check=True)
        kept.append(patch)
    if branch and git("rev-list", "--count", f"HEAD..{branch}").stdout.strip() not in ("", "0"):
        commits = PATCHES / f"{stamp}-{name}-commits"
        git("format-patch", "-q", "-o", str(commits), f"HEAD..{branch}", check=True)
        kept.append(commits)
    return kept


def long_path(path):
    r"""Windows refuses paths over 260 characters (an agent's build of ESP-IDF's examples has
    them) unless they carry the \\?\ prefix, whatever LongPathsEnabled says."""
    import os
    p = os.path.abspath(path)
    if os.name == "nt" and not p.startswith("\\\\?\\"):
        p = "\\\\?\\" + p
    return p


def remove_worktree(path):
    """git worktree remove, falling back to deleting the folder (read-only files and long paths
    stop git at times) and pruning git's record of it."""
    if git("-c", "core.longpaths=true", "worktree", "remove", "--force", path).returncode == 0:
        return
    import os
    import shutil
    import stat

    def writable(func, p, _exc):
        os.chmod(p, stat.S_IWRITE)
        func(p)
    if Path(path).exists():
        if sys.version_info >= (3, 12):
            shutil.rmtree(long_path(path), onexc=writable)
        else:
            shutil.rmtree(long_path(path), onerror=writable)
    git("worktree", "prune", check=True)


def clean_worktrees(args, trees):
    """Remove agent worktrees safely: what only the worktree holds is saved first (save_worktree),
    so nothing is lost and no one has to read 12 GB of build folders to say yes."""
    import time
    now = time.time()
    stamp = time.strftime("%Y-%m-%d")
    named = set(args.names)
    unknown = named - {Path(p).name for p, *_ in trees}
    for n in sorted(unknown):
        print(f"  {n}: not an agent worktree, skipped")
    removed = failed = 0
    for path, _head, branch, locked in trees:
        name = Path(path).name
        if named and name not in named:
            continue
        idle_h = (now - last_activity(path)) / 3600
        if locked and name not in named:
            print(f"  {name}: locked, skipped (name it to remove it anyway)")
            continue
        if not named and idle_h < args.older_than:
            print(f"  {name}: active {idle_h:.1f} h ago, skipped (an agent may still be working "
                  f"in it; name it, or wait {args.older_than:g} h)")
            continue
        if args.dry_run:
            print(f"  {name}: would save its changes and remove it and branch {branch or '-'}")
            continue
        try:
            kept = save_worktree(path, branch, stamp)
            remove_worktree(path)
            if branch:
                git("branch", "-D", branch, check=True)
        except (OSError, subprocess.CalledProcessError) as e:
            detail = getattr(e, "stderr", "") or e
            print(f"  {name}: NOT removed - {str(detail).strip()[:200]} (anything saved is in "
                  f"{PATCHES.relative_to(ROOT).as_posix()})")
            failed += 1
            continue
        what = ", ".join(p.relative_to(ROOT).as_posix() for p in kept) or "nothing to keep"
        print(f"  {name}: removed with branch {branch or '-'} (kept: {what})")
        removed += 1
    # Folders without a registered worktree: an empty one Claude Code left, or the rest of a
    # removal that failed after its changes were saved (finished now); anything else is a person's.
    if WORKTREES.is_dir() and not args.dry_run:
        registered = {Path(p).resolve() for p, *_ in agent_worktrees()}
        for d in sorted(WORKTREES.iterdir()):
            if not d.is_dir() or d.resolve() in registered or (named and d.name not in named):
                continue
            saved = sorted(PATCHES.glob(f"*-{d.name}.patch")) + sorted(PATCHES.glob(f"*-{d.name}-commits"))
            if not any(d.iterdir()):
                d.rmdir()
                print(f"  {d.name}: empty folder removed")
            elif saved:
                try:
                    remove_worktree(str(d))
                except OSError as e:
                    print(f"  {d.name}: rest of an earlier removal NOT deleted - {e}")
                    failed += 1
                    continue
                branch = f"worktree-{d.name}"
                if git("rev-parse", "--verify", "--quiet", f"refs/heads/{branch}").returncode == 0:
                    git("branch", "-D", branch, check=True)
                print(f"  {d.name}: finished an earlier removal (kept: "
                      f"{saved[-1].relative_to(ROOT).as_posix()})")
                removed += 1
            else:
                print(f"  {d.name}: a folder with files but no worktree - left for a person")
    git("worktree", "prune")
    print(f"{removed} worktree(s) removed" + (f", {failed} failed" if failed else "")
          + (" (dry run)" if args.dry_run else ""))
    return 1 if failed else 0


def cmd_worktrees(args):
    """Agent worktrees under .claude/worktrees: their age, and how much of their work main has;
    with --clean, remove them safely."""
    import time
    trees = agent_worktrees()
    if args.clean:
        return clean_worktrees(args, trees)
    if not trees:
        print("No agent worktrees.")
        return 0
    now = time.time()
    print(f"{len(trees)} agent worktree(s). Once a worktree's work is applied (or dropped), "
          f"`backlog.py worktrees --clean <name>` saves what it holds to "
          f"local-archive/worktree-patches and removes it with its branch:")
    for path, head, branch, locked in trees:
        name = Path(path).name
        status = git("status", "--porcelain", cwd=path).stdout.splitlines()
        untracked = sum(1 for s in status if s.startswith("??"))
        tracked = len(status) - untracked
        idle = (now - last_activity(path)) / 3600
        extra = f", {untracked} untracked" if untracked else ""
        lock = ", locked" if locked else ""
        print(f"  {name}  base {head}  idle {idle:.1f} h  {tracked} changed{extra}{lock}"
              f"  branch {branch}")
        if tracked and not args.quick:
            v = file_verdicts(path)
            print(f"    files: {len(v['in main'])} in main, {len(v['NOT in main'])} NOT in main, "
                  f"{len(v['unclear'])} unclear")
            for key in ("NOT in main", "unclear"):
                shown = v[key][:args.files]
                if shown:
                    more = len(v[key]) - len(shown)
                    print(f"    {key}: " + ", ".join(shown) + (f" (+{more})" if more > 0 else ""))
    return 0


STATE = ROOT / ".git" / "backlog-agents"


def dirty_paths():
    """Paths that differ from HEAD (modified, added, deleted, untracked), outside .claude/."""
    out = git("status", "--porcelain", "-uall", "-z").stdout.split("\0")
    paths, skip = [], False
    for entry in out:
        if skip:
            skip = False
            continue
        if len(entry) < 4:
            continue
        code, path = entry[:2], entry[3:]
        if code[0] in "RC":
            skip = True
        if not path.startswith(".claude/"):
            paths.append(path)
    return paths


def read_bytes(path):
    p = ROOT / path
    return p.read_bytes() if p.is_file() else None


def head_bytes(path):
    r = subprocess.run(["git", "-C", str(ROOT), "show", f"HEAD:{path}"], capture_output=True)
    return r.stdout if r.returncode == 0 else None


def cmd_mark(args):
    """Remember the state of every dirty file, so `changes` can show one unit's own edits."""
    import json
    import shutil
    base = STATE / args.label
    shutil.rmtree(base, ignore_errors=True)
    manifest = {}
    for path in dirty_paths():
        data = read_bytes(path)
        manifest[path] = data is not None
        if data is not None:
            dest = base / "files" / path
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)
    base.mkdir(parents=True, exist_ok=True)
    (base / "manifest.json").write_text(json.dumps(manifest, indent=0), encoding="utf-8")
    print(f"marked '{args.label}': {len(manifest)} dirty file(s) remembered")
    return 0


def cmd_changes(args):
    """What changed since `mark <label>`: this unit's edits only, even in a dirty tree."""
    import difflib
    import json
    base = STATE / args.label
    mf = base / "manifest.json"
    if not mf.exists():
        print(f"no mark '{args.label}' (run `mark {args.label}` before the unit starts)")
        return 1
    manifest = json.loads(mf.read_text(encoding="utf-8"))
    total_add = total_del = 0
    shown = 0
    for path in sorted(set(manifest) | set(dirty_paths())):
        if path in manifest:
            before = (base / "files" / path).read_bytes() if manifest[path] else None
        else:
            before = head_bytes(path)
        after = read_bytes(path)
        if before == after:
            continue
        shown += 1
        kind = "new" if before is None else "deleted" if after is None else "changed"
        try:
            a = (before or b"").decode("utf-8").splitlines()
            b = (after or b"").decode("utf-8").splitlines()
        except UnicodeDecodeError:
            print(f"  {kind:<7} (binary)  {path}")
            continue
        diff = list(difflib.unified_diff(a, b, f"a/{path}", f"b/{path}", lineterm="", n=2))
        add = sum(1 for d in diff if d.startswith("+") and not d.startswith("+++"))
        rem = sum(1 for d in diff if d.startswith("-") and not d.startswith("---"))
        total_add, total_del = total_add + add, total_del + rem
        print(f"  {kind:<7} +{add:<5} -{rem:<5} {path}")
        if args.diff:
            print("\n".join(diff))
    print(f"{shown} file(s) changed since '{args.label}': +{total_add} -{total_del}")
    return 0


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list", help="one line per row")
    p.add_argument("--pc", action="store_true", help="only rows with PC work that can start now")
    p.add_argument("--all", action="store_true", help="include 'Later and ideas' and 'On hold'")
    p.set_defaults(func=cmd_list)
    p = sub.add_parser("show", help="rows in full with their open boxes")
    p.add_argument("ids", nargs="+", metavar="BL-NNN")
    p.set_defaults(func=cmd_show)
    p = sub.add_parser("next-id", help="the next free BL number")
    p.set_defaults(func=cmd_next_id)
    p = sub.add_parser("mark", help="remember the dirty files before a unit starts")
    p.add_argument("label", help="e.g. BL-066")
    p.set_defaults(func=cmd_mark)
    p = sub.add_parser("changes", help="the files a unit changed since its mark")
    p.add_argument("label")
    p.add_argument("--diff", action="store_true", help="print the unit's own diff too")
    p.set_defaults(func=cmd_changes)
    p = sub.add_parser("worktrees", help="agent worktrees and whether main has their changes")
    p.add_argument("--files", type=int, default=4, help="file names to show per verdict")
    p.add_argument("--quick", action="store_true", help="skip the per-file verdicts")
    p.add_argument("--clean", action="store_true",
                   help="save each worktree's changes to local-archive/worktree-patches, then "
                        "remove it and its branch")
    p.add_argument("--older-than", type=float, default=12, metavar="HOURS",
                   help="with --clean and no names: only worktrees idle this long (default 12)")
    p.add_argument("--dry-run", action="store_true", help="with --clean: say what would happen")
    p.add_argument("names", nargs="*", metavar="NAME",
                   help="with --clean: just these worktrees (agent-...), however recent")
    p.set_defaults(func=cmd_worktrees)
    p = sub.add_parser("handoff", help="your status page (HANDOFF.local.md), with a size check")
    p.set_defaults(func=cmd_handoff)
    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
