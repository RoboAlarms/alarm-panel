#!/usr/bin/env python3
"""quick_check.py - the checks CI runs first, in seconds: run it before a push.

Generated code against its source (the UI layouts, images, the rail logo, the Zigbee
signatures), the UI contract, the component tiers, the board manifests, the backlog and the
translation catalogs. CI's "quick checks" job runs this same script (.github/workflows/
host-tests.yml), so a pass here is a pass there; the unit tests run beside it, and the preview
walks and firmware builds nightly or by hand (full-suite.yml, firmware.yml; CONTRIBUTING.md
"Build and test"). Every check runs even when one fails; the exit code is 1 if
any did.

    python tools/quick_check.py            # the quick checks (about 30 s)
    python tools/quick_check.py --tools    # and the tool tests, tools/tests (several minutes)
"""
import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PY = sys.executable


def checks(with_tools):
    """(name, command) in the order CI shows them."""
    images = sorted(str(p.relative_to(ROOT)) for p in (ROOT / "components/ui/alarm_ui/images").glob("*.jpg"))
    # The 4" layout (480x320) is on hold with docs/features/19: see host-tests.yml.
    out = [
        ("Generated UI code matches the XML (1024x600)",
         [PY, "tools/lvpro_gen.py", "components/ui/alarm_ui/layouts/1024x600/alarm_ui", "--check"]),
        ("The layouts not on hold offer what the UI code uses (CYD-007)", [PY, "tools/check_ui_contract.py"]),
        ("Embedded images match their JPEGs", [PY, "tools/embed_image.py", "--check", *images]),
        ("Embedded rail logo matches its alpha mask", [PY, "tools/embed_logo.py", "--check"]),
        ("Generated Zigbee device signatures match their JSON rows", [PY, "tools/zb_signatures.py", "--check"]),
        ("Component tiers stay one-way (docs/features/33)", [PY, "tools/check_architecture.py"]),
        ("Every board manifest is complete and consistent", [PY, "tools/check_boards.py"]),
        ("Every open task in docs/features has its backlog row", [PY, "tools/check_backlog.py"]),
        ("The translation template matches the source", [PY, "tools/i18n.py", "extract", "--check"]),
        ("Words are marked and catalogs are well formed", [PY, "tools/i18n.py", "check"]),
        ("The generated catalogs match the .po files", [PY, "tools/i18n.py", "build", "--check"]),
    ]
    if with_tools:
        out.append(("Tool regressions (tools/tests)", [PY, "-m", "unittest", "discover", "-s", "tools/tests"]))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--tools", action="store_true", help="also run the tool tests (several minutes)")
    args = ap.parse_args()
    ci = bool(os.environ.get("GITHUB_ACTIONS"))
    failed = []
    start = time.monotonic()
    for name, cmd in checks(args.tools):
        if ci:
            print(f"::group::{name}", flush=True)
        else:
            print(f"--- {name}", flush=True)
        t = time.monotonic()
        rc = subprocess.run(cmd, cwd=ROOT).returncode
        if ci:
            print("::endgroup::", flush=True)
        status = "ok" if rc == 0 else f"FAILED (exit {rc})"
        print(f"    {status}, {time.monotonic() - t:.1f} s", flush=True)
        if rc != 0:
            failed.append(name)
            if ci:
                print(f"::error::{name} failed: {' '.join(cmd[1:])}", flush=True)
    total = time.monotonic() - start
    if failed:
        print(f"\n{len(failed)} check(s) failed in {total:.0f} s:")
        for name in failed:
            print(f"  - {name}")
        return 1
    print(f"\nAll quick checks passed in {total:.0f} s.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
