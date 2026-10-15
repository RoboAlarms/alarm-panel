#!/usr/bin/env python3
"""The panel's UI mockup (docs/features/07-UI-LVGL-Pro/mockup): build it and check it.

    python tools/ui_mockup.py build [--shots]   # -> build/mockups/panel/index.html
    python tools/ui_mockup.py check [--shots-dir build/ui_preview/shots]
    python tools/ui_mockup.py --mock docs/features/07-UI-LVGL-Pro/mockup-next build   # a copy, to its own folder

build writes one HTML file that opens on its own (fonts, icons, logo and screen-saver photo
inlined). The tokens (colours, spaces, radii, layout sizes) and the font classes are generated
from the 7" layout's globals.xml, and the fonts are the layout's own TTFs, so the mockup can't
drift from the panel's design system. --shots copies the PC preview's pictures
(tools/ui_preview.ps1) next to it, for the viewer's "compare with the panel" modes.

check fails when a screen's id is used twice, a screen names a preview picture that doesn't
exist (when the preview's pictures are there), a proposal is malformed or replaces a screen that
doesn't exist, an icon isn't a Lucide name, a screen or proposal writes its own colour instead of
a token, or the kit's CSS uses a token globals.xml doesn't define. It also lists the preview
pictures no screen covers yet (information, not a failure).

The panel's appearances are the layout's themes.xml (BL-132, docs/features/07 UI-010; the one
copy, tools/lvpro_gen.py builds them into the firmware): a mockup whose viewer has the Appearance
switch (its viewer.html has the THEMES slot) recolours the screens with them, and check fails a
theme that misses a colour or whose words don't reach WCAG 2.x contrast on the backgrounds they
sit on.
"""
from __future__ import annotations

import argparse
import base64
import datetime as dt
import json
import re
import shutil
import struct
import sys
import xml.etree.ElementTree as ET
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MOCK = ROOT / "docs/features/07-UI-LVGL-Pro/mockup"
LAYOUT = ROOT / "components/ui/alarm_ui/layouts/1024x600/alarm_ui"
IMAGES = ROOT / "components/ui/alarm_ui/images"
OUT = ROOT / "build/mockups/panel"
SHOTS_DIR = ROOT / "build/ui_preview/shots"
# The gallery's order; any other file in screens/ follows, by name.
SCREEN_ORDER = ["home.js", "views.js", "settings.js", "installer.js", "setup.js"]
STATUSES = {"draft", "approved", "rejected", "built"}
# CSS custom properties the kit defines itself rather than globals.xml.
OWN_VARS = {"logo", "saver"}


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8")


def globals_xml() -> ET.Element:
    return ET.fromstring(read(LAYOUT / "globals.xml"))


def tokens(g: ET.Element) -> dict[str, str]:
    """globals.xml's consts as CSS values, keyed by their XML names."""
    out = {}
    for c in g.find("consts"):
        name, value = c.get("name"), c.get("value")
        if c.tag == "int":
            out[name] = f"{value}px" if not name.startswith("radius_full") else "999px"
        elif c.tag == "color":
            out[name] = "#" + value[2:].upper()
        elif c.tag == "opa":
            out[name] = str(int(value.rstrip("%")) / 100)
    return out


def themes() -> dict[str, dict]:
    """The panel's appearances (the layout's themes.xml), for a mockup whose viewer has the
    Appearance switch: name -> {title, help, high (contrast="high"), saver (the theme the screen
    saver keeps), colors {token: '#RRGGBB'}}."""
    p = LAYOUT / "themes.xml"
    if not p.exists() or "/*@THEMES@*/" not in read(MOCK / "viewer/viewer.html"):
        return {}
    return {t.get("name"): {"title": t.get("title") or t.get("name"), "help": t.get("help") or "",
                            "high": t.get("contrast") == "high", "saver": t.get("saver") or "",
                            "colors": {c.get("name"): "#" + c.get("value")[2:].upper() for c in t.findall("color")}}
            for t in ET.fromstring(read(p)).findall("theme")}


def theme_css(th: dict[str, dict]) -> str:
    """.theme_<name> gives what is inside it the theme's colours; the screen saver, a dark photo in
    every appearance, keeps the colours of the theme its theme names in saver=."""
    out = []
    for n, t in th.items():
        out.append(f".theme_{n} {{ " + " ".join(f"--{k}: {v};" for k, v in t["colors"].items()) + " }")
        if t["saver"] in th:
            out.append(f".theme_{n} .saver {{ " + " ".join(f"--{k}: {v};" for k, v in th[t["saver"]]["colors"].items()) + " }")
    return "\n".join(out)


def contrast(a: str, b: str) -> float:
    """WCAG 2.x contrast ratio of two '#RRGGBB' colours (1 to 21)."""
    def lum(h: str) -> float:
        rgb = [int(h[i:i + 2], 16) / 255 for i in (1, 3, 5)]
        lin = [c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4 for c in rgb]
        return 0.2126 * lin[0] + 0.7152 * lin[1] + 0.0722 * lin[2]
    hi, lo = sorted((lum(a), lum(b)), reverse=True)
    return (hi + 0.05) / (lo + 0.05)


TONES = ["green", "amber", "blue", "violet", "rose", "orange", "red"]


def contrast_problems(theme: dict) -> list[str]:
    """Where a theme's words don't read: every text colour and tone on the backgrounds words sit on,
    words on a tone (a key, a switch), words on the alarm's backgrounds, the keyboard's special keys
    and color_pressed (a held key, a toast). 4.5:1 is WCAG AA for text; a high-contrast theme asks
    7:1 (AAA) and outlines of 3:1 (non-text contrast)."""
    c, need = theme["colors"], 7.0 if theme["high"] else 4.5
    pairs = [(f, b) for f in ["color_text", "color_text_2", "color_text_3"] + [f"color_{t}" for t in TONES]
             for b in ["color_bg", "color_surface", "color_raised"]]
    pairs += [("color_on_tone", f"color_{t}") for t in TONES]
    pairs += [("color_text", b) for b in ["color_alarm_bg", "color_alarm_card"] if b in c]
    # The special keys' symbols sit on their own colour, or on color_line in a theme without one (today).
    pairs += [("color_text_2", "color_key_special" if "color_key_special" in c else "color_line")]
    pairs += [("color_text", "color_pressed"), ("color_text_2", "color_pressed")]
    out = [f"{f} on {b} is {contrast(c[f], c[b]):.2f}:1, under {need:g}:1"
           for f, b in pairs if f in c and b in c and contrast(c[f], c[b]) < need]
    if theme["high"]:
        out += [f"color_line on {b} is {contrast(c['color_line'], c[b]):.2f}:1, under 3:1: outlines must show"
                for b in ["color_bg", "color_surface"] if contrast(c["color_line"], c[b]) < 3]
    return out


def fonts(g: ET.Element) -> list[dict]:
    return [{"name": f.get("name"), "file": f.get("src_path").split("/")[-1], "size": int(f.get("size")),
             "range": f.get("range", "")} for f in g.find("fonts")]


def png_rgba(width: int, height: int, rgba: bytes) -> bytes:
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    rows = b"".join(b"\0" + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def logo_png() -> bytes:
    """The rail's logo: its alpha mask (images/roboalarms_mark.pgm) as a white PNG."""
    data = (IMAGES / "roboalarms_mark.pgm").read_bytes()
    parts = data.split(maxsplit=4)  # P5, width, height, maxval, pixels
    w, h, pixels = int(parts[1]), int(parts[2]), parts[4]
    return png_rgba(w, h, b"".join(b"\xff\xff\xff" + bytes([a]) for a in pixels[:w * h]))


def lucide() -> dict[str, str]:
    return json.loads(read(MOCK / "kit/lucide-codepoints.json"))


def panel_icon_names(g: ET.Element, names: dict[str, str]) -> list[str]:
    """The Lucide names whose glyphs are in the panel's icon fonts (icons_20 ... icons_96)."""
    cps = set()
    for f in fonts(g):
        if f["name"].startswith("icons_"):
            cps |= {int(x, 16) for x in f["range"].split(",") if x}
    return sorted(n for n, cp in names.items() if int(cp, 16) in cps)


def data_url(mime: str, raw: bytes) -> str:
    return f"data:{mime};base64,{base64.b64encode(raw).decode()}"


def screen_files() -> list[Path]:
    files = sorted((MOCK / "screens").glob("*.js"), key=lambda p: (SCREEN_ORDER.index(p.name) if p.name in SCREEN_ORDER else 99, p.name))
    return files + sorted(p for p in (MOCK / "proposals").glob("*.js") if not p.name.startswith("_"))


def shot_index(shots_dir: Path) -> dict[str, str]:
    """'home_ready' -> '01_home_ready.png' for every preview picture."""
    if not shots_dir.is_dir():
        return {}
    return {re.sub(r"^\d+_", "", p.stem): p.name for p in sorted(shots_dir.glob("*.png"))}


def build(args: argparse.Namespace) -> int:
    g = globals_xml()
    tok = tokens(g)
    names = lucide()
    fs = fonts(g)
    css_vars = [f"  --{k}: {v};" for k, v in tok.items()]
    th = themes()
    if th:  # a colour a theme adds (a literal of the XML's today) starts as the first theme's
        css_vars += [f"  --{k}: {v};" for k, v in next(iter(th.values()))["colors"].items() if k not in tok]
    face, classes = [], []
    for file in sorted({f["file"] for f in fs}):
        family = Path(file).stem
        face.append(f"@font-face {{ font-family: '{family}'; src: url({data_url('font/ttf', (LAYOUT / 'fonts' / file).read_bytes())}) format('truetype'); }}")
    for f in fs:
        family = Path(f["file"]).stem
        # LVGL's own line height (lv_font_conv's, in the generated font): lines then sit where the panel's do.
        lh = re.search(r"\.line_height = (\d+)", read(LAYOUT / "fonts" / f"{f['name']}_data.c")).group(1)
        css_vars += [f"  --{f['name']}-family: '{family}';", f"  --{f['name']}-size: {f['size']}px;",
                     f"  --{f['name']}-line: {lh}px;"]
        classes.append(f".{f['name']} {{ font-family: '{family}'; font-size: {f['size']}px; line-height: {lh}px; font-weight: 400; }}")
    css_vars += [f"  --logo: url({data_url('image/png', logo_png())});",
                 f"  --saver: url({data_url('image/jpeg', (IMAGES / 'saver_forest.jpg').read_bytes())});"]
    token_css = ":root {\n" + "\n".join(css_vars) + "\n}\n" + "\n".join(classes) + "\n" + theme_css(th)

    shots = {}
    if args.shots:
        idx = shot_index(SHOTS_DIR)
        if not idx:
            print(f"no preview pictures in {SHOTS_DIR.relative_to(ROOT)}: run tools/ui_preview.ps1 first", file=sys.stderr)
            return 1
        (OUT / "shots").mkdir(parents=True, exist_ok=True)
        for name, file in idx.items():
            shutil.copyfile(SHOTS_DIR / file, OUT / "shots" / file)
            shots[name] = f"shots/{file}"

    def js(text: str) -> str:
        return text.replace("</script", "<\\/script")
    screens = "\n".join(f"// ---- {p.relative_to(MOCK).as_posix()} ----\n(() => {{\n{read(p)}\n}})();" for p in screen_files())
    page = read(MOCK / "viewer/viewer.html")
    for key, value in {
        "/*@FONTS@*/": "\n".join(face), "/*@TOKENS@*/": token_css, "/*@PANEL_CSS@*/": read(MOCK / "kit/panel.css"),
        "/*@VIEWER_CSS@*/": read(MOCK / "viewer/viewer.css"), "/*@GUIDE@*/": read(MOCK / "viewer/guide.html"),
        "/*@ICONS@*/": json.dumps(names, separators=(",", ":")),
        "/*@PANEL_ICONS@*/": json.dumps(panel_icon_names(g, names)),
        "/*@SHOTS@*/": json.dumps(shots), "/*@BUILT@*/": json.dumps(dt.date.today().isoformat()),
        "/*@THEMES@*/": json.dumps([{"name": n, "title": t["title"], "help": t["help"]} for n, t in th.items()]),
        "/*@KIT@*/": js(read(MOCK / "kit/kit.js")), "/*@SCREENS@*/": js(screens),
        "/*@VIEWER_JS@*/": js(read(MOCK / "viewer/viewer.js")),
    }.items():
        page = page.replace(key, value)
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "index.html").write_text(page, encoding="utf-8", newline="\n")
    print(f"wrote {(OUT / 'index.html').relative_to(ROOT).as_posix()} ({len(page) // 1024} KB)"
          + (f", {len(shots)} preview pictures" if shots else ""))
    return 0


ID_RE = re.compile(r"\bid: '([^']+)'")
SHOT_RE = re.compile(r"\bshot: (?:'([^']+)'|null)")
GLYPH_RE = re.compile(r"\b(?:glyph|trail|wifi|enter): '([a-z0-9-]*)'|M\.icon\('([a-z0-9-]+)'")
HEX_RE = re.compile(r"#[0-9A-Fa-f]{3,8}\b|rgba?\(")
THEME_RE = re.compile(r"\btheme: '([^']*)'")


def check(args: argparse.Namespace) -> int:
    errors, info = [], []
    names = lucide()
    tok = tokens(globals_xml())
    font_names = {f["name"] for f in fonts(globals_xml())}
    th = themes()
    if th:
        colors = {k for k, v in tok.items() if k.startswith("color_")} | {k for t in th.values() for k in t["colors"]}
        for n, t in th.items():
            if set(t["colors"]) != colors:
                errors.append(f"themes.xml: the theme '{n}' has no {', '.join(sorted(colors - set(t['colors'])))}")
                continue
            if t["saver"] and t["saver"] not in th:
                errors.append(f"themes.xml: the theme '{n}' keeps the saver in '{t['saver']}', which is no theme")
            errors += [f"themes.xml: the theme '{n}': {p}" for p in contrast_problems(t)]
        tok = {**tok, **{k: "" for k in colors}}
    shots = shot_index(Path(args.shots_dir))
    seen: dict[str, str] = {}
    used_shots = set()
    for p in screen_files():
        rel = p.relative_to(ROOT).as_posix()
        text = read(p)
        is_proposal = p.parent.name == "proposals"
        for m in ID_RE.finditer(text):
            if m.group(1) in seen:
                errors.append(f"{rel}: the id '{m.group(1)}' is also used in {seen[m.group(1)]}")
            seen[m.group(1)] = rel
        for m in SHOT_RE.finditer(text):
            if m.group(1):
                if not is_proposal:
                    used_shots.add(m.group(1))
                if shots and m.group(1) not in shots:
                    errors.append(f"{rel}: no preview picture called '{m.group(1)}' in {args.shots_dir}")
        for m in GLYPH_RE.finditer(text):
            name = m.group(1) or m.group(2)
            if name and name not in names:
                errors.append(f"{rel}: '{name}' is not a Lucide icon name (kit/lucide-codepoints.json)")
        for n, line in enumerate(text.splitlines(), 1):
            if HEX_RE.search(line):
                errors.append(f"{rel}:{n}: a colour of its own; use a tone or a token (kit/panel.css, globals.xml)")
        for m in THEME_RE.finditer(text):
            if m.group(1) not in th:
                errors.append(f"{rel}: theme '{m.group(1)}' is not one of themes.xml's ({', '.join(th) or 'it has none'})")
        if is_proposal:
            if "proposal:" not in text:
                errors.append(f"{rel}: a proposal's screens need proposal: {{bl, status, why}}")
            for st in re.findall(r"\bstatus: '([^']+)'", text):
                if st not in STATUSES:
                    errors.append(f"{rel}: status '{st}' is not one of {', '.join(sorted(STATUSES))}")
            if not re.search(r"\bbl: 'BL-\d{3}'", text):
                errors.append(f"{rel}: the proposal names no backlog row (bl: 'BL-NNN')")
    for p in screen_files():
        for r in re.findall(r"\breplaces: '([^']+)'", read(p)):
            if r not in seen:
                errors.append(f"{p.relative_to(ROOT).as_posix()}: replaces '{r}', which is no screen's id")
    css = read(MOCK / "kit/panel.css") + read(MOCK / "kit/kit.js") + "".join(read(p) for p in screen_files())
    # A name followed by "-" is a font's (checked below), by "$" one kit.js puts together itself.
    for v, after in sorted(set(re.findall(r"var\(--([a-z_0-9]+)([-$]?)", css))):
        if not after and v not in tok and v not in OWN_VARS:
            errors.append(f"a var(--{v}) that globals.xml doesn't define")
    for v in sorted(set(re.findall(r"var\(--(font_[a-z]+)-(?:family|size|line)\)", css))):
        if v not in font_names:
            errors.append(f"a font var(--{v}-...) that globals.xml doesn't define")
    if shots:
        missing = [f for n, f in shots.items() if n not in used_shots]
        info.append(f"{len(shots) - len(missing)} of {len(shots)} preview pictures have a mockup screen")
    else:
        info.append(f"no preview pictures in {args.shots_dir}: their names weren't checked")
    for e in errors:
        print(f"error: {e}")
    for i in info:
        print(i)
    print(f"{len(seen)} screens, {len(errors)} problems")
    return 1 if errors else 0


def png_pixels(path: Path) -> tuple[int, int, int, bytes]:
    """width, height, bytes per pixel and the raw pixels of an 8-bit RGB, RGBA or palette PNG
    (a palette PNG comes back as RGB)."""
    data = path.read_bytes()
    pos, idat, w, plte = 8, b"", 0, b""
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
            if depth != 8 or ctype not in (2, 3, 6):
                raise ValueError(f"{path.name}: only 8-bit RGB, RGBA or palette PNGs")
            bpp = {2: 3, 3: 1, 6: 4}[ctype]
        elif kind == b"PLTE":
            plte = body
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    raw, stride = zlib.decompress(idat), w * bpp
    out, prev = bytearray(), bytearray(stride)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if f == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 255
        elif f == 2:
            line = bytearray((a + b) & 255 for a, b in zip(line, prev))
        elif f == 3:
            for i in range(stride):
                line[i] = (line[i] + ((line[i - bpp] if i >= bpp else 0) + prev[i]) // 2) & 255
        elif f == 4:
            for i in range(stride):
                a, b, c = (line[i - bpp] if i >= bpp else 0), prev[i], (prev[i - bpp] if i >= bpp else 0)
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out += line
        prev = line
    if bpp == 1:
        return w, h, 3, b"".join(plte[i * 3:i * 3 + 3] for i in out)
    return w, h, bpp, bytes(out)


def find_browser(given: str | None) -> str | None:
    for c in [given, *(str(Path(p)) for p in (
            r"C:\Program Files\Google\Chrome\Application\chrome.exe",
            r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
            "/opt/pw-browsers/chromium", "/usr/bin/chromium", "/usr/bin/google-chrome",
            "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"))]:
        if c and Path(c).exists():
            return c
    return shutil.which("chromium") or shutil.which("google-chrome")


def compare(args: argparse.Namespace) -> int:
    """Draw each screen with a headless Chrome and count the pixels that differ from its preview picture."""
    import subprocess
    import tempfile
    page = OUT / "index.html"
    browser = find_browser(args.browser)
    if not page.exists() or not browser:
        print("build the mockup first, and give --browser (Chrome or Edge)" if browser else "no Chrome or Edge found: --browser", file=sys.stderr)
        return 1
    shots = shot_index(SHOTS_DIR)
    ids = []
    for p in screen_files():
        if p.parent.name == "proposals":
            continue
        text = read(p)
        for m in re.finditer(r"\bid: '([^']+)'[^\n]*?\bshot: '([^']+)'", text):
            if (not args.ids or m.group(1) in args.ids) and m.group(2) in shots:
                ids.append((m.group(1), m.group(2)))
    rows = []
    with tempfile.TemporaryDirectory() as tmp:
        for sid, shot in ids:
            out = Path(tmp) / f"{sid}.png"
            subprocess.run([browser, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--force-device-scale-factor=1",
                            "--window-size=1024,600", "--virtual-time-budget=3000", f"--screenshot={out}",
                            f"{page.as_uri()}?bare#{sid}"], capture_output=True, timeout=60)
            if args.keep:
                Path(args.keep).mkdir(parents=True, exist_ok=True)
                shutil.copyfile(out, Path(args.keep) / f"{sid}.png")
            w1, h1, b1, a = png_pixels(out)
            w2, h2, b2, b = png_pixels(SHOTS_DIR / shots[shot])
            if (w1, h1) != (w2, h2):
                rows.append((100.0, sid, f"size {w1}x{h1} against {w2}x{h2}"))
                continue
            differ = sum(1 for i in range(w1 * h1)
                         if max(abs(a[i * b1] - b[i * b2]), abs(a[i * b1 + 1] - b[i * b2 + 1]), abs(a[i * b1 + 2] - b[i * b2 + 2])) > 48)
            rows.append((100 * differ / (w1 * h1), sid, shots[shot]))
    for pct, sid, what in sorted(rows, reverse=True):
        print(f"{pct:5.1f} %  {sid:28} {what}")
    print(f"{len(rows)} screens; pixels that differ clearly (one channel off by more than 48 of 255)")
    return 0


def main() -> int:
    global MOCK, OUT
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--mock", help="another copy of the mockup (a redesign); builds to build/mockups/panel<its folder's suffix>")
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="write build/mockups/panel/index.html")
    b.add_argument("--shots", action="store_true", help="copy the PC preview's pictures beside it, to compare")
    c = sub.add_parser("check", help="check the screens and proposals")
    c.add_argument("--shots-dir", default=str(SHOTS_DIR), help="the PC preview's pictures")
    k = sub.add_parser("compare", help="score each screen against its preview picture (needs Chrome or Edge)")
    k.add_argument("ids", nargs="*", help="only these screens")
    k.add_argument("--browser", help="Chrome, Edge or Chromium")
    k.add_argument("--keep", help="keep the mockup's pictures in this folder")
    args = ap.parse_args()
    if args.mock:
        MOCK = Path(args.mock).resolve()
        OUT = ROOT / "build/mockups" / ("panel" + MOCK.name.removeprefix("mockup"))
    return {"build": build, "check": check, "compare": compare}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
